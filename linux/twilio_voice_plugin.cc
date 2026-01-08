#include "include/twilio_voice/twilio_voice_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>

#define TWILIO_VOICE_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), twilio_voice_plugin_get_type(), \
                              TwilioVoicePlugin))

// Channel names - must match Dart side
static constexpr char kMethodChannelName[] = "twilio_voice/messages";
static constexpr char kEventChannelName[] = "twilio_voice/events";

struct _TwilioVoicePlugin {
  GObject parent_instance;
  FlPluginRegistrar* registrar;
  FlMethodChannel* method_channel;
  FlEventChannel* event_channel;
  FlEventSink* event_sink;
};

G_DEFINE_TYPE(TwilioVoicePlugin, twilio_voice_plugin, g_object_get_type())

// Helper to log stub calls in debug builds
static void log_stub(const gchar* method) {
#ifdef DEBUG
  g_print("twilio_voice: %s called on Linux (stub - no-op)\n", method);
#endif
}

// Handle method calls from Flutter
static void twilio_voice_plugin_handle_method_call(
    TwilioVoicePlugin* self,
    FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);
  
  g_autoptr(FlMethodResponse) response = nullptr;

  // Methods that return bool (false for stub)
  if (strcmp(method, "tokens") == 0 ||
      strcmp(method, "unregister") == 0 ||
      strcmp(method, "registerClient") == 0 ||
      strcmp(method, "unregisterClient") == 0 ||
      strcmp(method, "defaultCaller") == 0 ||
      strcmp(method, "makeCall") == 0 ||
      strcmp(method, "hangUp") == 0 ||
      strcmp(method, "answer") == 0 ||
      strcmp(method, "holdCall") == 0 ||
      strcmp(method, "toggleMute") == 0 ||
      strcmp(method, "toggleSpeaker") == 0 ||
      strcmp(method, "toggleBluetooth") == 0 ||
      strcmp(method, "sendDigits") == 0 ||
      strcmp(method, "requestMicPermission") == 0 ||
      strcmp(method, "requestBackgroundPermissions") == 0 ||
      strcmp(method, "requestReadPhoneStatePermission") == 0 ||
      strcmp(method, "requestCallPhonePermission") == 0 ||
      strcmp(method, "requestManageOwnCallsPermission") == 0 ||
      strcmp(method, "requestReadPhoneNumbersPermission") == 0 ||
      strcmp(method, "registerPhoneAccount") == 0 ||
      strcmp(method, "openPhoneAccountSettings") == 0 ||
      strcmp(method, "updateCallKitIcon") == 0 ||
      strcmp(method, "show-notifications") == 0) {
    log_stub(method);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(FALSE)));
  }
  // Methods that check permissions/state (return false)
  else if (strcmp(method, "hasMicPermission") == 0 ||
           strcmp(method, "requiresBackgroundPermissions") == 0 ||
           strcmp(method, "hasRegisteredPhoneAccount") == 0 ||
           strcmp(method, "isPhoneAccountEnabled") == 0 ||
           strcmp(method, "hasReadPhoneStatePermission") == 0 ||
           strcmp(method, "hasCallPhonePermission") == 0 ||
           strcmp(method, "hasManageOwnCallsPermission") == 0 ||
           strcmp(method, "hasReadPhoneNumbersPermission") == 0 ||
           strcmp(method, "isOnCall") == 0 ||
           strcmp(method, "isMuted") == 0 ||
           strcmp(method, "isHolding") == 0 ||
           strcmp(method, "isOnSpeaker") == 0 ||
           strcmp(method, "isBluetoothOn") == 0 ||
           strcmp(method, "rejectCallOnNoPermissions") == 0 ||
           strcmp(method, "isRejectingCallOnNoPermissions") == 0) {
    log_stub(method);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_bool(FALSE)));
  }
  // Methods that return null/string
  else if (strcmp(method, "call-sid") == 0) {
    log_stub(method);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(fl_value_new_null()));
  }
  // Unknown method
  else {
    log_stub(method);
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

// Method channel call handler
static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  TwilioVoicePlugin* plugin = TWILIO_VOICE_PLUGIN(user_data);
  twilio_voice_plugin_handle_method_call(plugin, method_call);
}

// Event channel listen callback
static FlMethodErrorResponse* event_channel_listen_cb(
    FlEventChannel* channel,
    FlValue* args,
    gpointer user_data) {
  TwilioVoicePlugin* plugin = TWILIO_VOICE_PLUGIN(user_data);
  // Store the event sink for potential future use
  // Note: In a real implementation, you'd send events through this sink
  return nullptr;
}

// Event channel cancel callback
static FlMethodErrorResponse* event_channel_cancel_cb(
    FlEventChannel* channel,
    FlValue* args,
    gpointer user_data) {
  TwilioVoicePlugin* plugin = TWILIO_VOICE_PLUGIN(user_data);
  plugin->event_sink = nullptr;
  return nullptr;
}

static void twilio_voice_plugin_dispose(GObject* object) {
  TwilioVoicePlugin* self = TWILIO_VOICE_PLUGIN(object);
  
  g_clear_object(&self->method_channel);
  g_clear_object(&self->event_channel);
  
  G_OBJECT_CLASS(twilio_voice_plugin_parent_class)->dispose(object);
}

static void twilio_voice_plugin_class_init(TwilioVoicePluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = twilio_voice_plugin_dispose;
}

static void twilio_voice_plugin_init(TwilioVoicePlugin* self) {}

void twilio_voice_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  TwilioVoicePlugin* plugin = TWILIO_VOICE_PLUGIN(
      g_object_new(twilio_voice_plugin_get_type(), nullptr));
  
  plugin->registrar = registrar;
  
  // Setup method channel
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  plugin->method_channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      kMethodChannelName,
      FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      plugin->method_channel,
      method_call_cb,
      g_object_ref(plugin),
      g_object_unref);
  
  // Setup event channel
  plugin->event_channel = fl_event_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      kEventChannelName,
      FL_METHOD_CODEC(codec));
  fl_event_channel_set_stream_handlers(
      plugin->event_channel,
      event_channel_listen_cb,
      event_channel_cancel_cb,
      g_object_ref(plugin),
      g_object_unref);

  g_object_unref(plugin);
}

