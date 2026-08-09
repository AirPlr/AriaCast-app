package com.aria.ariacast.ha

import android.content.Context
import android.content.SharedPreferences
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL

/**
 * "HA Mode" toggle + Home Assistant Area-backed room selection for the
 * companion app, per the AriaCast <-> Home Assistant ecosystem spec.
 *
 * When enabled, the Room selector in the cast UI is populated from the
 * `ariacast_core` add-on's `/api/rooms` endpoint (which mirrors Home
 * Assistant Areas, see `custom_components/ariacast` service
 * `ariacast.sync_ha_areas`) instead of the app's own manually-entered
 * server list, and the mobile Room Canvas position updates are pushed to
 * the same shared database via `/api/listener-position` so the desktop Web
 * UI Canvas editor and the phone stay in sync in real time.
 */
class HAModeManager(context: Context) {
    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences("ariacast_ha_mode", Context.MODE_PRIVATE)

    private val _enabled = MutableStateFlow(prefs.getBoolean(KEY_ENABLED, false))
    val enabled: StateFlow<Boolean> = _enabled.asStateFlow()

    private val _addonBaseUrl = MutableStateFlow(prefs.getString(KEY_ADDON_URL, "") ?: "")
    val addonBaseUrl: StateFlow<String> = _addonBaseUrl.asStateFlow()

    fun setEnabled(value: Boolean) {
        prefs.edit().putBoolean(KEY_ENABLED, value).apply()
        _enabled.value = value
    }

    fun setAddonBaseUrl(url: String) {
        val trimmed = url.trimEnd('/')
        prefs.edit().putString(KEY_ADDON_URL, trimmed).apply()
        _addonBaseUrl.value = trimmed
    }

    companion object {
        private const val KEY_ENABLED = "ha_mode_enabled"
        private const val KEY_ADDON_URL = "ha_mode_addon_base_url"
    }
}

data class HaRoom(
    val id: String,
    val name: String,
    val haAreaId: String?,
    val widthMeters: Double,
    val heightMeters: Double,
)

/**
 * Talks to the `ariacast_core` add-on's REST API (see
 * addon/ariacast_core/app/main.py) to list HA-Area-backed rooms and push the
 * phone's tracked listener position for `RoomSpatialAudioDSP`.
 */
class HaEcosystemClient(private val baseUrl: String) {

    suspend fun fetchRooms(): List<HaRoom> = withContext(Dispatchers.IO) {
        val body = getJson("$baseUrl/api/rooms") ?: return@withContext emptyList()
        val array = JSONArray(body)
        (0 until array.length()).map { i ->
            val obj = array.getJSONObject(i)
            HaRoom(
                id = obj.getString("id"),
                name = obj.getString("name"),
                haAreaId = obj.optString("ha_area_id").ifEmpty { null },
                widthMeters = obj.optDouble("width_meters", 4.0),
                heightMeters = obj.optDouble("height_meters", 4.0),
            )
        }
    }

    suspend fun pushListenerPosition(roomId: String, x: Double, y: Double): Boolean =
        withContext(Dispatchers.IO) {
            val payload = JSONObject().apply {
                put("room_id", roomId)
                put("x", x)
                put("y", y)
            }
            postJson("$baseUrl/api/listener-position", payload)
        }

    suspend fun updateSpeakerPosition(speakerId: String, x: Double, y: Double, orientationDeg: Double): Boolean =
        withContext(Dispatchers.IO) {
            val payload = JSONObject().apply {
                put("pos_x", x)
                put("pos_y", y)
                put("orientation_deg", orientationDeg)
            }
            postJson("$baseUrl/api/speakers/$speakerId/position", payload)
        }

    private fun getJson(url: String): String? {
        val connection = URL(url).openConnection() as HttpURLConnection
        return try {
            connection.requestMethod = "GET"
            connection.connectTimeout = 4000
            connection.readTimeout = 4000
            if (connection.responseCode !in 200..299) return null
            BufferedReader(InputStreamReader(connection.inputStream)).use { it.readText() }
        } catch (e: Exception) {
            null
        } finally {
            connection.disconnect()
        }
    }

    private fun postJson(url: String, payload: JSONObject): Boolean {
        val connection = URL(url).openConnection() as HttpURLConnection
        return try {
            connection.requestMethod = "POST"
            connection.doOutput = true
            connection.connectTimeout = 4000
            connection.readTimeout = 4000
            connection.setRequestProperty("Content-Type", "application/json")
            OutputStreamWriter(connection.outputStream).use { it.write(payload.toString()) }
            connection.responseCode in 200..299
        } catch (e: Exception) {
            false
        } finally {
            connection.disconnect()
        }
    }
}
