// This must be included before many other Windows headers.
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <propkey.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <windows.ui.notifications.h>
#include <wrl.h>
#include <notificationactivationcallback.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.data.xml.dom.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propvarutil.h>

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

// Helper function to convert wide string to UTF-8
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return std::string();
    }
    std::string utf8;
    utf8.resize(utf8Length - 1); // -1 to not include null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], utf8Length, nullptr, nullptr);
    return utf8;
}

namespace twilio_voice
{

  // Add the class factory implementation before it's used
  class TVNotificationActivationCallbackFactory : public Microsoft::WRL::RuntimeClass<
      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
      IClassFactory> {
  public:
      TVNotificationActivationCallbackFactory() = default;
      ~TVNotificationActivationCallbackFactory() = default;

      // IClassFactory
      IFACEMETHODIMP CreateInstance(
          IUnknown* outer,
          REFIID riid,
          void** ppv) override {
          if (outer != nullptr) {
              return CLASS_E_NOAGGREGATION;
          }

          Microsoft::WRL::ComPtr<TVNotificationActivationCallback> callback;
          HRESULT hr = Microsoft::WRL::MakeAndInitialize<TVNotificationActivationCallback>(&callback);
          if (SUCCEEDED(hr)) {
              hr = callback->QueryInterface(riid, ppv);
          }
          return hr;
      }

      IFACEMETHODIMP LockServer(BOOL lock) override {
          return S_OK;
      }
  };

  // Initialize static members
  TVWebView* TVNotificationManager::webview_ = nullptr;
  DWORD TVNotificationManager::comRegistrationCookie = 0;

  void TwilioVoicePlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarWindows *registrar)
  {
    auto plugin = std::make_unique<TwilioVoicePlugin>(registrar);
    registrar->AddPlugin(std::move(plugin));
  }

  TwilioVoicePlugin::TwilioVoicePlugin(flutter::PluginRegistrarWindows *registrar)
      : registrar_(registrar)
  {
    // Initialize COM with apartment threading model
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
      TV_LOG_DEBUG("COM initialized successfully");
    } else {
      TV_LOG_ERROR("Failed to initialize COM. HRESULT: " + std::to_string(hr));
    }

    // Set AUMID explicitly before any notification registration
    std::wstring defaultAumid = L"SpaceAuto.App";
    hr = SetCurrentProcessExplicitAppUserModelID(defaultAumid.c_str());
    if (SUCCEEDED(hr)) {
      TV_LOG_DEBUG("Set default AUMID successfully: " + WideToUtf8(defaultAumid));
    } else {
      TV_LOG_ERROR("Failed to set AUMID. HRESULT: " + std::to_string(hr));
    }

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
    TVNotificationManager::setWebView(webview_.get());
    
    // Register COM server for notification activation
    TVNotificationManager::RegisterCOMServer();

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
        L"})()",
        [](void*, std::string error) {
        });

      webview_->getWebView()->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            
            LPWSTR message;
            args->get_WebMessageAsJson(&message);
            if (message) {
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
                    
                    try {
                         auto json = nlohmann::json::parse(unescapedJson);
                  
                  if (json.contains("type")) {
                    std::string typeValue = json["type"].get<std::string>();
                    
                    if (typeValue == "call_event" && json.contains("event")) {
                      std::string eventValue = json["event"].get<std::string>();

                      // Handle special cases
                      if (eventValue == "incoming") {
                        // Check microphone permission when receiving an incoming call
                        CheckMicrophonePermission();
                        
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        std::string callSid = json.value("callSid", "");
                        
                        // Show notification
                        TVNotificationManager::getInstance().showIncomingCallNotification(from, callSid);
                        
                        SendEventToFlutter("Incoming|" + from + "|" + to + "|Incoming");
                      } else if (eventValue == "connected") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Connected|" + from + "|" + to + "|Outgoing");
                      } else if (eventValue == "accept") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        std::string callSid = json.value("callSid", "");
                        TVNotificationManager::getInstance().showIncomingCallNotification(from, callSid);
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
                  
                    } catch (const std::exception& e) {
                        TV_LOG_ERROR("Error processing message: " + std::string(e.what()));
                    }
                } else {
                    TV_LOG_ERROR("Failed to convert message to UTF-8");
                }
                CoTaskMemFree(message);
            } else {
                TV_LOG_ERROR("Received null message from WebView2");
            }
            return S_OK;
          }).Get(),
        nullptr);
    }); });
  }

  TwilioVoicePlugin::~TwilioVoicePlugin() {
    if (webview_) {
      webview_.reset();  // This will call the destructor of TVWebView
    }
    TVNotificationManager::UnregisterCOMServer();
  }

  void TwilioVoicePlugin::HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    const auto &method = method_call.method_name();

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
                                           L"  window.device.register().then(() => {"
                                           L"    window.device.on('incoming', (call) => {"
                                           L"      const params = call.parameters;"
                                           L"      window.connection = call;"
                                           L"      window.connection.on('accept', () => {"
                                           L"        window.chrome.webview.postMessage({"
                                           L"          type: 'call_event',"
                                           L"          event: 'accept',"
                                           L"          from: params.From,"
                                           L"          to: params.To,"
                                           L"          callSid: params.CallSid"
                                           L"        });"
                                           L"      });"
                                           L"      window.chrome.webview.postMessage({"
                                           L"        type: 'call_event',"
                                           L"        event: 'incoming',"
                                           L"        from: params.From,"
                                           L"        to: params.To,"
                                           L"        callSid: params.CallSid"
                                           L"      });"
                                           L"    });"
                                           L"    window.device.on('connect', (call) => {"
                                           L"      const params = call.parameters;"
                                           L"      window.chrome.webview.postMessage({"
                                           L"        type: 'call_event',"
                                           L"        event: 'connected',"
                                           L"        from: params.From,"
                                           L"        to: params.To,"
                                           L"        callSid: params.CallSid"
                                           L"      });"
                                           L"    });"
                                           L"    window.device.on('disconnect', () => window.chrome.webview.postMessage('Disconnected'));"
                                           L"    window.device.on('error', (error) => window.chrome.webview.postMessage('Error|' + error.message));"
                                           L"    window.device.on('offline', () => window.chrome.webview.postMessage('Offline'));"
                                           L"    window.device.on('ready', () => window.chrome.webview.postMessage('Ready'));"
                                           L"    return true;"
                                           L"  }).catch((error) => {"
                                           L"    console.error('Failed to register device:', error);"
                                           L"    return false;"
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
                                     L"      event: 'accept',"
                                     L"      callSid: window.connection.parameters.CallSid"
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

      webview_->evaluateJavaScript(
          js_code,
          [shared_result](void *, std::string error)
          {
            if (error != "{}")
            {
              (*shared_result)->Error("CALL_FAILED", error);
            }
            else
            {
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
              (*shared_result)->Success(response == "true");
            }
          });
    }
    else if (method == "hangUp")
    {
      HangUpCall(webview_.get(), std::move(result));
    }
    else if (method == "answer")
    {
      AnswerCall(webview_.get(), std::move(result));
    }
    else if (method == "hasMicPermission")
    {
      CheckMicrophonePermission(std::move(result));
    }
    else if (method == "requestMicPermission")
    {
      // Permission is requested in the hasMicPermission method automatically.
      result->Success(true);
    }
    else if (method == "hasNotificationPermission")
    {
      bool hasPermission = TVNotificationManager::getInstance().hasNotificationPermission();
      result->Success(hasPermission);
    }
    else if (method == "requestNotificationPermission")
    {
      bool success = TVNotificationManager::getInstance().requestNotificationPermission();
      result->Success(success);
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
    else if (method == "unregister")
    {
      if (!webview_)
      {
        result->Error("NOT_READY", "WebView not initialized");
        return;
      }

      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      std::wstring unregister_script = L"(() => {"
                                      L"  try {"
                                      L"    if (window.device) {"
                                      L"      window.device.unregister();"
                                      L"      window.device.removeAllListeners('incoming');"
                                      L"      window.device.removeAllListeners('connect');"
                                      L"      window.device.removeAllListeners('disconnect');"
                                      L"      window.device.removeAllListeners('error');"
                                      L"      window.device.removeAllListeners('offline');"
                                      L"      window.device.removeAllListeners('ready');"
                                      L"      return true;"
                                      L"    }"
                                      L"    return false;"
                                      L"  } catch (error) {"
                                      L"    return error.message;"
                                      L"  }"
                                      L"})()";

      webview_->evaluateJavaScript(
          unregister_script,
          [shared_result](void *, std::string response)
          {
            if (response == "true")
            {
              (*shared_result)->Success(true);
            }
            else
            {
              TV_LOG_ERROR("Failed to unregister from Twilio: " + response);
              (*shared_result)->Error("UNREGISTER_FAILED", response);
            }
          });
    }
    else
    {
      result->NotImplemented();
    }
  }

  void TwilioVoicePlugin::CheckMicrophonePermission(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    // Execute JavaScript to check microphone permission status
    std::wstring checkPermissionScript = L"(async () => {"
                                        L"  try {"
                                        L"    const permission = await navigator.permissions.query({name:'microphone'});"
                                        L"    return permission.state === 'granted';"
                                        L"  } catch (error) {"
                                        L"    return false;"
                                        L"  }"
                                        L"})()";

    if (result) {
      // If we have a result object, use it to send the response back to Flutter
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

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
    } else {
      // If no result object, just log the permission status
      webview_->evaluateJavaScript(
          checkPermissionScript,
          [](void *, std::string response)
          {
            try {
              bool hasPermission = response == "true";
              TV_LOG_DEBUG("Microphone permission status: " + std::string(hasPermission ? "granted" : "denied"));
            } catch (const std::exception &e) {
              TV_LOG_ERROR("Error parsing microphone permission response: " + std::string(e.what()));
            }
          });
    }
  }

  void TwilioVoicePlugin::SendEventToFlutter(const std::string &event)
  {
    if (!event_sink_)
    {
      TV_LOG_ERROR("Cannot send event to Flutter: event_sink_ is null, Event: " + event);
      return;
    }

    try
    {
      event_sink_->Success(flutter::EncodableValue(event));
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

  void TwilioVoicePlugin::AnswerCall(TVWebView* webview, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    TV_LOG_DEBUG("Executing answer command");

    // Hide any active notifications
    if (webview) {
        webview->evaluateJavaScript(
            L"window.connection ? window.connection.parameters.CallSid : ''",
            [](void *, std::string callSid) {
                if (!callSid.empty() && callSid != "\"\"") {
                    TVNotificationManager::getInstance().hideNotification(callSid);
                }
            });
    }

    if (!webview) {
      if (result) {
        result->Error("NOT_READY", "WebView not initialized");
      }
      return;
    }

    std::wstring answer_script = L"(() => {"
                                L"  try {"
                                L"    if (window.connection) {"
                                L"      window.connection.accept();"
                                L"      return true;"
                                L"    }"
                                L"    return false;"
                                L"  } catch (error) {"
                                L"    return error.message;"
                                L"  }"
                                L"})()";

    if (result) {
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview->evaluateJavaScript(
          answer_script,
          [shared_result](void *, std::string response) {
            if (response == "true") {
              if (*shared_result) {
                (*shared_result)->Success(true);
              }
            } else if (response == "false") {
              TV_LOG_ERROR("No active connection to answer");
              if (*shared_result) {
                (*shared_result)->Success(false);
              }
            } else {
              TV_LOG_ERROR("Answer error: " + response);
              if (*shared_result) {
                (*shared_result)->Error("ANSWER_FAILED", "Failed to answer call: " + response);
              }
            }
          });
    } else {
      // When called from notification handler, just execute the script without result handling
      webview->evaluateJavaScript(
          answer_script,
          [](void *, std::string response) {
            if (response == "true") {
              TV_LOG_DEBUG("Successfully answered call from notification");
            } else if (response == "false") {
              TV_LOG_ERROR("No active connection to answer from notification");
            } else {
              TV_LOG_ERROR("Answer error from notification: " + response);
            }
          });
    }
  }

  void TwilioVoicePlugin::HangUpCall(TVWebView* webview, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    TV_LOG_DEBUG("Executing hangUp command");

    // Hide any active notifications
    if (webview) {
        webview->evaluateJavaScript(
            L"window.connection ? window.connection.parameters.CallSid : ''",
            [](void *, std::string callSid) {
                TV_LOG_DEBUG("HangUp CallSid: " + callSid);
                if (!callSid.empty() && callSid != "\"\"") {
                    // Strip quotes from callSid
                    if (callSid.front() == '"' && callSid.back() == '"') {
                        callSid = callSid.substr(1, callSid.length() - 2);
                    }
                    TV_LOG_DEBUG("HangUp CallSid hideNotification: " + callSid);
                    TVNotificationManager::getInstance().hideNotification(callSid);
                }
            });
    }

    if (!webview) {
        if (result) {
            result->Error("NOT_READY", "WebView not initialized");
        }
        return;
    }

    std::wstring disconnect_script = L"(() => { \n"
                                   L"  try { \n"
                                   L"    if (window.connection) { \n"
                                   L"      const status = window.connection.status();\n"
                                   L"      if (status === 'pending' || status === 'ringing') {\n"
                                   L"        window.connection.reject();\n"
                                   L"      } else {\n"
                                   L"        window.connection.disconnect();\n"
                                   L"      }\n"
                                   L"        window.chrome.webview.postMessage({\n"
                                   L"          type: 'call_event',\n"
                                   L"          event: 'disconnected'\n"
                                   L"        });\n"
                                   L"      window.connection = null;\n"
                                   L"      return true;\n"
                                   L"    }\n"
                                   L"    return false;\n"
                                   L"  } catch (error) { \n"
                                   L"    return error.message; \n"
                                   L"  } \n"
                                   L"})()";

    if (result) {
        auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
            std::move(result));

        webview->evaluateJavaScript(
            disconnect_script,
            [shared_result](void *, std::string response) {
                if (response == "true") {
                    if (*shared_result) {
                        (*shared_result)->Success(true);
                    }
                } else if (response == "false") {
                    if (*shared_result) {
                        (*shared_result)->Success(false);
                    }
                } else {
                    TV_LOG_ERROR("Hangup error: " + response);
                    if (*shared_result) {
                        (*shared_result)->Error("HANGUP_FAILED", "Failed to hang up call: " + response);
                    }
                }
            });
    } else {
        // When called from notification handler, just execute the script without result handling
        webview->evaluateJavaScript(
            disconnect_script,
            [](void *, std::string response) {
                if (response == "true") {
                    TV_LOG_DEBUG("Successfully hung up call from notification");
                } else if (response == "false") {
                    TV_LOG_ERROR("No active connection to hang up from notification");
                } else {
                    TV_LOG_ERROR("Hangup error from notification: " + response);
                }
            });
    }
  }

  TVNotificationManager::TVNotificationManager() {
    TV_LOG_DEBUG("TVNotificationManager constructor called");
    
    // Initialize COM with multi-threaded apartment
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        if (hr == RPC_E_CHANGED_MODE) {
            TV_LOG_DEBUG("COM already initialized with different threading model, proceeding anyway");
        } else {
            TV_LOG_ERROR("Failed to initialize COM. HRESULT: " + std::to_string(hr));
            return;
        }
    } else {
        TV_LOG_DEBUG("COM initialized successfully with COINIT_MULTITHREADED");
    }

    // Initialize Windows Runtime
    hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) {
        if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
            TV_LOG_DEBUG("Windows Runtime already initialized, proceeding anyway");
        } else {
            TV_LOG_ERROR("Failed to initialize Windows Runtime. HRESULT: " + std::to_string(hr));
            return;
        }
    } else {
        TV_LOG_DEBUG("Windows Runtime initialized successfully");
    }

    // Register the notification activation callback
    Microsoft::WRL::ComPtr<TVNotificationActivationCallback> callback;
    hr = Microsoft::WRL::MakeAndInitialize<TVNotificationActivationCallback>(&callback);
    if (SUCCEEDED(hr)) {
        // Get the app's AUMID
        PWSTR aumid = nullptr;
        hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
        if (SUCCEEDED(hr)) {
            std::wstring regPath = L"SOFTWARE\\Classes\\AppUserModelId\\" + std::wstring(aumid);
            HKEY hKey;
            hr = RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
            if (SUCCEEDED(hr)) {
                // Convert the CLSID to string
                std::wstring clsidStr;
                LPOLESTR clsidOleStr;
                hr = StringFromCLSID(CLSID_TVNotificationActivationCallback, &clsidOleStr);
                if (SUCCEEDED(hr)) {
                    clsidStr = clsidOleStr;
                    CoTaskMemFree(clsidOleStr);

                    hr = RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, 
                        reinterpret_cast<const BYTE*>(L"Twilio Voice Notification Callback"), 
                        sizeof(L"Twilio Voice Notification Callback"));
                    
                    if (SUCCEEDED(hr)) {
                        hr = RegSetValueExW(hKey, L"CustomActivator", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(clsidStr.c_str()),
                            static_cast<DWORD>((clsidStr.length() + 1) * sizeof(wchar_t)));
                        
                        if (SUCCEEDED(hr)) {
                            TV_LOG_DEBUG("Successfully registered notification activation callback");
                        } else {
                            TV_LOG_ERROR("Failed to set CustomActivator registry value. HRESULT: " + std::to_string(hr));
                        }
                    } else {
                        TV_LOG_ERROR("Failed to set DisplayName registry value. HRESULT: " + std::to_string(hr));
                    }
                } else {
                    TV_LOG_ERROR("Failed to convert CLSID to string. HRESULT: " + std::to_string(hr));
                }
                RegCloseKey(hKey);
            } else {
                TV_LOG_ERROR("Failed to create registry key. HRESULT: " + std::to_string(hr));
            }
            CoTaskMemFree(aumid);
        } else {
            TV_LOG_ERROR("Failed to get AUMID for notification registration. HRESULT: " + std::to_string(hr));
        }
    } else {
        TV_LOG_ERROR("Failed to create notification activation callback. HRESULT: " + std::to_string(hr));
    }

    // Get the app's name from the module path
    wchar_t module_path[MAX_PATH];
    GetModuleFileNameW(NULL, module_path, MAX_PATH);
    std::wstring path(module_path);
    size_t lastBackslash = path.find_last_of(L"\\");
    std::wstring appName = path.substr(lastBackslash + 1);
    // Remove .exe from appName if present
    size_t dotPos = appName.find_last_of(L".");
    if (dotPos != std::wstring::npos) {
        appName = appName.substr(0, dotPos);
    }
    
    TV_LOG_DEBUG("App name: " + WideToUtf8(appName));

    // Get the actual AUMID for the app
    PWSTR aumid = nullptr;
    hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (SUCCEEDED(hr)) {
        std::wstring wAumid(aumid);
        CoTaskMemFree(aumid);
        TV_LOG_DEBUG("Got existing AUMID: " + WideToUtf8(wAumid));
    } else {
        TV_LOG_DEBUG("Setting default AUMID");
        std::wstring defaultAumid = appName + L".App";
        hr = SetCurrentProcessExplicitAppUserModelID(defaultAumid.c_str());
        if (SUCCEEDED(hr)) {
            TV_LOG_DEBUG("Set default AUMID successfully: " + WideToUtf8(defaultAumid));
            
            // Verify the AUMID was actually set
            PWSTR verifyAumid = nullptr;
            hr = GetCurrentProcessExplicitAppUserModelID(&verifyAumid);
            if (SUCCEEDED(hr)) {
                std::wstring wVerifyAumid(verifyAumid);
                CoTaskMemFree(verifyAumid);
                TV_LOG_DEBUG("Verified AUMID is set to: " + WideToUtf8(wVerifyAumid));
            } else {
                TV_LOG_ERROR("Failed to verify AUMID was set. HRESULT: " + std::to_string(hr));
            }
        } else {
            TV_LOG_ERROR("Failed to set AUMID. HRESULT: " + std::to_string(hr));
        }
    }

    // Register the app for notifications
    hr = Windows::Foundation::GetActivationFactory(
        Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
        &toastManager);
    if (SUCCEEDED(hr)) {
        TV_LOG_DEBUG("Toast notification manager factory created successfully");

        // Create a shortcut in the Start menu
        std::wstring shortcutPath = L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\" + appName + L".lnk";
        
        // Expand environment variables in the path
        wchar_t expandedPath[MAX_PATH];
        ExpandEnvironmentStringsW(shortcutPath.c_str(), expandedPath, MAX_PATH);
        shortcutPath = expandedPath;
        
        TV_LOG_DEBUG("Creating shortcut at: " + WideToUtf8(shortcutPath));

        // Create the shortcut
        IShellLinkW* shellLink = nullptr;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&shellLink);
        if (SUCCEEDED(hr)) {
            TV_LOG_DEBUG("Shell link created successfully");
            shellLink->SetPath(module_path);
            shellLink->SetDescription(L"Space Auto Application");
            shellLink->SetWorkingDirectory(path.substr(0, lastBackslash).c_str());

            // Set the AppUserModelID for the shortcut
            IPropertyStore* propertyStore = nullptr;
            hr = shellLink->QueryInterface(IID_IPropertyStore, (void**)&propertyStore);
            if (SUCCEEDED(hr)) {
                PROPVARIANT propVar;
                PropVariantInit(&propVar);
                std::wstring shortcutAumid = appName + L".App";
                hr = InitPropVariantFromString(shortcutAumid.c_str(), &propVar);
                if (SUCCEEDED(hr)) {
                    hr = propertyStore->SetValue(PKEY_AppUserModel_ID, propVar);
                    if (SUCCEEDED(hr)) {
                        TV_LOG_DEBUG("Set AppUserModelID on shortcut successfully");
                        hr = propertyStore->Commit();
                        if (SUCCEEDED(hr)) {
                            TV_LOG_DEBUG("Committed property store successfully");
                        } else {
                            TV_LOG_ERROR("Failed to commit property store. HRESULT: " + std::to_string(hr));
                        }
                    }
                    propertyStore->Release();
                }
            }

            IPersistFile* persistFile = nullptr;
            hr = shellLink->QueryInterface(IID_IPersistFile, (void**)&persistFile);
            if (SUCCEEDED(hr)) {
                TV_LOG_DEBUG("Persist file interface obtained successfully");
                hr = persistFile->Save(shortcutPath.c_str(), TRUE);
                if (SUCCEEDED(hr)) {
                    TV_LOG_DEBUG("Shortcut saved successfully");
                } else {
                    TV_LOG_ERROR("Failed to save shortcut. HRESULT: " + std::to_string(hr));
                }
                persistFile->Release();
            } else {
                TV_LOG_ERROR("Failed to get persist file interface. HRESULT: " + std::to_string(hr));
            }
            shellLink->Release();
        } else {
            TV_LOG_ERROR("Failed to create shell link. HRESULT: " + std::to_string(hr));
        }

        // Create toast notifier after shortcut is created
        // Give Windows more time to register the shortcut and AUMID
        Sleep(2000); // Increased to 2 seconds to ensure registration
        
        // Get the current AUMID
        PWSTR currentAumid = nullptr;
        hr = GetCurrentProcessExplicitAppUserModelID(&currentAumid);
        if (SUCCEEDED(hr)) {
            std::wstring wCurrentAumid(currentAumid);
            CoTaskMemFree(currentAumid);
            TV_LOG_DEBUG("Current AUMID before creating toast notifier: " + WideToUtf8(wCurrentAumid));
            
            // Try to create toast notifier multiple times
            const int maxRetries = 3;
            int retryCount = 0;
            bool success = false;
            
            while (retryCount < maxRetries && !success) {
                // Create HSTRING from AUMID
                HSTRING hAumid;
                hr = WindowsCreateString(wCurrentAumid.c_str(), static_cast<UINT32>(wCurrentAumid.length()), &hAumid);
                if (SUCCEEDED(hr)) {
                    // Try to create the notifier with the explicit AUMID
                    hr = toastManager->CreateToastNotifierWithId(hAumid, &toastNotifier);
                    WindowsDeleteString(hAumid);
                    
                    if (SUCCEEDED(hr)) {
                        TV_LOG_DEBUG("Toast notifier created successfully on attempt " + std::to_string(retryCount + 1));
                        success = true;
                    } else {
                        // Format the error message
                        LPSTR errorMessage = nullptr;
                        FormatMessageA(
                            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            hr,
                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                            (LPSTR)&errorMessage,
                            0,
                            NULL
                        );
                        
                        if (errorMessage) {
                            TV_LOG_ERROR("Failed to create toast notifier on attempt " + std::to_string(retryCount + 1) + 
                                        ". HRESULT: " + std::to_string(hr) + ", Error: " + errorMessage);
                            LocalFree(errorMessage);
                        } else {
                            TV_LOG_ERROR("Failed to create toast notifier on attempt " + std::to_string(retryCount + 1) + 
                                        ". HRESULT: " + std::to_string(hr));
                        }
                        
                        retryCount++;
                        if (retryCount < maxRetries) {
                            TV_LOG_DEBUG("Retrying in 1 second...");
                            Sleep(1000); // Wait 1 second before retrying
                        }
                    }
                } else {
                    TV_LOG_ERROR("Failed to create HSTRING from AUMID. HRESULT: " + std::to_string(hr));
                    break;
                }
            }
            
            if (!success) {
                TV_LOG_ERROR("Failed to create toast notifier after " + std::to_string(maxRetries) + " attempts");
            }
        } else {
            TV_LOG_ERROR("Failed to get current AUMID before creating toast notifier. HRESULT: " + std::to_string(hr));
        }
    } else {
        TV_LOG_ERROR("Failed to get activation factory for toast manager. HRESULT: " + std::to_string(hr));
    }
  }

  TVNotificationManager& TVNotificationManager::getInstance() {
    TV_LOG_DEBUG("Getting TVNotificationManager instance");
    static TVNotificationManager instance;
    TV_LOG_DEBUG("TVNotificationManager instance created/retrieved");
    return instance;
  }

  void TVNotificationManager::showIncomingCallNotification(const std::string& from, const std::string& callSid) {
    TV_LOG_DEBUG("Attempting to show notification for call from: " + from + " with SID: " + callSid);
    
    if (!toastNotifier) {
        TV_LOG_ERROR("Toast notifier not initialized");
        return;
    }

    // Check Windows notification permission
    if (!hasNotificationPermission()) {
        TV_LOG_ERROR("Windows notification permission not granted");
        if (!requestNotificationPermission()) {
            TV_LOG_ERROR("Failed to request notification permission");
            return;
        }
    }

    // Show the notification
    ShowNotificationInternal(from, callSid);
  }

  void TVNotificationManager::ShowNotificationInternal(const std::string& from, const std::string& callSid) {
    Microsoft::WRL::ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocument> xmlDoc;
    HRESULT hr = Windows::Foundation::ActivateInstance(
        Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
        &xmlDoc);
    
    if (SUCCEEDED(hr)) {
        TV_LOG_DEBUG("XML document created successfully");
        
        // Convert from string to wstring using proper UTF-8 conversion
        std::wstring wFrom;
        if (!from.empty()) {
            int wideLength = MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, nullptr, 0);
            if (wideLength > 0) {
                wFrom.resize(wideLength - 1);
                MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, &wFrom[0], wideLength);
            }
        } else {
            wFrom = L"Unknown Caller";
        }

        int wideLength = MultiByteToWideChar(CP_UTF8, 0, callSid.c_str(), -1, nullptr, 0);
        std::wstring wCallSid;
        if (wideLength > 0) {
            wCallSid.resize(wideLength - 1);
            MultiByteToWideChar(CP_UTF8, 0, callSid.c_str(), -1, &wCallSid[0], wideLength);
        }
        
        // Create the toast XML with proper error handling
        std::wstring xml = L"<toast>"
                          L"<visual><binding template='ToastGeneric'>"
                          L"<text>Incoming Call</text>"
                          L"<text>" + wFrom + L"</text>"
                          L"</binding></visual>"
                          L"<actions>"
                          L"<action content='Accept' arguments='accept:" + wCallSid + L"' activationType='foreground'/>"
                          L"<action content='Reject' arguments='reject:" + wCallSid + L"' activationType='foreground'/>"
                          L"</actions>"
                          L"<audio src='ms-winsoundevent:Notification.Looping.Call' loop='true'/>"
                          L"</toast>";

        TV_LOG_DEBUG("Generated XML: " + WideToUtf8(xml));

        Microsoft::WRL::ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocumentIO> xmlDocIO;
        hr = xmlDoc.As(&xmlDocIO);
        if (SUCCEEDED(hr)) {
            TV_LOG_DEBUG("Successfully got XML document IO interface");
            
            HSTRING xmlStr;
            hr = WindowsCreateString(xml.c_str(), static_cast<UINT32>(xml.length()), &xmlStr);
            if (SUCCEEDED(hr)) {
                TV_LOG_DEBUG("Successfully created Windows string from XML");
                
                hr = xmlDocIO->LoadXml(xmlStr);
                WindowsDeleteString(xmlStr);
                
                if (SUCCEEDED(hr)) {
                    TV_LOG_DEBUG("Successfully loaded XML into document");
                    
                    // Get the toast notification factory
                    Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotificationFactory> toastFactory;
                    hr = Windows::Foundation::GetActivationFactory(
                        Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
                        &toastFactory);
                    if (SUCCEEDED(hr)) {
                        TV_LOG_DEBUG("Successfully got toast notification factory");
                        
                        // Create the toast notification
                        Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotification> toast;
                        hr = toastFactory->CreateToastNotification(xmlDoc.Get(), &toast);
                        if (SUCCEEDED(hr)) {
                            TV_LOG_DEBUG("Successfully created toast notification");
                            
                            // Set the notification's tag and group
                            Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotification2> toast2;
                            hr = toast.As(&toast2);
                            if (SUCCEEDED(hr)) {
                                HSTRING tag;
                                hr = WindowsCreateString(wCallSid.c_str(), static_cast<UINT32>(wCallSid.length()), &tag);
                                if (SUCCEEDED(hr)) {
                                    hr = toast2->put_Tag(tag);
                                    WindowsDeleteString(tag);
                                    if (SUCCEEDED(hr)) {
                                        TV_LOG_DEBUG("Successfully set toast tag");
                                    }
                                }
                            }
                            
                            // Show the notification
                            hr = toastNotifier->Show(toast.Get());
                            if (SUCCEEDED(hr)) {
                                TV_LOG_DEBUG("Successfully showed toast notification");
                                activeNotifications[callSid] = toast;
                            } else {
                                LPSTR errorMessage = nullptr;
                                FormatMessageA(
                                    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    NULL,
                                    hr,
                                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                    (LPSTR)&errorMessage,
                                    0,
                                    NULL
                                );
                                if (errorMessage) {
                                    TV_LOG_ERROR("Failed to show toast notification. HRESULT: " + std::to_string(hr) + ", Error: " + errorMessage);
                                    LocalFree(errorMessage);
                                } else {
                                    TV_LOG_ERROR("Failed to show toast notification. HRESULT: " + std::to_string(hr));
                                }
                            }
                        } else {
                            TV_LOG_ERROR("Failed to create toast notification. HRESULT: " + std::to_string(hr));
                        }
                    } else {
                        TV_LOG_ERROR("Failed to get toast notification factory. HRESULT: " + std::to_string(hr));
                    }
                } else {
                    TV_LOG_ERROR("Failed to load XML into document. HRESULT: " + std::to_string(hr));
                }
            } else {
                TV_LOG_ERROR("Failed to create Windows string from XML. HRESULT: " + std::to_string(hr));
            }
        } else {
            TV_LOG_ERROR("Failed to get XML document IO interface. HRESULT: " + std::to_string(hr));
        }
    } else {
        TV_LOG_ERROR("Failed to create XML document. HRESULT: " + std::to_string(hr));
    }
  }

  void TVNotificationManager::hideNotification(const std::string& callSid) {
    TV_LOG_DEBUG("Attempting to hide notification for call SID: " + callSid);
    
    if (toastNotifier) {
        auto it = activeNotifications.find(callSid);
        if (it != activeNotifications.end()) {
            TV_LOG_DEBUG("Found active notification, attempting to hide");
            HRESULT hr = toastNotifier->Hide(it->second.Get());
            if (SUCCEEDED(hr)) {
                TV_LOG_DEBUG("Successfully hid notification");
            } else {
                TV_LOG_ERROR("Failed to hide notification. HRESULT: " + std::to_string(hr));
            }
            activeNotifications.erase(it);
        } else {
            TV_LOG_DEBUG("No active notification found for call SID: " + callSid);
        }
    } else {
        TV_LOG_ERROR("Toast notifier not initialized");
    }
  }

  void TVNotificationManager::hideAllNotifications() {
    TV_LOG_DEBUG("Attempting to hide all notifications. Active count: " + std::to_string(activeNotifications.size()));
    
    if (toastNotifier) {
        for (const auto& [callSid, notification] : activeNotifications) {
            TV_LOG_DEBUG("Hiding notification for call SID: " + callSid);
            HRESULT hr = toastNotifier->Hide(notification.Get());
            if (SUCCEEDED(hr)) {
                TV_LOG_DEBUG("Successfully hid notification for call SID: " + callSid);
            } else {
                TV_LOG_ERROR("Failed to hide notification for call SID: " + callSid + ". HRESULT: " + std::to_string(hr));
            }
        }
        activeNotifications.clear();
        TV_LOG_DEBUG("Cleared all active notifications");
    } else {
        TV_LOG_ERROR("Toast notifier not initialized");
    }
  }

  HRESULT TVNotificationActivationCallback::Activate(
      LPCWSTR appUserModelId,
      LPCWSTR invokedArgs,
      const NOTIFICATION_USER_INPUT_DATA* data,
      ULONG count) {
      
      TV_LOG_DEBUG("Notification activation callback triggered");
      TV_LOG_DEBUG("AppUserModelId: " + (appUserModelId ? WideToUtf8(appUserModelId) : "null"));
      TV_LOG_DEBUG("InvokedArgs: " + (invokedArgs ? WideToUtf8(invokedArgs) : "null"));
      
      if (!invokedArgs) {
          TV_LOG_ERROR("No arguments provided to notification activation callback");
          return E_INVALIDARG;
      }

      // Parse the arguments which are in the format "action:callSid"
      std::wstring args(invokedArgs);
      TV_LOG_DEBUG("Parsing arguments: " + WideToUtf8(args));
      
      size_t colonPos = args.find(L':');
      if (colonPos == std::wstring::npos) {
          TV_LOG_ERROR("Invalid notification arguments format: " + WideToUtf8(args));
          return E_INVALIDARG;
      }

      std::wstring action = args.substr(0, colonPos);
      std::wstring callSid = args.substr(colonPos + 1);
      
      TV_LOG_DEBUG("Parsed action: " + WideToUtf8(action));
      TV_LOG_DEBUG("Parsed callSid: " + WideToUtf8(callSid));

      return HandleNotificationAction(action, callSid);
  }

  HRESULT TVNotificationActivationCallback::HandleNotificationAction(
      const std::wstring& action,
      const std::wstring& callSid) {
      
      TV_LOG_DEBUG("Handling notification action: " + WideToUtf8(action) + " for call SID: " + WideToUtf8(callSid));

      // Convert callSid to UTF-8 for the webview
      std::string utf8CallSid = WideToUtf8(callSid);

      if (action == L"accept") {
          TV_LOG_DEBUG("Action is 'accept', attempting to hide notification and accept call");
          // Hide the notification
          TVNotificationManager::getInstance().hideNotification(utf8CallSid);
          
          // Use the AnswerCall method with null result
          if (TVNotificationManager::webview_) {
              TV_LOG_DEBUG("Webview is available, calling AnswerCall");
              TwilioVoicePlugin::AnswerCall(TVNotificationManager::webview_, nullptr);
          } else {
              TV_LOG_ERROR("Webview is not available, cannot accept call");
          }
      } else if (action == L"reject") {
          TV_LOG_DEBUG("Action is 'reject', attempting to hide notification and reject call");
          // Hide the notification
          TVNotificationManager::getInstance().hideNotification(utf8CallSid);
          
          // Use the HangUpCall method with null result
          if (TVNotificationManager::webview_) {
              TV_LOG_DEBUG("Webview is available, calling HangUpCall");
              TwilioVoicePlugin::HangUpCall(TVNotificationManager::webview_, nullptr);
          } else {
              TV_LOG_ERROR("Webview is not available, cannot reject call");
          }
      } else {
          TV_LOG_ERROR("Unknown notification action: " + WideToUtf8(action));
          return E_INVALIDARG;
      }

      return S_OK;
  }

  bool TVNotificationManager::hasNotificationPermission() {
    // Check if the app has notification permission by trying to create a toast notifier
    if (!toastManager) {
        TV_LOG_ERROR("Toast manager not initialized");
        return false;
    }

    // Get the app's AUMID
    PWSTR aumid = nullptr;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (SUCCEEDED(hr)) {
        std::wstring wAumid(aumid);
        CoTaskMemFree(aumid);

        // Create HSTRING from AUMID
        HSTRING hAumid;
        hr = WindowsCreateString(wAumid.c_str(), static_cast<UINT32>(wAumid.length()), &hAumid);
        if (SUCCEEDED(hr)) {
            // Try to create the notifier
            Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotifier> testNotifier;
            hr = toastManager->CreateToastNotifierWithId(hAumid, &testNotifier);
            WindowsDeleteString(hAumid);

            if (SUCCEEDED(hr)) {
                TV_LOG_DEBUG("Notification permission granted");
                return true;
            } else {
                TV_LOG_ERROR("Notification permission denied. HRESULT: " + std::to_string(hr));
                return false;
            }
        }
    }
    return false;
  }

  bool TVNotificationManager::requestNotificationPermission() {
    // On Windows, notification permissions are typically granted through the app manifest
    // and system settings. We can't programmatically request them like on web.
    // Instead, we should guide the user to enable notifications in Windows settings.
    
    // Try to initialize the notification system
    return InitializeNotificationSystem();
  }

  bool TVNotificationManager::InitializeNotificationSystem() {
    if (!toastManager) {
        TV_LOG_ERROR("Toast manager not initialized");
        return false;
    }

    // Get the app's AUMID
    PWSTR aumid = nullptr;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (SUCCEEDED(hr)) {
        std::wstring wAumid(aumid);
        CoTaskMemFree(aumid);

        // Create HSTRING from AUMID
        HSTRING hAumid;
        hr = WindowsCreateString(wAumid.c_str(), static_cast<UINT32>(wAumid.length()), &hAumid);
        if (SUCCEEDED(hr)) {
            // Try to create the notifier
            hr = toastManager->CreateToastNotifierWithId(hAumid, &toastNotifier);
            WindowsDeleteString(hAumid);

            if (SUCCEEDED(hr)) {
                TV_LOG_DEBUG("Notification system initialized successfully");
                return true;
            } else {
                TV_LOG_ERROR("Failed to initialize notification system. HRESULT: " + std::to_string(hr));
                return false;
            }
        }
    }
    return false;
  }

  // Add static member function to register the COM server
  bool TVNotificationManager::RegisterCOMServer() {
    // Get the app's AUMID
    PWSTR aumid = nullptr;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (FAILED(hr)) {
        TV_LOG_ERROR("Failed to get AUMID for COM server registration. HRESULT: " + std::to_string(hr));
        return false;
    }

    std::wstring wAumid(aumid);
    CoTaskMemFree(aumid);
    TV_LOG_DEBUG("Registering COM server with AUMID: " + WideToUtf8(wAumid));

    // Verify if the notification activation callback is already registered
    HKEY hKey;
    std::wstring regPath = L"SOFTWARE\\Classes\\AppUserModelId\\" + wAumid + L"\\CustomActivator";
    hr = RegOpenKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, KEY_READ, &hKey);
    if (SUCCEEDED(hr)) {
        TV_LOG_DEBUG("Notification activation callback is already registered");
        RegCloseKey(hKey);
    } else {
        // Create the COM server registration
        hr = RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
        
        if (SUCCEEDED(hr)) {
            // Convert CLSID to string
            LPOLESTR clsidOleStr = nullptr;
            std::wstring clsidStr;
            hr = StringFromCLSID(CLSID_TVNotificationActivationCallback, &clsidOleStr);
            if (SUCCEEDED(hr)) {
                clsidStr = clsidOleStr;
                CoTaskMemFree(clsidOleStr);

                hr = RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, 
                    reinterpret_cast<const BYTE*>(L"Twilio Voice Notification Callback"), 
                    sizeof(L"Twilio Voice Notification Callback"));
                
                if (SUCCEEDED(hr)) {
                    hr = RegSetValueExW(hKey, L"CustomActivator", 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(clsidStr.c_str()),
                        static_cast<DWORD>((clsidStr.length() + 1) * sizeof(wchar_t)));
                    
                    if (SUCCEEDED(hr)) {
                        TV_LOG_DEBUG("Successfully registered notification activation callback");
                    } else {
                        TV_LOG_ERROR("Failed to set CustomActivator registry value. HRESULT: " + std::to_string(hr));
                    }
                } else {
                    TV_LOG_ERROR("Failed to set DisplayName registry value. HRESULT: " + std::to_string(hr));
                }
            } else {
                TV_LOG_ERROR("Failed to convert CLSID to string. HRESULT: " + std::to_string(hr));
            }
            RegCloseKey(hKey);
        } else {
            TV_LOG_ERROR("Failed to create registry key. HRESULT: " + std::to_string(hr));
        }
    }

    // Register the COM server
    Microsoft::WRL::ComPtr<IClassFactory> classFactory;
    hr = Microsoft::WRL::MakeAndInitialize<TVNotificationActivationCallbackFactory>(&classFactory);
    if (SUCCEEDED(hr)) {
        hr = CoRegisterClassObject(
            CLSID_TVNotificationActivationCallback,
            classFactory.Get(),
            CLSCTX_LOCAL_SERVER,
            REGCLS_MULTIPLEUSE,
            &comRegistrationCookie);
        
        if (SUCCEEDED(hr)) {
            TV_LOG_DEBUG("Successfully registered COM server with cookie: " + std::to_string(comRegistrationCookie));
            return true;
        } else {
            TV_LOG_ERROR("Failed to register COM server. HRESULT: " + std::to_string(hr));
        }
    } else {
        TV_LOG_ERROR("Failed to create class factory. HRESULT: " + std::to_string(hr));
    }

    return false;
  }

  void TVNotificationManager::UnregisterCOMServer() {
    if (comRegistrationCookie != 0) {
        HRESULT hr = CoRevokeClassObject(comRegistrationCookie);
        if (SUCCEEDED(hr)) {
            TV_LOG_DEBUG("Successfully unregistered COM server");
            comRegistrationCookie = 0;
        } else {
            TV_LOG_ERROR("Failed to unregister COM server. HRESULT: " + std::to_string(hr));
        }
    }
  }

} // namespace twilio_voice
