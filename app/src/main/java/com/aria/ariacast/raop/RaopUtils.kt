package com.aria.ariacast.raop

import java.net.Inet4Address
import java.net.NetworkInterface

object RaopUtils {
    fun getLocalIpAddress(): java.net.InetAddress? {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val iface = interfaces.nextElement()
                if (iface.isLoopback || !iface.isUp) continue

                val name = iface.name.lowercase()
                if (name.contains("tun") || name.contains("ppp") || name.contains("tap")) continue

                val addresses = iface.inetAddresses
                while (addresses.hasMoreElements()) {
                    val addr = addresses.nextElement()
                    if (addr is Inet4Address) {
                        return addr
                    }
                }
            }
        } catch (e: Exception) {}
        return null
    }
}
