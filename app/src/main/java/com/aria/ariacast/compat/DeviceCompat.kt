package com.aria.ariacast.compat

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
