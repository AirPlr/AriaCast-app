package com.aria.ariacast.compat

import android.app.Notification
import android.app.Service
import android.os.Build

/**
 * Central place for OS-version gates. minSdk (26) is lower than the API level required by
 * [android.media.AudioPlaybackCaptureConfiguration] (29), which the direct on-device audio
 * capture path depends on, so callers must check this before starting that flow.
 */
object DeviceCompat {

    const val MIN_AUDIO_CAPTURE_SDK = Build.VERSION_CODES.Q

    fun supportsAudioPlaybackCapture(sdkInt: Int = Build.VERSION.SDK_INT): Boolean =
        sdkInt >= MIN_AUDIO_CAPTURE_SDK
}

/**
 * [Service.startForeground] with a foregroundServiceType only exists from API 29 (Q) onward;
 * calling it on Android 8/8.1/9 throws NoSuchMethodError. Falls back to the 2-arg overload below Q.
 */
fun Service.startForegroundCompat(id: Int, notification: Notification, foregroundServiceType: Int) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        startForeground(id, notification, foregroundServiceType)
    } else {
        startForeground(id, notification)
    }
}
