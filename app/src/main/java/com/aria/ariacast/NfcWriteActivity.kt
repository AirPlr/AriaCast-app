package com.aria.ariacast

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.nfc.FormatException
import android.nfc.NdefMessage
import android.nfc.NdefRecord
import android.nfc.NfcAdapter
import android.nfc.Tag
import android.nfc.tech.Ndef
import android.nfc.tech.NdefFormatable
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.ArrayAdapter
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import com.google.android.material.textfield.TextInputEditText
import java.io.IOException

/**
 * Writes an ariacast://<host>[:<port>][?type=...&name=...][&ssid=...&pass=...] link to a
 * blank or rewritable NFC tag, so a phone tapping it later (MainActivity.handleDeepLink)
 * can auto-start casting - optionally after joining a Wi-Fi network first (WifiJoinManager).
 * This screen only builds and writes the link; it never joins Wi-Fi or starts casting itself.
 */
class NfcWriteActivity : AppCompatActivity() {

    private lateinit var hostInput: TextInputEditText
    private lateinit var portInput: TextInputEditText
    private lateinit var typeInput: MaterialAutoCompleteTextView
    private lateinit var nameInput: TextInputEditText
    private lateinit var wifiSwitch: MaterialSwitch
    private lateinit var wifiFieldsContainer: View
    private lateinit var ssidInput: TextInputEditText
    private lateinit var passInput: TextInputEditText
    private lateinit var statusText: android.widget.TextView

    private var nfcAdapter: NfcAdapter? = null

    private data class ProtocolOption(val labelRes: Int, val typeKey: String?)

    private val protocolOptions = listOf(
        ProtocolOption(R.string.ariacast_protocol, null),
        ProtocolOption(R.string.airplay_protocol, "airplay"),
        ProtocolOption(R.string.airplay2_protocol, "airplay2"),
        ProtocolOption(R.string.dlna_protocol, "dlna"),
        ProtocolOption(R.string.google_cast_protocol, "googlecast")
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        val sharedPreferences = getSharedPreferences(AudioCastService.PREFS_NAME, Context.MODE_PRIVATE)
        val accentColor = sharedPreferences.getInt(SettingsActivity.KEY_ACCENT_COLOR, R.color.accent_blue)
        setTheme(ThemeUtils.getThemeForAccent(accentColor))

        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_nfc_write)

        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        setSupportActionBar(toolbar)
        toolbar.setNavigationOnClickListener { finish() }

        hostInput = findViewById(R.id.hostInput)
        portInput = findViewById(R.id.portInput)
        typeInput = findViewById(R.id.typeInput)
        nameInput = findViewById(R.id.nameInput)
        wifiSwitch = findViewById(R.id.wifiSwitch)
        wifiFieldsContainer = findViewById(R.id.wifiFieldsContainer)
        ssidInput = findViewById(R.id.ssidInput)
        passInput = findViewById(R.id.passInput)
        statusText = findViewById(R.id.statusText)

        typeInput.setAdapter(
            ArrayAdapter(this, android.R.layout.simple_list_item_1, protocolOptions.map { getString(it.labelRes) })
        )
        typeInput.setText(getString(protocolOptions[0].labelRes), false)

        wifiSwitch.setOnCheckedChangeListener { _, checked ->
            wifiFieldsContainer.visibility = if (checked) View.VISIBLE else View.GONE
        }

        nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        if (nfcAdapter == null) {
            statusText.text = getString(R.string.nfc_not_available)
        }
    }

    override fun onResume() {
        super.onResume()
        nfcAdapter?.let { adapter ->
            val intent = Intent(this, javaClass).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
            val pendingIntent = PendingIntent.getActivity(
                this, 0, intent, PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            adapter.enableForegroundDispatch(this, pendingIntent, null, null)
        }
    }

    override fun onPause() {
        super.onPause()
        nfcAdapter?.disableForegroundDispatch(this)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val tag: Tag? = if (Build.VERSION.SDK_INT >= 33) {
            intent.getParcelableExtra(NfcAdapter.EXTRA_TAG, Tag::class.java)
        } else {
            @Suppress("DEPRECATION") intent.getParcelableExtra(NfcAdapter.EXTRA_TAG)
        }
        if (tag != null) writeTag(tag)
    }

    /** Builds the ariacast:// link from the form, or null if it isn't fillable yet
     *  (missing host). Uses Uri.Builder so every field is safely percent-encoded, however
     *  it's typed - the same values will round-trip through Uri.getQueryParameter() on the
     *  reading side in MainActivity.handleDeepLink(). */
    private fun buildDeepLinkUri(): Uri? {
        val host = hostInput.text?.toString()?.trim().orEmpty()
        if (host.isEmpty()) return null

        val port = portInput.text?.toString()?.trim().orEmpty()
        val name = nameInput.text?.toString()?.trim().orEmpty()
        val typeKey = protocolOptions.firstOrNull { getString(it.labelRes) == typeInput.text?.toString() }?.typeKey

        val builder = Uri.Builder()
            .scheme("ariacast")
            .authority(if (port.isNotEmpty()) "$host:$port" else host)
        if (typeKey != null) builder.appendQueryParameter("type", typeKey)
        if (name.isNotEmpty()) builder.appendQueryParameter("name", name)

        if (wifiSwitch.isChecked) {
            val ssid = ssidInput.text?.toString()?.trim().orEmpty()
            if (ssid.isNotEmpty()) {
                builder.appendQueryParameter("ssid", ssid)
                val pass = passInput.text?.toString().orEmpty()
                if (pass.isNotEmpty()) builder.appendQueryParameter("pass", pass)
            }
        }

        return builder.build()
    }

    private fun writeTag(tag: Tag) {
        val typeKey = protocolOptions.firstOrNull { getString(it.labelRes) == typeInput.text?.toString() }?.typeKey
        if (typeKey == "dlna" && wifiSwitch.isChecked) {
            statusText.text = getString(R.string.nfc_write_dlna_wifi_conflict)
            return
        }

        val uri = buildDeepLinkUri()
        if (uri == null) {
            statusText.text = getString(R.string.nfc_write_missing_host)
            return
        }

        statusText.text = getString(R.string.nfc_write_status_writing)
        val message = NdefMessage(arrayOf(NdefRecord.createUri(uri)))

        try {
            val ndef = Ndef.get(tag)
            if (ndef != null) {
                ndef.connect()
                try {
                    if (!ndef.isWritable) {
                        statusText.text = getString(R.string.nfc_write_read_only)
                        return
                    }
                    if (ndef.maxSize < message.toByteArray().size) {
                        statusText.text = getString(R.string.nfc_write_too_small)
                        return
                    }
                    ndef.writeNdefMessage(message)
                    statusText.text = getString(R.string.nfc_write_status_success, uri.host)
                } finally {
                    try { ndef.close() } catch (e: IOException) {}
                }
                return
            }

            val formatable = NdefFormatable.get(tag)
            if (formatable != null) {
                formatable.connect()
                try {
                    formatable.format(message)
                    statusText.text = getString(R.string.nfc_write_status_success, uri.host)
                } finally {
                    try { formatable.close() } catch (e: IOException) {}
                }
                return
            }

            statusText.text = getString(R.string.nfc_write_unsupported_tag)
        } catch (e: IOException) {
            statusText.text = getString(R.string.nfc_write_failed)
        } catch (e: FormatException) {
            statusText.text = getString(R.string.nfc_write_failed)
        }
    }
}
