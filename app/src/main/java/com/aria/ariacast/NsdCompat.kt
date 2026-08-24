package com.aria.ariacast

import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import java.util.concurrent.Executor

/**
 * Both single-shot [NsdManager.resolveService] overloads are deprecated as of API 35 in
 * favor of [NsdManager.registerServiceInfoCallback], which delivers live updates instead
 * of resolving once. This adapts that live callback back into the one-shot
 * [NsdManager.ResolveListener] contract this app's callers expect: it unregisters itself
 * as soon as the first update (or a terminal failure) arrives. Below API 34, where
 * [NsdManager.registerServiceInfoCallback] does not exist, this app's minSdk of 31 still
 * needs the deprecated single-arg overload.
 */
fun NsdManager.resolveServiceCompat(serviceInfo: NsdServiceInfo, executor: Executor, listener: NsdManager.ResolveListener) {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
        var settled = false
        val callback = object : NsdManager.ServiceInfoCallback {
            override fun onServiceInfoCallbackRegistrationFailed(errorCode: Int) {
                if (settled) return
                settled = true
                listener.onResolveFailed(serviceInfo, errorCode)
            }
            override fun onServiceUpdated(updatedInfo: NsdServiceInfo) {
                if (settled) return
                settled = true
                this@resolveServiceCompat.unregisterServiceInfoCallback(this)
                listener.onServiceResolved(updatedInfo)
            }
            override fun onServiceLost() {
                if (settled) return
                settled = true
                this@resolveServiceCompat.unregisterServiceInfoCallback(this)
                listener.onResolveFailed(serviceInfo, NsdManager.FAILURE_INTERNAL_ERROR)
            }
            override fun onServiceInfoCallbackUnregistered() {}
        }
        registerServiceInfoCallback(serviceInfo, executor, callback)
    } else {
        @Suppress("DEPRECATION")
        resolveService(serviceInfo, listener)
    }
}

/**
 * [NsdServiceInfo.host] was replaced by the multi-address [NsdServiceInfo.hostAddresses] in
 * API 34; below that (down to this app's minSdk 31) [host] is the only option.
 */
val NsdServiceInfo.hostAddressCompat: String?
    get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
        hostAddresses.firstOrNull()?.hostAddress
    } else {
        @Suppress("DEPRECATION")
        host?.hostAddress
    }
