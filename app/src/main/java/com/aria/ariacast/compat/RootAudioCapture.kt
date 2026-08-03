package com.aria.ariacast.compat

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.DataOutputStream

/**
 * On Android 8/8.1/9 (API 26-28), [android.media.AudioPlaybackCaptureConfiguration] doesn't
 * exist, and the only other API that exposes the system's mixed audio output to an app -
 * MediaRecorder.AudioSource.REMOTE_SUBMIX - requires the signature-level CAPTURE_AUDIO_OUTPUT
 * permission, which PackageManager will not grant to a normal, non-system-signed APK. On a
 * rooted device that permission can be granted manually via `pm grant`, so this is a best-effort
 * fallback for users who already have root - it is not usable on a stock, unrooted device.
 */
object RootAudioCapture {

    private const val CAPTURE_AUDIO_OUTPUT_PERMISSION = "android.permission.CAPTURE_AUDIO_OUTPUT"

    /** REMOTE_SUBMIX is only needed below API 29; from Q onward AudioPlaybackCaptureConfiguration covers it. */
    fun isRelevant(sdkInt: Int = Build.VERSION.SDK_INT): Boolean =
        sdkInt < DeviceCompat.MIN_AUDIO_CAPTURE_SDK

    fun isPermissionGranted(context: Context): Boolean =
        ContextCompat.checkSelfPermission(context, CAPTURE_AUDIO_OUTPUT_PERMISSION) ==
            PackageManager.PERMISSION_GRANTED

    /**
     * Attempts to grant CAPTURE_AUDIO_OUTPUT to this app via a root shell. Returns true only if
     * the permission is actually held afterward - false if there's no su binary, the user denies
     * the root prompt, or the grant otherwise fails.
     */
    suspend fun tryGrantViaRoot(context: Context): Boolean = withContext(Dispatchers.IO) {
        if (isPermissionGranted(context)) return@withContext true
        try {
            val process = Runtime.getRuntime().exec("su")
            DataOutputStream(process.outputStream).use { stdin ->
                stdin.writeBytes("pm grant ${context.packageName} $CAPTURE_AUDIO_OUTPUT_PERMISSION\n")
                stdin.writeBytes("exit\n")
                stdin.flush()
            }
            process.waitFor()
        } catch (e: Exception) {
            return@withContext false
        }
        isPermissionGranted(context)
    }

    /** SDK-eligible and permission is (or was just successfully) granted via root. */
    suspend fun isAvailable(context: Context, sdkInt: Int = Build.VERSION.SDK_INT): Boolean =
        isRelevant(sdkInt) && tryGrantViaRoot(context)
}
