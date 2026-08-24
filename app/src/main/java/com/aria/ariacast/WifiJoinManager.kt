package com.aria.ariacast

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.util.Log

/**
 * A deep link (ariacast://...?ssid=...&pass=...) can ask the phone to join a specific
 * Wi-Fi network before casting - e.g. an NFC tag that should both switch networks and
 * start casting to a receiver that only lives on that network. The request/callback this
 * needs (ConnectivityManager.requestNetwork) has to stay registered for the whole cast
 * session, not just until the requesting Activity is destroyed, so it's held here at the
 * process level - MainActivity starts the join, AudioCastService releases it in
 * cleanupSession() once casting actually stops.
 *
 * NET_CAPABILITY_INTERNET is deliberately not required: the target network (a home LAN,
 * an IoT VLAN) may not have - or may not have validated - internet access, only local
 * reachability to the receiver, which is all this app needs from it.
 */
object WifiJoinManager {
    private const val TAG = "WifiJoinManager"
    private const val JOIN_TIMEOUT_MS = 20000

    private var connectivityManager: ConnectivityManager? = null
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var processWasBound = false

    /** Requests [ssid] (with [password], or open if blank), binds the app's traffic to it
     *  once available, and reports success/failure via [onResult] on the main thread. */
    fun join(context: Context, ssid: String, password: String, onResult: (Boolean) -> Unit) {
        release(context)

        val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        connectivityManager = cm

        val specifierBuilder = WifiNetworkSpecifier.Builder().setSsid(ssid)
        if (password.isNotEmpty()) specifierBuilder.setWpa2Passphrase(password)

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(specifierBuilder.build())
            .build()

        var settled = false
        val nc = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                if (settled) return
                settled = true
                processWasBound = cm.bindProcessToNetwork(network)
                Log.d(TAG, "Joined Wi-Fi \"$ssid\" (process bound=$processWasBound)")
                onResult(true)
            }

            override fun onUnavailable() {
                if (settled) return
                settled = true
                Log.w(TAG, "Could not join Wi-Fi \"$ssid\" - denied, wrong password, or out of range")
                release(context)
                onResult(false)
            }
        }
        callback = nc

        try {
            cm.requestNetwork(request, nc, JOIN_TIMEOUT_MS)
        } catch (e: Exception) {
            Log.e(TAG, "requestNetwork failed: ${e.message}")
            connectivityManager = null
            callback = null
            onResult(false)
        }
    }

    /** Releases a previously joined/pending network, if any, restoring the phone's normal
     *  default network. Safe to call even when nothing was ever joined. */
    fun release(context: Context) {
        val cm = connectivityManager ?: return
        callback?.let {
            try { cm.unregisterNetworkCallback(it) } catch (e: Exception) {}
        }
        if (processWasBound) {
            try { cm.bindProcessToNetwork(null) } catch (e: Exception) {}
        }
        connectivityManager = null
        callback = null
        processWasBound = false
    }
}
