package com.aria.ariacast

import android.app.Activity
import android.content.Intent
import android.net.wifi.WifiNetworkSuggestion
import android.provider.Settings
import android.util.Log
import java.net.InetSocketAddress
import java.net.Socket
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * A deep link (ariacast://...?ssid=...&pass=...) can ask the phone to join a specific
 * Wi-Fi network before casting - e.g. an NFC tag that should both switch networks and
 * start casting to a receiver that only lives on that network.
 *
 * This goes through Android's own Settings.ACTION_WIFI_ADD_NETWORKS flow instead of
 * WifiNetworkSpecifier/ConnectivityManager.requestNetwork: the system shows its native
 * "save this Wi-Fi network?" dialog, and once accepted the network is saved and managed
 * exactly as if the user had added it by hand in Settings - the app gets no ownership of
 * it and the platform decides when to associate. That also means there's nothing for
 * AudioCastService to release when casting stops; the network simply stays saved, like
 * any other Wi-Fi network on the phone.
 */
object WifiJoinManager {
    private const val TAG = "WifiJoinManager"

    /** Builds the Settings.ACTION_WIFI_ADD_NETWORKS intent for [ssid]/[password] (open
     *  network if [password] is blank). Launch it with an ActivityResultLauncher and pass
     *  the result to [interpretResult]. */
    fun buildAddNetworkIntent(ssid: String, password: String): Intent {
        val builder = WifiNetworkSuggestion.Builder().setSsid(ssid)
        if (password.isNotEmpty()) builder.setWpa2Passphrase(password)
        return Intent(Settings.ACTION_WIFI_ADD_NETWORKS).apply {
            putParcelableArrayListExtra(Settings.EXTRA_WIFI_NETWORK_LIST, arrayListOf(builder.build()))
        }
    }

    /** True if the network was saved (or already existed) - i.e. the user didn't dismiss
     *  the system dialog and the platform accepted the credentials. */
    fun interpretResult(resultCode: Int, data: Intent?): Boolean {
        if (resultCode != Activity.RESULT_OK) return false
        val codes = data?.getIntegerArrayListExtra(Settings.EXTRA_WIFI_NETWORK_RESULT_LIST) ?: return false
        return codes.any { it == Settings.ADD_WIFI_RESULT_SUCCESS || it == Settings.ADD_WIFI_RESULT_ALREADY_EXISTS }
    }

    /**
     * Saving a network doesn't mean the phone is connected to it yet - the platform
     * associates with it in the background, on its own schedule. Rather than trying to
     * read back the active SSID (which needs location permission once the app no longer
     * owns the suggestion - see class doc), this just probes the actual target directly:
     * that's what the caller needs to know anyway, and it comes back true the moment the
     * receiver is really reachable, whichever network that ends up being over.
     */
    suspend fun waitForReachable(host: String, port: Int, timeoutMs: Long = 15000): Boolean = withContext(Dispatchers.IO) {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            try {
                Socket().use { it.connect(InetSocketAddress(host, port), 1500) }
                return@withContext true
            } catch (e: Exception) {
                Log.d(TAG, "$host:$port not reachable yet: ${e.message}")
            }
            delay(500)
        }
        false
    }
}
