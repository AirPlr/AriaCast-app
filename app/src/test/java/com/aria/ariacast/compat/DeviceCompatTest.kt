package com.aria.ariacast.compat

import android.os.Build
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DeviceCompatTest {

    @Test
    fun `audio playback capture is unsupported below Android 10`() {
        assertFalse(DeviceCompat.supportsAudioPlaybackCapture(sdkInt = 26)) // Android 8.0
        assertFalse(DeviceCompat.supportsAudioPlaybackCapture(sdkInt = 27)) // Android 8.1
        assertFalse(DeviceCompat.supportsAudioPlaybackCapture(sdkInt = 28)) // Android 9
    }

    @Test
    fun `audio playback capture is supported from Android 10 onward`() {
        assertTrue(DeviceCompat.supportsAudioPlaybackCapture(sdkInt = 29)) // Android 10
        assertTrue(DeviceCompat.supportsAudioPlaybackCapture(sdkInt = 34))
        assertTrue(DeviceCompat.supportsAudioPlaybackCapture(sdkInt = 35))
    }

    @Test
    fun `MIN_AUDIO_CAPTURE_SDK matches the boundary this app relies on`() {
        assertTrue(DeviceCompat.MIN_AUDIO_CAPTURE_SDK == Build.VERSION_CODES.Q)
    }
}
