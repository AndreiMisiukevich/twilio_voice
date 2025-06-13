package com.twilio.twilio_voice.ui

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.WindowManager
import android.widget.TextView
import android.view.Gravity
import android.graphics.Color
import android.widget.FrameLayout
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.twilio.twilio_voice.receivers.TVBroadcastReceiver
import com.twilio.twilio_voice.service.TVConnectionService
import com.twilio.twilio_voice.service.TVCallInviteConnection

class IncomingCallActivity : Activity() {
    companion object {
        private const val TAG = "IncomingCallActivity"
        private const val ACCEPT_DELAY_MS = 10000L // Delay to let activity fully resume before opening mic
    }

    private val callEndedReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            finish()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(
            WindowManager.LayoutParams.FLAG_FULLSCREEN or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
        )
        
        // Create simple UI programmatically
        val container = FrameLayout(this).apply {
            setBackgroundColor(Color.parseColor("#BF000000")) // Semi-transparent black
        }
        
        val textView = TextView(this).apply {
            text = "CALL IN PROGRESS"
            textSize = 28f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }
        
        val layoutParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply {
            gravity = Gravity.CENTER
        }
        
        container.addView(textView, layoutParams)
        setContentView(container)
        
        LocalBroadcastManager.getInstance(this).registerReceiver(
            callEndedReceiver,
            IntentFilter(TVBroadcastReceiver.ACTION_CALL_ENDED)
        )
    }

    override fun onResume() {
        super.onResume()
        // Accept the incoming call after a delay
        acceptIncomingCall()
    }

    private fun acceptIncomingCall() {
        Handler(Looper.getMainLooper()).postDelayed({
            try {
                val incomingCallHandle = TVConnectionService.getIncomingCallHandle()
                if (incomingCallHandle != null) {
                    val connection = TVConnectionService.getConnection(incomingCallHandle)
                    if (connection is TVCallInviteConnection) {
                        // Check if call is still ringing (not already accepted)
                        if (connection.state == android.telecom.Connection.STATE_RINGING) {
                            Log.d(TAG, "Accepting incoming call with handle: $incomingCallHandle")
                            connection.acceptInvite()
                        } else {
                            Log.d(TAG, "Call is already accepted or in different state: ${connection.state}")
                        }
                    } else {
                        Log.e(TAG, "Connection is not a TVCallInviteConnection or not found")
                    }
                } else {
                    Log.e(TAG, "No incoming call handle found")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Error accepting incoming call: ${e.message}")
            }
        }, ACCEPT_DELAY_MS)
    }

    override fun onDestroy() {
        LocalBroadcastManager.getInstance(this).unregisterReceiver(callEndedReceiver)
        super.onDestroy()
    }

    override fun onBackPressed() {
        // Move task to background instead of finishing so mic stays active
        moveTaskToBack(true)
    }
} 