#ifndef FLUTTER_PLUGIN_TWILIO_VOICE_PLUGIN_H_
#define FLUTTER_PLUGIN_TWILIO_VOICE_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/plugin_registrar_windows.h>
#include "webview/tv_webview.h"
#include "js_interop/call/tv_call.h"
#include <memory>

namespace twilio_voice {

class TwilioVoicePlugin : public flutter::Plugin, public TVCallDelegate {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  TwilioVoicePlugin(flutter::PluginRegistrarWindows* registrar);
  virtual ~TwilioVoicePlugin();

  // TVCallDelegate implementation
  void onCallAccept(TVCall* call) override;
  void onCallCancel(TVCall* call) override;
  void onCallDisconnect(TVCall* call) override;
  void onCallError(const TVError& error) override;
  void onCallReconnecting(const TVError& error) override;
  void onCallReconnected() override;
  void onCallReject() override;
  void onCallStatus(const TVCallStatus& status) override;

 private:
  std::unique_ptr<TVWebView> webview_;
  std::unique_ptr<TVCall> activeCall_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> event_channel_;
  std::unique_ptr<flutter::StreamHandler<flutter::EncodableValue>> stream_handler_;
  flutter::PluginRegistrarWindows* registrar_;
  flutter::EventSink<flutter::EncodableValue>* event_sink_ = nullptr;
  
  // Flag to track if SDK is ready
  bool sdk_ready_ = false;
  
  void InitializeWebView();
  void HandleCallEvent(const std::string& event, const std::string& data);
  void SendEventToFlutter(const std::string& event);

  // Mic permission handling
  bool CheckMicrophonePermission();
  bool RequestMicrophonePermission();

  // Disallow copy and assign.
  TwilioVoicePlugin(const TwilioVoicePlugin&) = delete;
  TwilioVoicePlugin& operator=(const TwilioVoicePlugin&) = delete;

  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

}  // namespace twilio_voice

#endif  // FLUTTER_PLUGIN_TWILIO_VOICE_PLUGIN_H_
