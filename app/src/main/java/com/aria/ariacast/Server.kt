package com.aria.ariacast

data class Server(
    val name: String,
    val host: String,
    val port: Int,
    val version: String,
    val codecs: List<String>,
    val sampleRate: Int,
    val channels: Int,
    val platform: String? = null,
    val extra: String? = null // For DLNA control URL or other data
)

/**
 * [Server.extra] and [com.aria.ariacast.CastDestination.extra] pack several key=value
 * fields into one ';'-joined string. Some of those values (mDNS TXT record contents like
 * "model") come straight from an untrusted device on the network, so a naive split(";")/
 * substringAfter("=") parse lets a crafted value like "EvilName;pk=attacker" inject a
 * decoy field ahead of the real one. Escaping '\', ';' and '=' in each value before
 * joining - and unescaping only on unescaped delimiters when parsing - closes that.
 */
object ExtraFields {
    fun encode(key: String, value: String): String = "$key=${escape(value)}"

    fun join(fields: List<Pair<String, String>>): String = fields.joinToString(";") { (k, v) -> encode(k, v) }

    fun get(extra: String?, key: String): String? = parse(extra)[key]

    fun parse(extra: String?): Map<String, String> {
        if (extra.isNullOrEmpty()) return emptyMap()
        val result = LinkedHashMap<String, String>()
        for (part in splitUnescaped(extra, ';')) {
            if (part.isEmpty()) continue
            val eqIdx = indexOfUnescaped(part, '=')
            if (eqIdx < 0) continue
            result[unescape(part.substring(0, eqIdx))] = unescape(part.substring(eqIdx + 1))
        }
        return result
    }

    private fun escape(value: String): String =
        value.replace("\\", "\\\\").replace(";", "\\;").replace("=", "\\=")

    private fun unescape(value: String): String {
        val sb = StringBuilder()
        var i = 0
        while (i < value.length) {
            if (value[i] == '\\' && i + 1 < value.length) {
                sb.append(value[i + 1]); i += 2
            } else {
                sb.append(value[i]); i++
            }
        }
        return sb.toString()
    }

    private fun indexOfUnescaped(s: String, target: Char): Int {
        var i = 0
        while (i < s.length) {
            if (s[i] == '\\' && i + 1 < s.length) { i += 2; continue }
            if (s[i] == target) return i
            i++
        }
        return -1
    }

    private fun splitUnescaped(s: String, delimiter: Char): List<String> {
        val parts = mutableListOf<String>()
        val current = StringBuilder()
        var i = 0
        while (i < s.length) {
            if (s[i] == '\\' && i + 1 < s.length) {
                current.append(s[i]).append(s[i + 1]); i += 2; continue
            }
            if (s[i] == delimiter) {
                parts.add(current.toString()); current.clear(); i++; continue
            }
            current.append(s[i]); i++
        }
        parts.add(current.toString())
        return parts
    }
}
