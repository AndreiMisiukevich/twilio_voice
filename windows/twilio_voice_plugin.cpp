// This must be included before many other Windows headers.
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <propkey.h>
#include <Functiondiscoverykeys_devpkey.h>

#include "twilio_voice_plugin.h"
#include "js_interop/call/tv_error.h"
#include "js_interop/call/tv_call_status.h"
#include "utils/tv_logger.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <future>
#include <thread>

using json = nlohmann::json;

namespace twilio_voice
{

  void TwilioVoicePlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarWindows *registrar)
  {
    auto plugin = std::make_unique<TwilioVoicePlugin>(registrar);
    registrar->AddPlugin(std::move(plugin));
  }

  TwilioVoicePlugin::TwilioVoicePlugin(flutter::PluginRegistrarWindows *registrar)
      : registrar_(registrar)
  {
    channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "twilio_voice/messages",
        &flutter::StandardMethodCodec::GetInstance());

    event_channel_ = std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
        registrar->messenger(), "twilio_voice/events",
        &flutter::StandardMethodCodec::GetInstance());

    auto handler = std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
        [this](const flutter::EncodableValue *arguments,
               std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> &&events) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
        {
          event_sink_ = events.release();
          return nullptr;
        },
        [this](const flutter::EncodableValue *arguments) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
        {
          if (event_sink_)
          {
            delete event_sink_;
            event_sink_ = nullptr;
          }
          return nullptr;
        });
    stream_handler_ = std::move(handler);
    event_channel_->SetStreamHandler(std::move(stream_handler_));

    TVLogger::getInstance().setMethodChannel(channel_.get());

    channel_->SetMethodCallHandler(
        [this](const auto &call, auto result)
        {
          HandleMethodCall(call, std::move(result));
        });

    InitializeWebView();
  }

  void TwilioVoicePlugin::InitializeWebView()
  {
    HWND hwnd = registrar_->GetView()->GetNativeWindow();
    webview_ = std::make_unique<TVWebView>(hwnd);

    webview_->initialize([this]()
                         {
    wchar_t module_path[MAX_PATH];
    GetModuleFileNameW(NULL, module_path, MAX_PATH);
    std::wstring path(module_path);
    path = path.substr(0, path.find_last_of(L"\\/"));
    path = path.substr(0, path.find_last_of(L"\\/"));
    std::wstring assets_path = path + L"\\Debug\\assets";
    std::wstring html_path = assets_path + L"\\index.html";
        
    webview_->loadFile(html_path, [this]() {
      

      webview_->evaluateJavaScript(
        L"(() => {"
        L"    // Clean up any existing event listeners first"
        L"    if (window.device) {"
        L"      window.device.removeAllListeners('incoming');"
        L"      window.device.removeAllListeners('connect');"
        L"      window.device.removeAllListeners('disconnect');"
        L"      window.device.removeAllListeners('error');"
        L"      window.device.removeAllListeners('offline');"
        L"      window.device.removeAllListeners('ready');"
        L"    }"
        L"    "
        L"    // Set up event listeners for Twilio.Device"
        L"    window.device.on('incoming', () => window.chrome.webview.postMessage('Incoming'));"
        L"    window.device.on('connect', () => window.chrome.webview.postMessage('Connected'));"
        L"    window.device.on('disconnect', () => window.chrome.webview.postMessage('Disconnected'));"
        L"    window.device.on('error', (error) => window.chrome.webview.postMessage('Error|' + error.message));"
        L"    window.device.on('offline', () => window.chrome.webview.postMessage('Offline'));"
        L"    window.device.on('ready', () => window.chrome.webview.postMessage('Ready'));"
        L"})()",
        [](void*, std::string error) {
        });

      webview_->getWebView()->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            
            LPWSTR message;
            args->get_WebMessageAsJson(&message);
            if (message) {
                // Convert wide string to UTF-8
                int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
                if (utf8Length > 0) {
                    std::string utf8Message;
                    utf8Message.resize(utf8Length - 1);
                    WideCharToMultiByte(CP_UTF8, 0, message, -1, &utf8Message[0], utf8Length, nullptr, nullptr);
                     size_t jsonStart = utf8Message.find_first_of('{');
                  if (jsonStart != std::string::npos) {
                    utf8Message = utf8Message.substr(jsonStart);
                  }
                  
                  // Remove any trailing garbage
                  size_t jsonEnd = utf8Message.find_last_of('}');
                  if (jsonEnd != std::string::npos) {
                    utf8Message = utf8Message.substr(0, jsonEnd + 1);
                  }
                  
                  // Remove escaped quotes
                  std::string unescapedJson = utf8Message;
                  size_t pos = 0;
                  while ((pos = unescapedJson.find("\\\"", pos)) != std::string::npos) {
                    unescapedJson.replace(pos, 2, "\"");
                    pos += 1;
                  }
                  
                  auto json = nlohmann::json::parse(unescapedJson);
                  
                  if (json.contains("type")) {
                    std::string typeValue = json["type"].get<std::string>();
                    
                    if (typeValue == "call_event" && json.contains("event")) {
                      std::string eventValue = json["event"].get<std::string>();

                      // Handle special cases
                      if (eventValue == "incoming") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Incoming|" + from + "|" + to + "|Incoming");
                      } else if (eventValue == "connected") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Connected|" + from + "|" + to + "|Outgoing");
                      } else if (eventValue == "accept") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Answer|" + from + "|" + to);
                      } else if (eventValue == "disconnected") {
                        SendEventToFlutter("Call Ended");
                      } else if (eventValue == "error") {
                        std::string error = json.value("error", "Unknown error");
                        SendEventToFlutter("Error|" + error);
                      } else {
                        // For all other events, just send the capitalized name
                        std::string eventName = eventValue;
                        if (!eventName.empty()) {
                          eventName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(eventName[0])));
                        }
                        SendEventToFlutter(eventName);
                      }
                    }
                  }
                }
                CoTaskMemFree(message);
            }
            return S_OK;
          }).Get(),
        nullptr);
    }); });
  }

  TwilioVoicePlugin::~TwilioVoicePlugin() {}

  void TwilioVoicePlugin::HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {

    const auto &method = method_call.method_name();
    TV_LOG_DEBUG("Handling method: " + method);

    if (method == "tokens")
    {
      if (!method_call.arguments())
      {
        result->Error("Invalid Arguments", "Expected access token");
        return;
      }

      const auto *args = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (!args)
      {
        result->Error("Invalid Arguments", "Expected map with access token");
        return;
      }

      auto token_it = args->find(flutter::EncodableValue("accessToken"));
      if (token_it == args->end())
      {
        result->Error("Invalid Arguments", "Missing access token");
        return;
      }

      const auto &token = std::get<std::string>(token_it->second);
      std::wstring wtoken(token.begin(), token.end());

      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));
      std::wstring setup_script = L"(() => {"
                                  L"  window.device = new Twilio.Device('" +
                                  wtoken + L"', {"
                                           L"    closeProtection: true,"
                                           L"    codecPreferences: ['opus', 'pcmu']"
                                           L"  });"
                                           L"})()";

      webview_->evaluateJavaScript(
          setup_script,
          [shared_result](void *, std::string error)
          {
            if (error == "false")
            {
              (*shared_result)->Error("Setup Failed", error);
            }
            else
            {
              (*shared_result)->Success(true);
            }
          });
    }
    else if (method == "makeCall")
    {
      if (!webview_)
      {
        result->Error("NOT_READY", "WebView not initialized");
        return;
      }

      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (!arguments)
      {
        result->Error("INVALID_ARGUMENTS", "Invalid arguments for makeCall");
        return;
      }

      // Extract required parameters
      std::string from;
      std::string to;
      std::map<std::string, std::string> extraOptions;

      for (const auto &[key, value] : *arguments)
      {
        if (!std::holds_alternative<std::string>(key) ||
            !std::holds_alternative<std::string>(value))
        {
          continue;
        }

        auto keyStr = std::get<std::string>(key);
        auto valueStr = std::get<std::string>(value);

        if (keyStr == "From")
        {
          from = valueStr;
        }
        else if (keyStr == "To")
        {
          to = valueStr;
        }
        else
        {
          extraOptions[keyStr] = valueStr;
        }
      }

      if (from.empty() || to.empty())
      {
        result->Error("INVALID_ARGUMENTS", "From and To parameters are required");
        return;
      }

      TV_LOG_INFO("Making call with From: " + from + ", To: " + to);

      // Convert parameters to wide strings for JavaScript
      std::wstring wfrom(from.begin(), from.end());
      std::wstring wto(to.begin(), to.end());

      // Build JavaScript code with extensive logging
      std::wstring js_code = L"(async () => {"
                             L"try {"
                                                          L"  window.chrome.webview.postMessage({"
                             L"    type: 'call_event',"
                             L"    event: 'ringing'"
                             L"  });"
                             L"  if (typeof Twilio === 'undefined') {"
                             L"    throw new Error('Twilio SDK not loaded - please wait for initialization');"
                             L"  }"
                             L"  if (!window.device) {"
                             L"    throw new Error('Twilio Device not initialized - please call tokens() first');"
                             L"  }"
                             L"  const params = {"
                             L"    params: {"
                             L"      To: '" +
                             wto + L"',"
                                   L"      From: '" +
                             wfrom + L"'"
                                     L"    },"
                                     L"    codecPreferences: ['opus', 'pcmu']"
                                     L"  };"
                                     L"  window.connection = await window.device.connect(params);"
                                     L"  if (!window.connection) {"
                                     L"    throw new Error('Failed to create connection - connection is null');"
                                     L"  }"
                                     L"  window.connection.on('accept', () => {"
                                     L"    window.chrome.webview.postMessage({"
                                     L"      type: 'call_event',"
                                     L"      event: 'accept'"
                                     L"    });"
                                     L"  });"
                                     L"  window.connection.on('disconnect', () => {"
                                     L"    window.chrome.webview.postMessage({"
                                     L"      type: 'call_event',"
                                     L"      event: 'disconnected'"
                                     L"    });"
                                     L"  });"
                                     L"  window.connection.on('error', (error) => {"
                                     L"    window.chrome.webview.postMessage({"
                                     L"      type: 'call_event',"
                                     L"      event: 'error',"
                                     L"      error: error.message"
                                     L"    });"
                                     L"  });"
                                     L"  window.connection.on('reject', () => {"
                                     L"    window.chrome.webview.postMessage({"
                                     L"      type: 'call_event',"
                                     L"      event: 'reject'"
                                     L"    });"
                                     L"  });"
                                     L"  window.connection.on('cancel', () => {"
                                     L"    window.chrome.webview.postMessage({"
                                     L"      type: 'call_event',"
                                     L"      event: 'cancel'"
                                     L"    });"
                                     L"  });"
                                     L"  return '';"
                                     L"} catch (error) {"
                                     L"  window.chrome.webview.postMessage({"
                                     L"    type: 'call_event',"
                                     L"    event: 'error',"
                                     L"    error: error.message"
                                     L"  });"
                                     L"  throw error;"
                                     L"}"
                                     L"})()";

      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      TV_LOG_DEBUG("Executing JavaScript for makeCall");
      webview_->evaluateJavaScript(
          js_code,
          [shared_result](void *, std::string error)
          {
            if (error != "{}")
            {
              TV_LOG_ERROR("JavaScript error: " + error);
              (*shared_result)->Error("CALL_FAILED", error);
            }
            else
            {
              TV_LOG_INFO("Call initiated successfully");
              (*shared_result)->Success(true);
            }
          });
    }
    else if (method == "toggleMute")
    {
      if (!method_call.arguments())
      {
        result->Error("Invalid Arguments", "Expected mute state");
        return;
      }

      const auto *args = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (!args)
      {
        result->Error("Invalid Arguments", "Expected map with mute state");
        return;
      }

      auto muted_it = args->find(flutter::EncodableValue("muted"));
      if (muted_it == args->end())
      {
        result->Error("Invalid Arguments", "Missing 'muted' parameter");
        return;
      }

      bool muted = std::get<bool>(muted_it->second);

      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview_->evaluateJavaScript(
          std::wstring(L"window.connection.mute(") + (muted ? L"true" : L"false") + L"); window.connection.isMuted()",
          [shared_result, this](void *, std::string response)
          {
            if (response == "null")
            {
              (*shared_result)->Success(nullptr);
            }
            else
            {
              TV_LOG_DEBUG("isMuted response: " + response);
              bool isMuted = response == "true";
              (*shared_result)->Success(isMuted);
              SendEventToFlutter(isMuted ? "Mute" : "Unmute");
            }
          });
    }
    else if (method == "isMuted")
    {
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview_->evaluateJavaScript(
          L"window.connection.isMuted()",
          [shared_result](void *, std::string response)
          {
            if (response == "null")
            {
              (*shared_result)->Success(nullptr);
            }
            else
            {
              TV_LOG_DEBUG("isMuted response: " + response);
              (*shared_result)->Success(response == "true");
            }
          });
    }
    else if (method == "hangUp")
    {
      TV_LOG_DEBUG("Executing hangUp command");

      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      std::wstring disconnect_script = L"(() => { \n"
                                       L"  try { \n"
                                       L"    // Try different ways to get the active connection\n"
                                       L"    let activeConnection = window.connection;\n"
                                       L"    if (activeConnection) { \n"
                                       L"      activeConnection.disconnect(); \n"
                                       L"    }\n"
                                       L"    window.device.removeAllListeners('incoming'); \n"
                                       L"    window.device.removeAllListeners('connect'); \n"
                                       L"    window.device.removeAllListeners('disconnect'); \n"
                                       L"    window.device.removeAllListeners('error'); \n"
                                       L"    window.device.removeAllListeners('offline'); \n"
                                       L"    window.device.removeAllListeners('ready'); \n"
                                       L"    window.device.removeAllListeners('reject'); \n"
                                       L"    window.device.removeAllListeners('cancel'); \n"
                                       L"    \n"
                                       L"    // Force audio to stop \n"
                                       L"    if (window.device.audio && window.device.audio.disconnect) { \n"
                                       L"      window.device.audio.disconnect(); \n"
                                       L"    }\n"
                                       L"    // Clean up any tracked audio resources \n"
                                       L"    if (typeof window.cleanupAudioResources === 'function') { \n"
                                       L"      window.cleanupAudioResources(); \n"
                                       L"    }\n"
                                       L"    return ''; \n"
                                       L"  } catch (error) { \n"
                                       L"    return error.message; \n"
                                       L"  } \n"
                                       L"})()";

      webview_->evaluateJavaScript(
          disconnect_script,
          [shared_result, this](void *, std::string error)
          {
            // Always reset the activeCall_ pointer
            activeCall_.reset();

            if (error != "\"\"")
            {
              TV_LOG_ERROR("Hangup error: " + error);
              (*shared_result)->Error("HANGUP_FAILED", "Failed to hang up call: " + error);
            }
            else
            {
              TV_LOG_INFO("Call successfully disconnected");
              (*shared_result)->Success(true);
            }
          });
    }
    else if (method == "hasMicPermission")
    {
      // Use a shared pointer to keep the result alive until the async operation completes
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      // Execute JavaScript to check microphone permission status
      std::wstring checkPermissionScript = L"(async () => {"
                                          L"  try {"
                                          L"    const permission = await navigator.permissions.query({name:'microphone'});"
                                          L"    return permission.state === 'granted';"
                                          L"  } catch (error) {"
                                          L"    return false;"
                                          L"  }"
                                          L"})()";

      webview_->evaluateJavaScript(
          checkPermissionScript,
          [shared_result](void *, std::string response)
          {
            try {
              bool hasPermission = response == "true";
              (*shared_result)->Success(hasPermission);
            } catch (const std::exception &e) {
              TV_LOG_ERROR("Error parsing microphone permission response: " + std::string(e.what()));
              (*shared_result)->Success(false);
            }
          });
    }
    else if (method == "requestMicPermission")
    {
      // Permission is requested in the hasMicPermission method automatically.
      result->Success(true);
    }
    else if (method == "isHolding")
    {
      // Not supported on Windows
      result->Success(false);
    }
    else if (method == "isBluetoothOn")
    {
      // Not supported on Windows
      result->Success(false);
    }
    else if (method == "toggleSpeaker")
    {
      // Not supported on Windows
      result->Success(true);
    }
    else if (method == "isOnSpeaker")
    {
      // Not supported on Windows
      result->Success(true);
    }
    else
    {
      result->NotImplemented();
    }
  }

  void TwilioVoicePlugin::SendEventToFlutter(const std::string &event)
  {
    if (!event_sink_)
    {
      TV_LOG_ERROR("Cannot send event to Flutter: event_sink_ is null, Event: " + event);
      return;
    }

    TV_LOG_DEBUG("Attempting to send event to Flutter: " + event);
    try
    {
      event_sink_->Success(flutter::EncodableValue(event));
      TV_LOG_DEBUG("Successfully sent event to Flutter: " + event);
    }
    catch (const std::exception &e)
    {
      TV_LOG_ERROR("Failed to send event to Flutter: " + std::string(e.what()) + ", Event: " + event);
    }
  }

  // TVCallDelegate implementations
  void TwilioVoicePlugin::onCallAccept(TVCall *call)
  {
    SendEventToFlutter("Accept");
  }

  void TwilioVoicePlugin::onCallCancel(TVCall *call)
  {
    SendEventToFlutter("Cancel");
  }

  void TwilioVoicePlugin::onCallDisconnect(TVCall *call)
  {
    SendEventToFlutter("Disconnect");
  }

  void TwilioVoicePlugin::onCallError(const TVError &error)
  {
    json event;
    event["type"] = "error";
    event["code"] = error.code;
    event["message"] = error.message;
    SendEventToFlutter(event.dump());
  }

  void TwilioVoicePlugin::onCallReconnecting(const TVError &error)
  {
    json event;
    event["type"] = "reconnecting";
    event["error"] = error.message;
    SendEventToFlutter(event.dump());
  }

  void TwilioVoicePlugin::onCallReconnected()
  {
    SendEventToFlutter("Reconnected");
  }

  void TwilioVoicePlugin::onCallReject()
  {
    SendEventToFlutter("Reject");
  }

  void TwilioVoicePlugin::onCallStatus(const TVCallStatus &status)
  {
    json event;
    event["type"] = "status";
    event["status"] = status.status;
    event["callSid"] = status.callSid;
    SendEventToFlutter(event.dump());
  }

} // namespace twilio_voice
