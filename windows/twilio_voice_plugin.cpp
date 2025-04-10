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

namespace twilio_voice
{
  class TVNotificationActivationCallbackFactory : public Microsoft::WRL::RuntimeClass<
                                                      Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                      IClassFactory>
  {
  public:
    TVNotificationActivationCallbackFactory() = default;
    ~TVNotificationActivationCallbackFactory() = default;

    IFACEMETHODIMP CreateInstance(
        IUnknown *outer,
        REFIID riid,
        void **ppv) override
    {
      if (outer != nullptr)
      {
        return CLASS_E_NOAGGREGATION;
      }

      Microsoft::WRL::ComPtr<TVNotificationActivationCallback> callback;
      HRESULT hr = Microsoft::WRL::MakeAndInitialize<TVNotificationActivationCallback>(&callback);
      if (SUCCEEDED(hr))
      {
        hr = callback->QueryInterface(riid, ppv);
      }
      return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
      return S_OK;
    }
  };

  TVWebView *TVNotificationManager::webview_ = nullptr;
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
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    std::wstring defaultAumid = L"SpaceAuto.App";
    hr = SetCurrentProcessExplicitAppUserModelID(defaultAumid.c_str());

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
        L"    if (window.device) {"
        L"      window.device.removeAllListeners('incoming');"
        L"      window.device.removeAllListeners('connect');"
        L"      window.device.removeAllListeners('disconnect');"
        L"      window.device.removeAllListeners('error');"
        L"      window.device.removeAllListeners('offline');"
        L"      window.device.removeAllListeners('ready');"
        L"    }"
        L"})()",
        [](void*, std::string error) {});

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
                    
                    size_t jsonEnd = utf8Message.find_last_of('}');
                    if (jsonEnd != std::string::npos) {
                        utf8Message = utf8Message.substr(0, jsonEnd + 1);
                    }
                    
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

                      if (eventValue == "incoming") {
                        CheckMicrophonePermission();
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        std::string callSid = json.value("callSid", "");
                        TVNotificationManager::getInstance().showIncomingCallNotification(from, callSid);
                        SendEventToFlutter("Incoming|" + from + "|" + to + "|Incoming");
                      } else if (eventValue == "cancel") {
                        // Handle missed call
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        std::string callSid = json.value("callSid", "");
                        // Hide the previous incoming call notification
                        TVNotificationManager::getInstance().hideNotification(callSid, true);
                        // Show missed call notification
                        TVNotificationManager::getInstance().showMissedCallNotification(from, callSid);
                        SendEventToFlutter("Missed Call");
                        SendEventToFlutter("Call Ended");
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

  TwilioVoicePlugin::~TwilioVoicePlugin()
  {
    if (webview_)
    {
      webview_.reset(); // This will call the destructor of TVWebView
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
                                           L"      call.on('accept', () => {"
                                           L"        window.chrome.webview.postMessage({"
                                           L"          type: 'call_event',"
                                           L"          event: 'accept',"
                                           L"          from: params.From,"
                                           L"          to: params.To,"
                                           L"          callSid: params.CallSid"
                                           L"        });"
                                           L"      });"
                                           L"      call.on('cancel', () => {"
                                           L"        window.chrome.webview.postMessage({"
                                           L"          type: 'call_event',"
                                           L"          event: 'cancel',"
                                           L"          from: params.From,"
                                           L"          to: params.To,"
                                           L"          callSid: params.CallSid"
                                           L"        });"
                                           L"      });"
                                           L"      call.on('disconnect', () => {"
                                           L"        window.chrome.webview.postMessage({"
                                           L"          type: 'call_event',"
                                           L"          event: 'disconnected'"
                                           L"        });"
                                           L"      });"
                                           L"      call.on('error', (error) => {"
                                           L"        window.chrome.webview.postMessage({"
                                           L"          type: 'call_event',"
                                           L"          event: 'error',"
                                           L"          error: error.message"
                                           L"        });"
                                           L"      });"
                                           L"      call.on('reject', () => {"
                                           L"        window.chrome.webview.postMessage({"
                                           L"          type: 'call_event',"
                                           L"          event: 'reject'"
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

      std::string from;
      std::string to;

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
      }

      if (from.empty() || to.empty())
      {
        result->Error("INVALID_ARGUMENTS", "From and To parameters are required");
        return;
      }

      MakeCall(webview_.get(), from, to, std::move(result));
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
    else if (method == "missedCall")
    {
      if (!webview_)
      {
        result->Error("NOT_READY", "WebView not initialized");
        return;
      }

      const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
      if (!arguments)
      {
        result->Error("INVALID_ARGUMENTS", "Invalid arguments for missedCall");
        return;
      }

      std::string from;
      std::string callSid;

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
        else if (keyStr == "CallSid")
        {
          callSid = valueStr;
        }
      }

      if (from.empty() || callSid.empty())
      {
        result->Error("INVALID_ARGUMENTS", "From and CallSid parameters are required");
        return;
      }

      TVNotificationManager::getInstance().showMissedCallNotification(from, callSid);
      result->Success(true);
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
    std::wstring checkPermissionScript = L"(async () => {"
                                         L"  try {"
                                         L"    const permission = await navigator.permissions.query({name:'microphone'});"
                                         L"    return permission.state === 'granted';"
                                         L"  } catch (error) {"
                                         L"    return false;"
                                         L"  }"
                                         L"})()";

    if (result)
    {
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview_->evaluateJavaScript(
          checkPermissionScript,
          [shared_result](void *, std::string response)
          {
            try
            {
              bool hasPermission = response == "true";
              (*shared_result)->Success(hasPermission);
            }
            catch (const std::exception &e)
            {
              TV_LOG_ERROR("Error parsing microphone permission response: " + std::string(e.what()));
              (*shared_result)->Success(false);
            }
          });
    }
    else
    {
      webview_->evaluateJavaScript(
          checkPermissionScript,
          [](void *, std::string response)
          {
            try
            {
              bool hasPermission = response == "true";
              TV_LOG_DEBUG("Microphone permission status: " + std::string(hasPermission ? "granted" : "denied"));
            }
            catch (const std::exception &e)
            {
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

  void TwilioVoicePlugin::AnswerCall(TVWebView *webview, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    TV_LOG_DEBUG("Executing answer command");

    if (webview)
    {
      webview->evaluateJavaScript(
          L"window.connection ? window.connection.parameters.CallSid : ''",
          [](void *, std::string callSid)
          {
            if (!callSid.empty() && callSid != "\"\"")
            {
              TVNotificationManager::getInstance().hideNotification(callSid, true);
            }
          });
    }

    if (!webview)
    {
      if (result)
      {
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

    if (result)
    {
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview->evaluateJavaScript(
          answer_script,
          [shared_result](void *, std::string response)
          {
            if (response == "true")
            {
              if (*shared_result)
              {
                (*shared_result)->Success(true);
              }
            }
            else if (response == "false")
            {
              TV_LOG_ERROR("No active connection to answer");
              if (*shared_result)
              {
                (*shared_result)->Success(false);
              }
            }
            else
            {
              TV_LOG_ERROR("Answer error: " + response);
              if (*shared_result)
              {
                (*shared_result)->Error("ANSWER_FAILED", "Failed to answer call: " + response);
              }
            }
          });
    }
    else
    {
      webview->evaluateJavaScript(
          answer_script,
          [](void *, std::string response)
          {
            if (response == "true")
            {
              TV_LOG_DEBUG("Successfully answered call from notification");
            }
            else if (response == "false")
            {
              TV_LOG_ERROR("No active connection to answer from notification");
            }
            else
            {
              TV_LOG_ERROR("Answer error from notification: " + response);
            }
          });
    }
  }

  void TwilioVoicePlugin::HangUpCall(TVWebView *webview, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    TV_LOG_DEBUG("Executing hangUp command");

    if (webview)
    {
      webview->evaluateJavaScript(
          L"window.connection ? window.connection.parameters.CallSid : ''",
          [](void *, std::string callSid)
          {
            TV_LOG_DEBUG("HangUp CallSid: " + callSid);
            if (!callSid.empty() && callSid != "\"\"")
            {
              if (callSid.front() == '"' && callSid.back() == '"')
              {
                callSid = callSid.substr(1, callSid.length() - 2);
              }
              TV_LOG_DEBUG("HangUp CallSid hideNotification: " + callSid);
              TVNotificationManager::getInstance().hideNotification(callSid, true);
            }
          });
    }

    if (!webview)
    {
      if (result)
      {
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

    if (result)
    {
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview->evaluateJavaScript(
          disconnect_script,
          [shared_result](void *, std::string response)
          {
            if (response == "true")
            {
              if (*shared_result)
              {
                (*shared_result)->Success(true);
              }
            }
            else if (response == "false")
            {
              if (*shared_result)
              {
                (*shared_result)->Success(false);
              }
            }
            else
            {
              TV_LOG_ERROR("Hangup error: " + response);
              if (*shared_result)
              {
                (*shared_result)->Error("HANGUP_FAILED", "Failed to hang up call: " + response);
              }
            }
          });
    }
    else
    {
      webview->evaluateJavaScript(
          disconnect_script,
          [](void *, std::string response)
          {
            if (response == "true")
            {
            }
            else if (response == "false")
            {
              TV_LOG_ERROR("No active connection to hang up from notification");
            }
            else
            {
              TV_LOG_ERROR("Hangup error from notification: " + response);
            }
          });
    }
  }

  void TwilioVoicePlugin::MakeCall(TVWebView *webview, const std::string &from, const std::string &to, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    TV_LOG_DEBUG("Executing makeCall command");

    if (!webview)
    {
      if (result)
      {
        result->Error("NOT_READY", "WebView not initialized");
      }
      return;
    }

    std::wstring wfrom(from.begin(), from.end());
    std::wstring wto(to.begin(), to.end());

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
                           L"      To: '" + wto + L"',"
                           L"      From: '" + wfrom + L"'"
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
                           L"      event: 'cancel',"
                           L"      from: window.connection.parameters.From,"
                           L"      to: window.connection.parameters.To,"
                           L"      callSid: window.connection.parameters.CallSid"
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

    if (result)
    {
      auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
          std::move(result));

      webview->evaluateJavaScript(
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
    else
    {
      webview->evaluateJavaScript(
          js_code,
          [](void *, std::string response)
          {
            if (response != "{}")
            {
              TV_LOG_ERROR("Call error from notification: " + response);
            }
          });
    }
  }

  TVNotificationManager::TVNotificationManager()
  {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    hr = RoInitialize(RO_INIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<TVNotificationActivationCallback> callback;
    hr = Microsoft::WRL::MakeAndInitialize<TVNotificationActivationCallback>(&callback);
    if (SUCCEEDED(hr))
    {
      PWSTR aumid = nullptr;
      hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
      if (SUCCEEDED(hr))
      {
        std::wstring regPath = L"SOFTWARE\\Classes\\AppUserModelId\\" + std::wstring(aumid);
        HKEY hKey;
        hr = RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
        if (SUCCEEDED(hr))
        {
          LPOLESTR clsidOleStr;
          std::wstring clsidStr;
          hr = StringFromCLSID(CLSID_TVNotificationActivationCallback, &clsidOleStr);
          if (SUCCEEDED(hr))
          {
            clsidStr = clsidOleStr;
            CoTaskMemFree(clsidOleStr);

            hr = RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ,
                                reinterpret_cast<const BYTE *>(L"Space Auto"),
                                sizeof(L"Space Auto"));

            if (SUCCEEDED(hr))
            {
              hr = RegSetValueExW(hKey, L"CustomActivator", 0, REG_SZ,
                                  reinterpret_cast<const BYTE *>(clsidStr.c_str()),
                                  static_cast<DWORD>((clsidStr.length() + 1) * sizeof(wchar_t)));
            }
          }
          RegCloseKey(hKey);
        }
        CoTaskMemFree(aumid);
      }
    }

    wchar_t module_path[MAX_PATH];
    GetModuleFileNameW(NULL, module_path, MAX_PATH);
    std::wstring path(module_path);
    size_t lastBackslash = path.find_last_of(L"\\");
    std::wstring appName = path.substr(lastBackslash + 1);
    size_t dotPos = appName.find_last_of(L".");
    if (dotPos != std::wstring::npos)
    {
      appName = appName.substr(0, dotPos);
    }

    PWSTR aumid = nullptr;
    hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (SUCCEEDED(hr))
    {
      std::wstring wAumid(aumid);
      CoTaskMemFree(aumid);
    }
    else
    {
      std::wstring defaultAumid = appName + L".App";
      hr = SetCurrentProcessExplicitAppUserModelID(defaultAumid.c_str());
      if (SUCCEEDED(hr))
      {
        PWSTR verifyAumid = nullptr;
        hr = GetCurrentProcessExplicitAppUserModelID(&verifyAumid);
        if (SUCCEEDED(hr))
        {
          std::wstring wVerifyAumid(verifyAumid);
          CoTaskMemFree(verifyAumid);
        }
      }
    }

    hr = Windows::Foundation::GetActivationFactory(
        Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
        &toastManager);
    if (SUCCEEDED(hr))
    {
      std::wstring shortcutPath = L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\" + appName + L".lnk";
      wchar_t expandedPath[MAX_PATH];
      ExpandEnvironmentStringsW(shortcutPath.c_str(), expandedPath, MAX_PATH);
      shortcutPath = expandedPath;

      PWSTR currentAumid = nullptr;
      hr = GetCurrentProcessExplicitAppUserModelID(&currentAumid);
      if (SUCCEEDED(hr))
      {
        std::wstring wCurrentAumid(currentAumid);
        CoTaskMemFree(currentAumid);

        const int maxRetries = 3;
        int retryCount = 0;
        bool success = false;

        while (retryCount < maxRetries && !success)
        {
          HSTRING hAumid;
          hr = WindowsCreateString(wCurrentAumid.c_str(), static_cast<UINT32>(wCurrentAumid.length()), &hAumid);
          if (SUCCEEDED(hr))
          {
            hr = toastManager->CreateToastNotifierWithId(hAumid, &toastNotifier);
            WindowsDeleteString(hAumid);

            if (SUCCEEDED(hr))
            {
              success = true;
            }
            else
            {
              LPSTR errorMessage = nullptr;
              FormatMessageA(
                  FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  hr,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPSTR)&errorMessage,
                  0,
                  NULL);

              if (errorMessage)
              {
                LocalFree(errorMessage);
              }
            }
          }
        }
      }
    }
  }

  TVNotificationManager &TVNotificationManager::getInstance()
  {
    static TVNotificationManager instance;
    return instance;
  }

  void TVNotificationManager::showIncomingCallNotification(const std::string &from, const std::string &callSid)
  {
    if (!toastNotifier)
    {
      return;
    }

    if (!hasNotificationPermission())
    {
      TV_LOG_ERROR("Windows notification permission not granted");
      if (!requestNotificationPermission())
      {
        TV_LOG_ERROR("Failed to request notification permission");
        return;
      }
    }

    ShowNotificationInternal(from, callSid, true);
  }

  void TVNotificationManager::showMissedCallNotification(const std::string &from, const std::string &callSid)
  {
    if (!toastNotifier)
    {
      return;
    }

    if (!hasNotificationPermission())
    {
      TV_LOG_ERROR("Windows notification permission not granted");
      if (!requestNotificationPermission())
      {
        TV_LOG_ERROR("Failed to request notification permission");
        return;
      }
    }

    ShowNotificationInternal(from, callSid, false);
  }

  void TVNotificationManager::ShowNotificationInternal(const std::string &from, const std::string &callSid, bool isIncomingCall)
  {
    Microsoft::WRL::ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocument> xmlDoc;
    HRESULT hr = Windows::Foundation::ActivateInstance(
        Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
        &xmlDoc);

    if (SUCCEEDED(hr))
    {
      std::wstring wFrom;
      if (!from.empty())
      {
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, nullptr, 0);
        if (wideLength > 0)
        {
          wFrom.resize(wideLength - 1);
          MultiByteToWideChar(CP_UTF8, 0, from.c_str(), -1, &wFrom[0], wideLength);
        }
      }
      else
      {
        wFrom = L"Unknown Caller";
      }

      int wideLength = MultiByteToWideChar(CP_UTF8, 0, callSid.c_str(), -1, nullptr, 0);
      std::wstring wCallSid;
      if (wideLength > 0)
      {
        wCallSid.resize(wideLength - 1);
        MultiByteToWideChar(CP_UTF8, 0, callSid.c_str(), -1, &wCallSid[0], wideLength);
      }

      // Store the notification arguments for later use
      lastNotificationArgs_ = L"from:" + wFrom + L"|to:" + wCallSid;

      std::wstring title = isIncomingCall ? L"Incoming Call" : L"Missed Call";
      std::wstring scenario = isIncomingCall ? L" scenario='alarm' silent='true'" : L"";
      std::wstring xml = L"<toast" + scenario + L">"
                         L"<visual><binding template='ToastGeneric'>"
                         L"<text>" + title + L"</text>"
                         L"<text>" + wFrom + L"</text>"
                         L"</binding></visual>"
                         L"<actions>";

      if (isIncomingCall)
      {
        xml += L"<action content='Accept' arguments='accept:" + wCallSid + L"' activationType='foreground'/>"
               L"<action content='Reject' arguments='reject:" + wCallSid + L"' activationType='foreground'/>";
      }
      else
      {
        xml += L"<action content='Call Back' arguments='call:" + wCallSid + L"' activationType='foreground'/>";
      }

      xml += L"</actions>"
             L"</toast>";

      Microsoft::WRL::ComPtr<ABI::Windows::Data::Xml::Dom::IXmlDocumentIO> xmlDocIO;
      hr = xmlDoc.As(&xmlDocIO);
      if (SUCCEEDED(hr))
      {
        HSTRING xmlStr;
        hr = WindowsCreateString(xml.c_str(), static_cast<UINT32>(xml.length()), &xmlStr);
        if (SUCCEEDED(hr))
        {
          hr = xmlDocIO->LoadXml(xmlStr);
          WindowsDeleteString(xmlStr);

          if (SUCCEEDED(hr))
          {
            Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotificationFactory> toastFactory;
            hr = Windows::Foundation::GetActivationFactory(
                Microsoft::WRL::Wrappers::HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
                &toastFactory);
            if (SUCCEEDED(hr))
            {
              Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotification> toast;
              hr = toastFactory->CreateToastNotification(xmlDoc.Get(), &toast);
              if (SUCCEEDED(hr))
              {
                Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotification2> toast2;
                hr = toast.As(&toast2);
                if (SUCCEEDED(hr))
                {
                  HSTRING tag;
                  hr = WindowsCreateString(wCallSid.c_str(), static_cast<UINT32>(wCallSid.length()), &tag);
                  if (SUCCEEDED(hr))
                  {
                    hr = toast2->put_Tag(tag);
                    WindowsDeleteString(tag);
                  }
                }

                hr = toastNotifier->Show(toast.Get());
                if (SUCCEEDED(hr))
                {
                  NotificationInfo info;
                  info.notification = toast;
                  info.isIncomingCall = isIncomingCall;
                  activeNotifications[callSid] = info;
                }
                else
                {
                  LPSTR errorMessage = nullptr;
                  FormatMessageA(
                      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL,
                      hr,
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      (LPSTR)&errorMessage,
                      0,
                      NULL);
                  if (errorMessage)
                  {
                    LocalFree(errorMessage);
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  void TVNotificationManager::hideNotification(const std::string &callSid, bool isIncomingCall)
  {
    if (toastNotifier)
    {
      auto it = activeNotifications.find(callSid);
      if (it != activeNotifications.end() && it->second.isIncomingCall == isIncomingCall)
      {
        toastNotifier->Hide(it->second.notification.Get());
        activeNotifications.erase(it);
      }
    }
  }

  void TVNotificationManager::hideAllNotifications()
  {
    if (toastNotifier)
    {
      for (const auto &[callSid, info] : activeNotifications)
      {
        toastNotifier->Hide(info.notification.Get());
      }
      activeNotifications.clear();
    }
  }

  HRESULT TVNotificationActivationCallback::Activate(
      LPCWSTR appUserModelId,
      LPCWSTR invokedArgs,
      const NOTIFICATION_USER_INPUT_DATA *data,
      ULONG count)
  {
    if (!invokedArgs)
    {
      return E_INVALIDARG;
    }

    std::wstring args(invokedArgs);

    size_t colonPos = args.find(L':');
    if (colonPos == std::wstring::npos)
    {
      return E_INVALIDARG;
    }

    std::wstring action = args.substr(0, colonPos);

    return HandleNotificationAction(action);
  }

  HRESULT TVNotificationActivationCallback::HandleNotificationAction(
      const std::wstring &action)
  {
    if (action == L"accept")
    {
      if (TVNotificationManager::webview_)
      {
        TwilioVoicePlugin::AnswerCall(TVNotificationManager::webview_, nullptr);
      }
    }
    else if (action == L"reject")
    {
      if (TVNotificationManager::webview_)
      {
        TwilioVoicePlugin::HangUpCall(TVNotificationManager::webview_, nullptr);
      }
    }
    else if (action == L"call")
    {
      if (TVNotificationManager::webview_)
      {
        // Extract from and to from the notification arguments
        std::wstring args = TVNotificationManager::getInstance().getLastNotificationArgs();
        size_t fromPos = args.find(L"from:");
        size_t toPos = args.find(L"to:");
        if (fromPos != std::wstring::npos && toPos != std::wstring::npos)
        {
          std::wstring wfrom = args.substr(fromPos + 5, args.find(L"|", fromPos) - fromPos - 5);
          std::wstring wto = args.substr(toPos + 3, args.find(L"|", toPos) - toPos - 3);
          
          // Convert wide strings to UTF-8 strings
          int fromLength = WideCharToMultiByte(CP_UTF8, 0, wfrom.c_str(), -1, nullptr, 0, nullptr, nullptr);
          int toLength = WideCharToMultiByte(CP_UTF8, 0, wto.c_str(), -1, nullptr, 0, nullptr, nullptr);
          
          if (fromLength > 0 && toLength > 0)
          {
            std::string from(fromLength - 1, '\0');
            std::string to(toLength - 1, '\0');
            
            WideCharToMultiByte(CP_UTF8, 0, wfrom.c_str(), -1, &from[0], fromLength, nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, wto.c_str(), -1, &to[0], toLength, nullptr, nullptr);
            
            TwilioVoicePlugin::MakeCall(TVNotificationManager::webview_, from, to, nullptr);
          }
        }
      }
    }
    else
    {
      return E_INVALIDARG;
    }

    return S_OK;
  }

  bool TVNotificationManager::hasNotificationPermission()
  {
    if (!toastManager)
    {
      TV_LOG_ERROR("Toast manager not initialized");
      return false;
    }

    PWSTR aumid = nullptr;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (SUCCEEDED(hr))
    {
      std::wstring wAumid(aumid);
      CoTaskMemFree(aumid);

      HSTRING hAumid;
      hr = WindowsCreateString(wAumid.c_str(), static_cast<UINT32>(wAumid.length()), &hAumid);
      if (SUCCEEDED(hr))
      {
        Microsoft::WRL::ComPtr<ABI::Windows::UI::Notifications::IToastNotifier> testNotifier;
        hr = toastManager->CreateToastNotifierWithId(hAumid, &testNotifier);
        WindowsDeleteString(hAumid);

        if (SUCCEEDED(hr))
        {
          return true;
        }
        else
        {
          return false;
        }
      }
    }
    return false;
  }

  bool TVNotificationManager::requestNotificationPermission()
  {
    // On Windows, notification permissions are typically granted through the app manifest
    // and system settings. We can't programmatically request them like on web.
    // Instead, we should guide the user to enable notifications in Windows settings.

    // Try to initialize the notification system
    return InitializeNotificationSystem();
  }

  bool TVNotificationManager::InitializeNotificationSystem()
  {
    if (!toastManager)
    {
      return false;
    }

    PWSTR aumid = nullptr;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&aumid);
    if (SUCCEEDED(hr))
    {
      std::wstring wAumid(aumid);
      CoTaskMemFree(aumid);

      HSTRING hAumid;
      hr = WindowsCreateString(wAumid.c_str(), static_cast<UINT32>(wAumid.length()), &hAumid);
      if (SUCCEEDED(hr))
      {
        hr = toastManager->CreateToastNotifierWithId(hAumid, &toastNotifier);
        WindowsDeleteString(hAumid);

        if (SUCCEEDED(hr))
        {
          return true;
        }
        else
        {
          return false;
        }
      }
    }
    return false;
  }

  bool TVNotificationManager::RegisterCOMServer()
  {
    Microsoft::WRL::ComPtr<IClassFactory> classFactory;
    HRESULT hr = Microsoft::WRL::MakeAndInitialize<TVNotificationActivationCallbackFactory>(&classFactory);
    if (SUCCEEDED(hr))
    {
      hr = CoRegisterClassObject(
          CLSID_TVNotificationActivationCallback,
          classFactory.Get(),
          CLSCTX_LOCAL_SERVER,
          REGCLS_MULTIPLEUSE,
          &comRegistrationCookie);

      if (SUCCEEDED(hr))
      {
        return true;
      }
    }

    return false;
  }

  void TVNotificationManager::UnregisterCOMServer()
  {
    if (comRegistrationCookie != 0)
    {
      HRESULT hr = CoRevokeClassObject(comRegistrationCookie);
      if (SUCCEEDED(hr))
      {
        comRegistrationCookie = 0;
      }
    }
  }

  std::wstring TVNotificationManager::getLastNotificationArgs() const
  {
    return lastNotificationArgs_;
  }

} // namespace twilio_voice
