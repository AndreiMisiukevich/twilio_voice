// TODO
// - add twilio parameter interpretation
// - create contact with twi:// from twilio parameters

package com.twilio.twilio_voice.service

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.BroadcastReceiver
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.telecom.CallAudioState
import android.telecom.Connection
import android.telecom.DisconnectCause
import android.telecom.StatusHints
import android.util.Log
import android.graphics.drawable.Icon
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.twilio.twilio_voice.R
import com.twilio.twilio_voice.call.TVParameters
import com.twilio.twilio_voice.receivers.TVBroadcastReceiver
import com.twilio.twilio_voice.types.CallAudioStateExtension.copyWith
import com.twilio.twilio_voice.types.CallDirection
import com.twilio.twilio_voice.types.CallExceptionExtension.toBundle
import com.twilio.twilio_voice.types.CompletionHandler
import com.twilio.twilio_voice.types.ContextExtension.hasMicrophoneAccess
import com.twilio.twilio_voice.types.TVNativeCallActions
import com.twilio.twilio_voice.types.TVNativeCallEvents
import com.twilio.twilio_voice.types.ValueBundleChanged
import com.twilio.voice.Call
import com.twilio.voice.CallException
import com.twilio.voice.CallInvite
import com.twilio.twilio_voice.types.ContextExtension
import android.app.NotificationChannel
import android.app.NotificationManager
import androidx.core.app.NotificationCompat
import android.app.PendingIntent
import android.app.Notification


class TVCallInviteConnection(
    ctx: Context,
    ci: CallInvite,
    callParams: TVParameters,
    onEvent: ValueBundleChanged<String>? = null,
    onAction: ValueBundleChanged<String>? = null,
    onDisconnected: CompletionHandler<DisconnectCause>? = null
) : TVCallConnection(ctx, onEvent, onAction, onDisconnected) {

    override val TAG = "VoipCallInviteConnection"
    private val callInvite: CallInvite
    override val callDirection = CallDirection.INCOMING

    init {
        callInvite = ci
        setCallParameters(callParams)
    }

    override fun onAnswer() {
        Log.d(TAG, "onAnswer: onAnswer")
        Log.i("TwilioVoiceDebug", "TVCallInviteConnection onAnswer called - calling super.onAnswer()")
        super.onAnswer()
        Log.i("TwilioVoiceDebug", "After super.onAnswer() - now accepting CallInvite")
        twilioCall = callInvite.accept(context, this)
        onAction?.onChange(TVNativeCallActions.ACTION_ANSWERED, Bundle().apply {
            putParcelable(TVBroadcastReceiver.EXTRA_CALL_INVITE, callInvite)
            putInt(TVBroadcastReceiver.EXTRA_CALL_DIRECTION, callDirection.id)
        })
    }

    fun acceptInvite() {
        Log.d(TAG, "acceptInvite: acceptInvite")
        Log.i("TwilioVoiceDebug", "ACCEPT INVITE CALLED - Setting up audio mode for background call")
        onAnswer()
    }

    fun rejectInvite() {
        Log.d(TAG, "rejectInvite: rejectInvite")
        onReject()
    }

    override fun onReject() {
        Log.d(TAG, "onReject: onReject")
        super.onReject()
        callInvite.reject(context)
        // if the call was answered, then immediately rejected/ended, we need to disconnect the call also
        twilioCall?.let {
            Log.d(TAG, "onReject: disconnecting call")
            it.disconnect()
        }
        onEvent?.onChange(TVNativeCallEvents.EVENT_DISCONNECTED_LOCAL, null)
        onDisconnected?.withValue(DisconnectCause(DisconnectCause.REJECTED))
        onAction?.onChange(TVNativeCallActions.ACTION_REJECTED, null)
        setDisconnected(DisconnectCause(DisconnectCause.REJECTED))
        destroy()
    }
}

open class TVCallConnection(
    ctx: Context,
    onEvent: ValueBundleChanged<String>? = null,
    onAction: ValueBundleChanged<String>? = null,
    onDisconnected: CompletionHandler<DisconnectCause>? = null,
) : Connection(), Call.Listener {

    open val TAG = "VoipConnection"
    val context: Context
    var twilioCall: Call? = null
    var onDisconnected: CompletionHandler<DisconnectCause>? = null
    var onEvent: ValueBundleChanged<String>? = null
    var onAction: ValueBundleChanged<String>? = null
    private var onCallStateListener: CompletionHandler<Call.State>? = null
    open val callDirection = CallDirection.OUTGOING
    private var callParams: TVParameters? = null
    
    // Screen state monitoring for mid-call lock/unlock
    private var screenStateReceiver: BroadcastReceiver? = null
    private var isCallActive = false

    init {
        context = ctx
        this.onDisconnected = onDisconnected
        this.onEvent = onEvent
        this.onAction = onAction
        audioModeIsVoip = true
        connectionCapabilities = CAPABILITY_MUTE or CAPABILITY_HOLD or CAPABILITY_SUPPORT_HOLD
    }

    fun setOnCallDisconnected(handler: CompletionHandler<DisconnectCause>) {
        onDisconnected = handler
    }

    fun setOnCallEventListener(listener: ValueBundleChanged<String>) {
        onEvent = listener
    }

    fun setOnCallActionListener(listener: ValueBundleChanged<String>) {
        onAction = listener
    }

    fun setOnCallStateListener(listener: CompletionHandler<Call.State>) {
        onCallStateListener = listener
    }

    fun setCallParameters(params: TVParameters) {
        callParams = params
    }

    fun getCallParameters(): TVParameters? {
        return callParams
    }

    private fun setupScreenStateMonitoring() {
        if (screenStateReceiver == null) {
            screenStateReceiver = object : BroadcastReceiver() {
                override fun onReceive(context: Context?, intent: Intent?) {
                    when (intent?.action) {
                        Intent.ACTION_SCREEN_ON -> {
                            Log.i("TwilioVoiceDebug", "Screen unlocked during call - re-applying microphone workarounds")
                            if (isCallActive) {
                                // Re-apply audio mode when screen is unlocked during call
                                try {
                                    val audioManager = context?.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
                                    audioManager?.mode = AudioManager.MODE_IN_COMMUNICATION
                                    Log.i("TwilioVoiceDebug", "Audio mode re-applied after screen unlock")
                                    
                                    // Also try to bring app to foreground
                                    try {
                                        val packageName = context?.packageName
                                        val launchIntent = context?.packageManager?.getLaunchIntentForPackage(packageName ?: "")
                                        if (launchIntent != null) {
                                            launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                                            launchIntent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
                                            launchIntent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
                                            context?.startActivity(launchIntent)
                                            Log.i("TwilioVoiceDebug", "App brought to foreground after screen unlock")
                                        }
                                    } catch (e: Exception) {
                                        Log.e("TwilioVoiceDebug", "Failed to bring app to foreground after unlock", e)
                                    }
                                } catch (e: Exception) {
                                    Log.e("TwilioVoiceDebug", "Failed to re-apply audio mode after screen unlock", e)
                                }
                            }
                        }
                        Intent.ACTION_SCREEN_OFF -> {
                            Log.i("TwilioVoiceDebug", "Screen locked during call")
                        }
                    }
                }
            }
            
            // Register receiver for screen state changes
            try {
                val filter = IntentFilter().apply {
                    addAction(Intent.ACTION_SCREEN_ON)
                    addAction(Intent.ACTION_SCREEN_OFF)
                }
                context.registerReceiver(screenStateReceiver, filter)
                Log.i("TwilioVoiceDebug", "Screen state monitoring registered")
            } catch (e: Exception) {
                Log.e("TwilioVoiceDebug", "Failed to register screen state receiver", e)
            }
        }
    }
    
    private fun cleanupScreenStateMonitoring() {
        screenStateReceiver?.let { receiver ->
            try {
                context.unregisterReceiver(receiver)
                Log.i("TwilioVoiceDebug", "Screen state monitoring unregistered")
            } catch (e: Exception) {
                Log.e("TwilioVoiceDebug", "Failed to unregister screen state receiver", e)
            }
            screenStateReceiver = null
        }
        isCallActive = false
    }

    //region Call.Listener
    /**
     * The call failed to connect.
     *
     *
     * Calls that fail to connect will result in [Call.Listener.onConnectFailure]
     * and always return a [CallException] providing more information about what failure occurred.
     *
     *
     * @param call          An object model representing a call that failed to connect.
     * @param callException CallException that describes why the connect failed.
     */
    override fun onConnectFailure(call: Call, callException: CallException) {
        Log.d(TAG, "onConnectFailure: onConnectFailure")
        twilioCall = null
        val rejectedErrorCodeList = listOf(
            31600, // Call invite rejected
        )
        val disconnectCauseCode = if (rejectedErrorCodeList.contains(callException.errorCode)) {
            DisconnectCause.REJECTED
        } else {
            DisconnectCause.ERROR
        }
        val disconnectCause = DisconnectCause(disconnectCauseCode, callException.message);
        this@TVCallConnection.setDisconnected(disconnectCause)
        onDisconnected?.withValue(disconnectCause)
        onEvent?.onChange(TVNativeCallEvents.EVENT_CONNECT_FAILURE, callException.toBundle())
        onCallStateListener?.withValue(call.state)
    }

    /**
     * Emitted once before the [Call.Listener.onConnected] callback. If
     * `answerOnBridge` is true, this represents the callee being alerted of a call.
     *
     * The [Call.getSid] is now available.
     *
     * @param call  An object model representing a call.
     */
    override fun onRinging(call: Call) {
        twilioCall = call

        when (callDirection) {
            CallDirection.INCOMING -> {
                setRinging()
            }
            CallDirection.OUTGOING -> {
                setInitialized()
            }
        }
        onCallStateListener?.withValue(call.state)
        onEvent?.onChange(TVNativeCallEvents.EVENT_RINGING, Bundle().apply {
            putString(TVBroadcastReceiver.EXTRA_CALL_HANDLE, callParams?.callSid)
            putString(TVBroadcastReceiver.EXTRA_CALL_FROM, callParams?.fromRaw)
            putString(TVBroadcastReceiver.EXTRA_CALL_TO, callParams?.toRaw)
            putInt(TVBroadcastReceiver.EXTRA_CALL_DIRECTION, callDirection.id)
        })
    }

    override fun onConnected(call: Call) {
        Log.d(TAG, "onConnected: onConnected")
        
        // Mark call as active and start monitoring screen state
        isCallActive = true
        setupScreenStateMonitoring()
        
        // CRITICAL: Set audio mode again when call connects to ensure proper routing
        try {
            val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
            Log.d(TAG, "Audio mode set to MODE_IN_COMMUNICATION in onConnected")
            Log.i("TwilioVoiceDebug", "Audio mode set to MODE_IN_COMMUNICATION in onConnected")
            
            // CRITICAL: Bring app to foreground to bypass Android 15 microphone restrictions
            try {
                Log.i("TwilioVoiceDebug", "Attempting to bring app to foreground for microphone access")
                
                // For locked screens, use full-screen intent notification (proper way for calls)
                try {
                    val notificationManager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                    
                    // Create a full-screen intent that can bypass lock screen
                    val packageName = context.packageName
                    val launchIntent = context.packageManager.getLaunchIntentForPackage(packageName)
                    if (launchIntent != null) {
                        // Add special flags for call handling
                        launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        launchIntent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
                        launchIntent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP)
                        launchIntent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
                        
                        // Create full-screen intent for locked screens
                        val fullScreenIntent = PendingIntent.getActivity(
                            context,
                            0,
                            launchIntent,
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                            } else {
                                PendingIntent.FLAG_UPDATE_CURRENT
                            }
                        )
                        
                        // Create notification channel for call notifications
                        val channelId = "incoming_call_channel"
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                            val channel = NotificationChannel(
                                channelId,
                                "Incoming Calls",
                                NotificationManager.IMPORTANCE_HIGH
                            ).apply {
                                description = "Notifications for incoming voice calls"
                                setBypassDnd(true)
                                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                            }
                            notificationManager.createNotificationChannel(channel)
                        }
                        
                        // Create full-screen notification for calls (works on locked screens)
                        val notification = NotificationCompat.Builder(context, channelId)
                            .setContentTitle("Incoming Call")
                            .setContentText("Voice call in progress")
                            .setSmallIcon(android.R.drawable.sym_call_incoming)
                            .setPriority(NotificationCompat.PRIORITY_HIGH)
                            .setCategory(NotificationCompat.CATEGORY_CALL)
                            .setOngoing(true)
                            .setAutoCancel(false)
                            .setFullScreenIntent(fullScreenIntent, true) // This is key for lock screen
                            .build()
                        
                        // Show the full-screen notification
                        notificationManager.notify(1001, notification)
                        Log.i("TwilioVoiceDebug", "Full-screen intent notification created for locked screen")
                        
                        // Also try regular app launch for unlocked screens
                        try {
                            context.startActivity(launchIntent)
                            Log.i("TwilioVoiceDebug", "App launch attempted for unlocked screen")
                        } catch (e: Exception) {
                            Log.i("TwilioVoiceDebug", "Regular app launch failed (likely locked screen): ${e.message}")
                        }
                        
                    } else {
                        Log.e("TwilioVoiceDebug", "Failed to get launch intent for app")
                    }
                } catch (notificationException: Exception) {
                    Log.e("TwilioVoiceDebug", "Failed to create full-screen notification", notificationException)
                }
            } catch (foregroundException: Exception) {
                Log.e("TwilioVoiceDebug", "Failed to bring app to foreground", foregroundException)
            }
            
            // Use Telecom framework audio routing instead of fighting against it
            Log.i("TwilioVoiceDebug", "Setting audio route through Telecom framework")
            
            // Force audio routing through the Telecom framework
            // This ensures proper microphone access for VoIP calls
            try {
                // Temporarily set to speaker to force audio system activation
                setAudioRoute(CallAudioState.ROUTE_SPEAKER)
                Log.i("TwilioVoiceDebug", "Temporarily set audio route to SPEAKER")
                
                // Wait a moment, then set to normal earpiece but maintain communication mode
                android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                    try {
                        // Ensure audio mode is still communication
                        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
                        // Set route to earpiece for normal phone call behavior
                        setAudioRoute(CallAudioState.ROUTE_WIRED_OR_EARPIECE)
                        Log.i("TwilioVoiceDebug", "Audio route set to WIRED_OR_EARPIECE with communication mode")
                    } catch (e: Exception) {
                        Log.e("TwilioVoiceDebug", "Failed to set final audio route", e)
                    }
                }, 500) // 500ms delay
                
            } catch (routeException: Exception) {
                Log.e("TwilioVoiceDebug", "Failed to set audio route through Telecom framework", routeException)
            }
            
            // Samsung-specific workaround: Additional audio mode enforcement
            val deviceManufacturer = android.os.Build.MANUFACTURER.toLowerCase()
            if (deviceManufacturer.contains("samsung")) {
                Log.i("TwilioVoiceDebug", "Samsung device detected - applying additional audio workaround")
                try {
                    // Additional delay to ensure Samsung's audio system is ready
                    android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                        try {
                            audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
                            Log.i("TwilioVoiceDebug", "Samsung additional workaround: audio mode re-enforced")
                        } catch (e: Exception) {
                            Log.e("TwilioVoiceDebug", "Samsung additional workaround failed", e)
                        }
                    }, 1500) // 1.5 second delay for Samsung
                } catch (e: Exception) {
                    Log.e("TwilioVoiceDebug", "Samsung additional workaround setup failed", e)
                }
            }
        } catch (e: Exception) {
            Log.e("TwilioVoiceDebug", "Failed to set audio mode in onConnected", e)
        }
        
        twilioCall = call
        setActive()
        onCallStateListener?.withValue(call.state)
        onEvent?.onChange(TVNativeCallEvents.EVENT_CONNECTED, Bundle().apply {
            putString(TVBroadcastReceiver.EXTRA_CALL_HANDLE, callParams?.callSid)
            putString(TVBroadcastReceiver.EXTRA_CALL_FROM, callParams?.fromRaw)
            putString(TVBroadcastReceiver.EXTRA_CALL_TO, callParams?.toRaw)
            putInt(TVBroadcastReceiver.EXTRA_CALL_DIRECTION, callDirection.id)
        })
    }

    /**
     * The call starts reconnecting.
     *
     * Reconnect is triggered when a network change is detected and Call is already in [Call.State.CONNECTED] state.
     * If the call is in [Call.State.CONNECTING] or in [Call.State.RINGING] when network
     * change happened the SDK will continue attempting to connect, but a reconnect event will not be raised.
     *
     * @param call           An object model representing a call.
     * @param callException  CallException that describes the reconnect reason. This would have one of the two
     * possible values with error codes 53001 "Signaling connection disconnected" and 53405 "Media connection failed".
     */
    override fun onReconnecting(call: Call, callException: CallException) {
        twilioCall = call
        onCallStateListener?.withValue(call.state)
        onEvent?.onChange(TVNativeCallEvents.EVENT_RECONNECTING, Bundle().apply {
            putString(TVBroadcastReceiver.EXTRA_CALL_HANDLE, callParams?.callSid)
            putString(TVBroadcastReceiver.EXTRA_CALL_FROM, callParams?.fromRaw)
            putString(TVBroadcastReceiver.EXTRA_CALL_TO, callParams?.toRaw)
            putInt(TVBroadcastReceiver.EXTRA_CALL_DIRECTION, callDirection.id)
            putExtras(callException.toBundle())
        })
    }

    /**
     * The call is reconnected.
     *
     * @param call An object model representing a call.
     */
    override fun onReconnected(call: Call) {
        twilioCall = call
        setActive()
        onCallStateListener?.withValue(call.state)
        onEvent?.onChange(TVNativeCallEvents.EVENT_RECONNECTED, Bundle().apply {
            putString(TVBroadcastReceiver.EXTRA_CALL_HANDLE, callParams?.callSid)
            putString(TVBroadcastReceiver.EXTRA_CALL_FROM, callParams?.fromRaw)
            putString(TVBroadcastReceiver.EXTRA_CALL_TO, callParams?.toRaw)
            putInt(TVBroadcastReceiver.EXTRA_CALL_DIRECTION, callDirection.id)
        });
    }

    override fun onDisconnected(call: Call, reason: CallException?) {
        // TODO run below only if we did NOT ended call i.e. remove disconnect from other client
        Log.d(TAG, "onDisconnected: onDisconnected, reason: ${reason?.message}.\nException: ${reason.toString()}")
        
        // Clean up call state and monitoring
        cleanupScreenStateMonitoring()
        
        // Reset audio mode when call ends
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager.mode = AudioManager.MODE_NORMAL
        Log.d(TAG, "Audio mode reset to MODE_NORMAL")
        
        twilioCall = null
        onCallStateListener?.withValue(call.state)
        onEvent?.onChange(TVNativeCallEvents.EVENT_DISCONNECTED_REMOTE, Bundle().apply {
            reason?.toBundle()?.let { putExtras(it) }
        })
        setDisconnected(DisconnectCause(DisconnectCause.REMOTE))
        onDisconnected?.withValue(DisconnectCause(DisconnectCause.REMOTE))
        destroy()
    }
    //endregion

    override fun onAbort() {
        super.onAbort()
        Log.i(TAG, "onAbort: onAbort")
        twilioCall?.disconnect()
        setDisconnected(DisconnectCause(DisconnectCause.CANCELED))
        onAction?.onChange(TVNativeCallActions.ACTION_ABORT, null)
        onDisconnected?.withValue(DisconnectCause(DisconnectCause.CANCELED))
        destroy()
    }

    override fun onDisconnect() {
        super.onDisconnect()
        Log.i(TAG, "onDisconnect: onDisconnect")
        
        // Clean up call state and monitoring
        cleanupScreenStateMonitoring()
        
        // Reset audio mode when call ends
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager.mode = AudioManager.MODE_NORMAL
        Log.d(TAG, "Audio mode reset to MODE_NORMAL")
        
        twilioCall?.disconnect()
        setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
        this.onDisconnected?.withValue(DisconnectCause(DisconnectCause.LOCAL))
        onEvent?.onChange(TVNativeCallEvents.EVENT_DISCONNECTED_LOCAL, null)
        destroy()
    }

    override fun onHold() {
        super.onHold()
        Log.i(TAG, "onHold: onHold")
        twilioCall?.hold(true)
        setOnHold()
        onAction?.onChange(TVNativeCallActions.ACTION_HOLD, null)

        Intent(TVBroadcastReceiver.ACTION_CALL_STATE).apply {
            putExtra(TVBroadcastReceiver.EXTRA_HOLD_STATE, true)
        }.also {
            sendBroadcast(context, it)
        }
    }

    override fun onUnhold() {
        super.onUnhold()
        Log.i(TAG, "onUnhold: onUnhold")
        twilioCall?.hold(false)
        setActive()
        onAction?.onChange(TVNativeCallActions.ACTION_UNHOLD, null)

        Intent(TVBroadcastReceiver.ACTION_CALL_STATE).apply {
            putExtra(TVBroadcastReceiver.EXTRA_HOLD_STATE, false)
        }.also {
            sendBroadcast(context, it)
        }
    }

    override fun onPlayDtmfTone(c: Char) {
        super.onPlayDtmfTone(c)
        Log.i(TAG, "onPlayDtmfTone: dtmf tone: $c")
        twilioCall?.sendDigits(c.toString())
        onAction?.onChange(TVNativeCallActions.ACTION_DTMF, Bundle().apply {
            putString(TVNativeCallActions.EXTRA_DTMF_TONE, c.toString())
        })
    }

    override fun onExtrasChanged(extras: Bundle?) {
        super.onExtrasChanged(extras)
        Log.i(TAG, "onExtrasChanged: onExtrasChanged " + extras.toString())
        extras?.let {
            val set = it.keySet()
            set.forEach {
                Log.i(TAG, "extra: $it")
            }
//            setCallerDisplayName()
        }
    }

    override fun onAnswer(videoState: Int) {
        Log.i("TwilioVoiceDebug", "TVCallConnection onAnswer called - about to setup audio mode for background calls")
        
        // NOTE: We skip the microphone permission check here because:
        // 1. We're using Android's Telecom framework (ConnectionService + PhoneAccount)
        // 2. Telecom framework handles VoIP permissions differently
        // 3. Standard permission checks fail in background even with granted permissions
        // 4. The system manages microphone access through the call framework
        Log.i("TwilioVoiceDebug", "Skipping microphone permission check for Telecom framework VoIP call")

        try {
            // Request audio focus explicitly when answering
            val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            Log.i("TwilioVoiceDebug", "AudioManager obtained successfully")
            
            // CRITICAL: Set audio mode to MODE_IN_COMMUNICATION for VoIP calls
            // This ensures microphone works properly in background calls
            audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
            Log.d(TAG, "Audio mode set to MODE_IN_COMMUNICATION")
            Log.i("TwilioVoiceDebug", "Audio mode set to MODE_IN_COMMUNICATION successfully")
            
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    Log.i("TwilioVoiceDebug", "Setting up audio focus for Android 8.0+")
                    val attributes = android.media.AudioAttributes.Builder()
                        .setUsage(android.media.AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build()
                    
                    // Create audio focus change listener
                    val audioFocusChangeListener = android.media.AudioManager.OnAudioFocusChangeListener { focusChange ->
                        Log.d(TAG, "Audio focus changed: $focusChange")
                        when (focusChange) {
                            AudioManager.AUDIOFOCUS_GAIN -> {
                                Log.d(TAG, "Audio focus gained")
                                // Ensure audio mode is still set correctly
                                try {
                                    audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
                                } catch (e: Exception) {
                                    Log.e(TAG, "Failed to reset audio mode on focus gain", e)
                                }
                            }
                            AudioManager.AUDIOFOCUS_LOSS -> {
                                Log.d(TAG, "Audio focus lost")
                            }
                            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                                Log.d(TAG, "Audio focus lost temporarily")
                            }
                            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                                Log.d(TAG, "Audio focus lost temporarily (can duck)")
                            }
                        }
                    }
                    
                    val focusRequest = android.media.AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                        .setAudioAttributes(attributes)
                        .setOnAudioFocusChangeListener(audioFocusChangeListener)
                        .build()
                    
                    val result = audioManager.requestAudioFocus(focusRequest)
                    Log.d(TAG, "Audio focus request on answer result: $result")
                    Log.i("TwilioVoiceDebug", "Audio focus request completed with result: $result")
                } else {
                    Log.i("TwilioVoiceDebug", "Setting up audio focus for older Android versions")
                    val audioFocusChangeListener = android.media.AudioManager.OnAudioFocusChangeListener { focusChange ->
                        Log.d(TAG, "Audio focus changed: $focusChange")
                    }
                    
                    val result = audioManager.requestAudioFocus(
                        audioFocusChangeListener,
                        AudioManager.STREAM_VOICE_CALL,
                        AudioManager.AUDIOFOCUS_GAIN
                    )
                    Log.d(TAG, "Audio focus request on answer result: $result")
                    Log.i("TwilioVoiceDebug", "Audio focus request completed with result: $result")
                }
            } catch (audioFocusException: Exception) {
                Log.e("TwilioVoiceDebug", "Audio focus setup failed, but continuing with call", audioFocusException)
                // Continue with call even if audio focus fails
            }
        } catch (audioException: Exception) {
            Log.e("TwilioVoiceDebug", "Audio setup failed, but continuing with call", audioException)
            // Continue with call even if audio setup fails
        }

        try {
            Log.i("TwilioVoiceDebug", "Calling super.onAnswer()")
            super.onAnswer(videoState)
            Log.i("TwilioVoiceDebug", "super.onAnswer() completed successfully")
            Log.d(TAG, "onAnswer: onAnswer")
            
            // CRITICAL: Create full-screen intent immediately when call is answered
            // This ensures app launches automatically when user taps "Accept"
            try {
                Log.i("TwilioVoiceDebug", "Creating immediate full-screen intent for call answer")
                
                val notificationManager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                val packageName = context.packageName
                val launchIntent = context.packageManager.getLaunchIntentForPackage(packageName)
                
                if (launchIntent != null) {
                    launchIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    launchIntent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    launchIntent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP)
                    launchIntent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
                    
                    val fullScreenIntent = PendingIntent.getActivity(
                        context,
                        0,
                        launchIntent,
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                        } else {
                            PendingIntent.FLAG_UPDATE_CURRENT
                        }
                    )
                    
                    // Create notification channel
                    val channelId = "incoming_call_channel"
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                        val channel = NotificationChannel(
                            channelId,
                            "Incoming Calls",
                            NotificationManager.IMPORTANCE_HIGH
                        ).apply {
                            description = "Notifications for incoming voice calls"
                            setBypassDnd(true)
                            lockscreenVisibility = Notification.VISIBILITY_PUBLIC
                        }
                        notificationManager.createNotificationChannel(channel)
                    }
                    
                    // Create immediate full-screen notification
                    val notification = NotificationCompat.Builder(context, channelId)
                        .setContentTitle("Call Answered")
                        .setContentText("Voice call active")
                        .setSmallIcon(android.R.drawable.sym_call_incoming)
                        .setPriority(NotificationCompat.PRIORITY_HIGH)
                        .setCategory(NotificationCompat.CATEGORY_CALL)
                        .setOngoing(true)
                        .setAutoCancel(false)
                        .setFullScreenIntent(fullScreenIntent, true)
                        .build()
                    
                    notificationManager.notify(1002, notification)
                    Log.i("TwilioVoiceDebug", "Immediate full-screen intent created for call answer")
                }
            } catch (immediateNotificationException: Exception) {
                Log.e("TwilioVoiceDebug", "Failed to create immediate full-screen notification", immediateNotificationException)
            }
            
        } catch (superException: Exception) {
            Log.e("TwilioVoiceDebug", "super.onAnswer() failed", superException)
            throw superException
        }
    }

    override fun onReject(rejectReason: Int) {
        Log.d(TAG, "onReject: onReject $rejectReason")
        super.onReject(rejectReason)
        twilioCall?.disconnect()
        onAction?.onChange(TVNativeCallActions.ACTION_REJECTED, null)
    }

    override fun onReject(replyMessage: String?) {
        Log.d(TAG, "onReject: onReject $replyMessage")
        super.onReject(replyMessage)
        twilioCall?.disconnect()
        onAction?.onChange(TVNativeCallActions.ACTION_REJECTED, Bundle().apply {
            putString(TVNativeCallActions.EXTRA_REJECT_REASON, replyMessage)
        })
    }

    @Suppress("DEPRECATION")
    @Deprecated("Deprecated in Java")
    override fun onCallAudioStateChanged(state: CallAudioState?) {
        Log.d(TAG, "onCallAudioStateChanged: onCallAudioStateChanged ${state.toString()}")
        super.onCallAudioStateChanged(state)

        Intent(TVBroadcastReceiver.ACTION_AUDIO_STATE).apply {
            putExtra(TVBroadcastReceiver.EXTRA_AUDIO_STATE, state)
        }.also {
            sendBroadcast(context, it)
        }
    }

    override fun onStateChanged(state: Int) {
        super.onStateChanged(state)
        Log.d(TAG, "onStateChanged: $state")
//        when (state) {
//            STATE_ACTIVE -> {
//                Log.d(TAG, "onStateChanged: STATE_ACTIVE")
//                setActive()
//            }
//
//            STATE_DIALING -> {
//                Log.d(TAG, "onStateChanged: STATE_DIALING")
//                setDialing()
//            }
//
//            STATE_DISCONNECTED -> {
//                Log.d(TAG, "onStateChanged: STATE_DISCONNECTED")
//                destroy()
//            }
//
//            STATE_HOLDING -> {
//                Log.d(TAG, "onStateChanged: STATE_HOLDING")
//                setOnHold()
//            }
//
//            STATE_NEW -> {
//                Log.d(TAG, "onStateChanged: STATE_NEW")
//                setRinging()
//            }
//
//            STATE_RINGING -> {
//                Log.d(TAG, "onStateChanged: STATE_RINGING")
//                setRinging()
//            }
//
//            else -> {
//                Log.d(TAG, "onStateChanged: STATE_UNKNOWN")
//            }
//        }
    }

    fun toggleHold(newState: Boolean) {
        if (newState) {
            onHold()
        } else {
            onUnhold()
        }
    }

    /**
     * Toggle mute state of the call.
     * @param newState: true to mute, false to unmute
     * Note: [getCallAudioState] and [onCallAudioStateChanged] has been deprecated in API 34,
     * however this will be used until [getCurrentCallEndpoint], [onCallEndpointChanged] and [onMuteStateChanged] has been implemented.
     */
    @Suppress("DEPRECATION")
    fun toggleMute(newState: Boolean) {
        //TODO(cybex-dev) implement API 34 endpoint & mute state change listeners
        twilioCall?.let {
            it.mute(newState)
            callAudioState?.let { a ->
                val newAudioRoute = a.copyWith(newState)
                onCallAudioStateChanged(newAudioRoute)
            } ?: run {
                Log.e(TAG, "toggleMute: Unable to toggle mute, callAudioState is null")
            }
        } ?: run {
            Log.e(TAG, "toggleMute: Unable to toggle mute, active call is null")
        }
    }

    /**
     * Toggle audio route of the call.
     * @param newState: true if speaker is enabled, false if speaker is disabled
     */
    fun toggleSpeaker(newState: Boolean) {
        toggleAudioRoute(CallAudioState.ROUTE_SPEAKER, newState)
    }

    /**
     * Toggle audio route of the call.
     * @param newState: true if bluetooth is enabled, false if bluetooth is disabled
     */
    fun toggleBluetooth(newState: Boolean) {
        toggleAudioRoute(CallAudioState.ROUTE_BLUETOOTH, newState)
    }

    /**
     * Toggle audio route of the call.
     * @param newAudioRoute: the new audio route to set
     * @param condition: true to use [newAudioRoute], false to use [fallback]
     * @param fallback: the fallback audio route to use if [condition] is false
     *
     * Note: [getCallAudioState] and [onCallAudioStateChanged] has been deprecated in API 34,
     * however this will be used until [getCurrentCallEndpoint], [onCallEndpointChanged] and [onMuteStateChanged] has been implemented.
     */
    @Suppress("DEPRECATION")
    private fun toggleAudioRoute(newAudioRoute: Int, condition: Boolean? = null, fallback: Int = CallAudioState.ROUTE_WIRED_OR_EARPIECE) {
        //TODO(cybex-dev) implement API 34 endpoint & mute state change listeners
        callAudioState?.let {
            val newRoute = if (condition ?: (newAudioRoute == fallback)) newAudioRoute else fallback
            setAudioRoute(newRoute)

            // Since audio route onCallAudioStateChanged does not respond to changes when call is on hold, we invoke this change manually to notify the UI.
            if (state == STATE_HOLDING) {
                onCallAudioStateChanged(callAudioState.copyWith(newRoute))
            }
        }
    }

    /**
     * Send a broadcast to the [TVBroadcastReceiver] with the given [intent].
     * @param ctx: the context
     * @param intent: the intent to send
     */
    private fun sendBroadcast(ctx: Context, intent: Intent) {
        LocalBroadcastManager.getInstance(ctx).sendBroadcast(intent)
    }

    /**
     * Disconnect the call.
     * If the call is ringing and is an incoming call, reject the call using the [CallInvite.reject].
     * Otherwise, disconnect the call using [Call.disconnect] with [DisconnectCause.LOCAL]
     */
    fun disconnect() {
        Log.d(TAG, "disconnect: disconnect")
        
        // Reset audio mode when call ends
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager.mode = AudioManager.MODE_NORMAL
        Log.d(TAG, "Audio mode reset to MODE_NORMAL")
        
        if (this is TVCallInviteConnection && state == STATE_RINGING) {
            rejectInvite()
        } else {
            Log.d(TAG, "onDisconnected: onDisconnected")
            twilioCall.let {
                it?.disconnect()
            }
            onEvent?.onChange(TVNativeCallEvents.EVENT_DISCONNECTED_LOCAL, null)
            setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
            onDisconnected?.withValue(DisconnectCause(DisconnectCause.LOCAL))
            onCallStateListener?.withValue(Call.State.DISCONNECTED)
            destroy()
        }
    }

    /**
     * Send digits to the active call.
     * @param digits: the digits to send
     */
    fun sendDigits(digits: String) {
        twilioCall?.sendDigits(digits) ?: run {
            Log.e(TAG, "sendDigits: Unable to send digits, active call is null")
        }
    }
}