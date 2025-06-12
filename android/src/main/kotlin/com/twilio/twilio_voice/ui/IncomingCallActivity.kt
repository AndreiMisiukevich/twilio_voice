package com.twilio.twilio_voice.ui

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Bundle
import android.view.WindowManager
import android.widget.TextView
import android.view.Gravity
import android.graphics.Color
import android.widget.FrameLayout
import androidx.localbroadcastmanager.content.LocalBroadcastManager
import com.twilio.twilio_voice.receivers.TVBroadcastReceiver

class IncomingCallActivity : Activity() {
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

    override fun onDestroy() {
        LocalBroadcastManager.getInstance(this).unregisterReceiver(callEndedReceiver)
        super.onDestroy()
    }

    override fun onBackPressed() {
        // Move task to background instead of finishing so mic stays active
        moveTaskToBack(true)
    }
} 