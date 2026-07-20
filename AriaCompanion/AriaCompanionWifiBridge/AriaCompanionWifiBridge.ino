// AriaCompanionWifiBridge.ino
//
// Fixed-role half of the two-ESP32 AriaCompanion bridge: this board is
// ALWAYS the WiFi bridge. It reads raw PCM over I2S (as a slave — clock
// comes from the other board, which runs AriaCompanionBtSink.ino) and
// forwards it to AriaCast over a plain TCP server on port 7001, matching
// AudioCastService.COMPANION_STREAM_PORT and the "_ariacompanion._tcp"
// mDNS service AriaCompanionActivity scans for.
//
// WiFi provisioning mirrors the Pi build: no hardcoded credentials. If
// none are saved in NVS, this starts an AP ("AriaCompanion-XXXX") and
// serves POST /wifi on 192.168.4.1 (the ESP32 default softAP IP) — the
// exact route AriaCompanionActivity.sendWifiCredentials() already POSTs
// to, so no app-side changes are needed. Once saved, credentials persist
// across reboots; if you ever replace this board, you'll need to redo AP
// setup once on the replacement.
//
// Board: any ESP32 (this role needs WiFi only, no Classic BT, so S3/C3/C6
// work fine here too). Pins below are tuned for a YD-ESP32-S3 (ESP32-S3-
// DevKitC-1 clone, N8R2 — quad PSRAM, so GPIO33-37 are free too); adjust if
// you're on a different module/variant.
// Library needed (Arduino Library Manager): "arduino-audio-tools" by
// pschatzmann (for I2SStream — same driver family the BT-sink board's
// ESP32-A2DP library uses internally, so the two never fight over the I2S
// peripheral even though they're now separate binaries).
//
// Wiring to the BT-sink board — GPIO numbers differ per side since the two
// boards are different chips (classic ESP32 vs S3), just match signal to
// signal, not GPIO number to GPIO number:
//   This board (S3)      BT-sink board (classic ESP32)
//   GPIO4  (I2S BCK)  <-> GPIO18
//   GPIO5  (I2S WS)   <-> GPIO19
//   GPIO6  (I2S DATA) <-> GPIO23   (BT-sink board drives it, this board reads it)
//   GND               <-> GND
// Power each board separately (USB each, or bridge 5V-to-VIN — not 3.3V-to-3.3V).

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <math.h>
#include "AudioTools.h"

#define PIN_I2S_BCK  4
#define PIN_I2S_WS   5
#define PIN_I2S_DATA 6
#define PIN_LED      7

#define TCP_PORT       7001   // must match AudioCastService.COMPANION_STREAM_PORT
#define SAMPLE_RATE    44100  // must match what AriaCast expects from a companion (README)
#define I2S_READ_CHUNK 3528   // 20ms @ 44.1kHz/16-bit/stereo, same granularity AriaCast reads in

I2SStream i2s_in;
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;

Preferences prefs;
String savedSsid, savedPass;

WebServer apServer(80);
DNSServer dnsServer;
volatile bool wifiConfigReceived = false;

void loadCreds() {
    prefs.begin("ariacomp", true);
    savedSsid = prefs.getString("ssid", "");
    savedPass = prefs.getString("pass", "");
    prefs.end();
}

void saveCreds(const String& ssid, const String& pass) {
    prefs.begin("ariacomp", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    savedSsid = ssid;
    savedPass = pass;
}

bool tryConnectWifi(const String& ssid, const String& pass) {
    Serial.print("Connecting to saved WiFi \"" + ssid + "\"");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300);
        Serial.print(".");
        digitalWrite(PIN_LED, (millis() / 150) % 2); // fast blink while connecting
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

// AP + captive-portal-style setup, mirroring the Pi build. The AriaCast app's
// AriaCompanionActivity.sendWifiCredentials() already POSTs ssid/pass to
// http://192.168.4.1/wifi expecting a 200, so that's the exact route
// implemented here — no app-side changes needed.
void runApSetupPortal() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "AriaCompanion-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apName);
    Serial.print("No saved WiFi credentials. AP setup mode: connect to \"");
    Serial.print(apName);
    Serial.println("\", then use AriaCast's AriaCompanion screen to configure.");

    dnsServer.start(53, "*", WiFi.softAPIP());

    apServer.on("/wifi", HTTP_POST, []() {
        String ssid = apServer.arg("ssid");
        String pass = apServer.arg("pass");
        if (ssid.length() == 0) {
            apServer.send(400, "text/plain", "missing ssid");
            return;
        }
        saveCreds(ssid, pass);
        apServer.send(200, "text/plain", "OK");
        wifiConfigReceived = true;
    });
    apServer.onNotFound([]() {
        apServer.sendHeader("Location", "http://192.168.4.1/", true);
        apServer.send(302, "text/plain", "");
    });
    apServer.begin();

    while (!wifiConfigReceived) {
        dnsServer.processNextRequest();
        apServer.handleClient();
        digitalWrite(PIN_LED, (millis() / 500) % 2); // slow blink while waiting for setup
    }
    Serial.println("Credentials saved, rebooting to connect...");
    delay(500);
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    delay(500); // give the USB-serial chip time to enumerate so early prints aren't lost
    pinMode(PIN_LED, OUTPUT);

    loadCreds();
    if (savedSsid.length() == 0 || !tryConnectWifi(savedSsid, savedPass)) {
        runApSetupPortal(); // blocks until credentials arrive, then restarts
    }

    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(PIN_LED, HIGH); // solid: connected and about to start serving

    MDNS.begin("ariacompanion");
    MDNS.addService("ariacompanion", "tcp", TCP_PORT);
    Serial.println("mDNS service _ariacompanion._tcp announced, TCP server starting...");

    auto cfg = i2s_in.defaultConfig(RX_MODE);
    cfg.is_master = false; // clock/WS come from the BT-sink board over the shared wires
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = 16;
    cfg.channels = 2;
    cfg.pin_bck = PIN_I2S_BCK;
    cfg.pin_ws = PIN_I2S_WS;
    cfg.pin_data = PIN_I2S_DATA;    // TX field, harmless to also set in RX mode
    cfg.pin_data_rx = PIN_I2S_DATA; // some AudioTools versions read the RX data pin from here instead
    // Bigger than the library default so a brief WiFi stall (tcpClient.write()
    // blocking for a few ms under congestion) doesn't overflow the DMA buffer
    // and glitch the audio — trades a bit of latency for tolerance to jitter.
    cfg.buffer_count = 16;
    cfg.buffer_size = 1024;
    i2s_in.begin(cfg);

    tcpServer.begin();
}

// RMS level of the 16-bit stereo samples in buf, in dBFS (0 = full scale,
// more negative = quieter, -100 as a floor for near-silence).
float levelDbfs(uint8_t* buf, size_t byteLen) {
    int16_t* samples = (int16_t*)buf;
    size_t n = byteLen / 2;
    if (n == 0) return -100.0;
    double sumSquares = 0;
    for (size_t i = 0; i < n; i++) sumSquares += (double)samples[i] * samples[i];
    double rms = sqrt(sumSquares / n);
    if (rms < 1) return -100.0;
    return 20.0 * log10(rms / 32768.0);
}

void loop() {
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient newClient = tcpServer.available();
        if (newClient) {
            // Without this, Nagle's algorithm can coalesce/delay our small
            // 20ms writes waiting for ACKs, building up latency that keeps
            // growing instead of settling — exactly a creeping/never-catches-up lag.
            newClient.setNoDelay(true);
            tcpClient = newClient;
            Serial.println("AriaCast connected.");
        }
    }

    static uint8_t buf[I2S_READ_CHUNK];
    size_t bytesRead = i2s_in.readBytes(buf, I2S_READ_CHUNK);

    // Drop samples when nobody's connected instead of blocking the I2S read
    // loop — keeps the DMA buffers from backing up while AriaCast isn't casting.
    if (tcpClient && tcpClient.connected() && bytesRead > 0) {
        tcpClient.write(buf, bytesRead);
    }

    static uint32_t lastLog = 0;
    if (millis() - lastLog > 1000) {
        lastLog = millis();
        Serial.printf("I2S read: %u/%u bytes, level: %.1f dBFS, WiFi RSSI: %d dBm%s\n",
            (unsigned)bytesRead, (unsigned)I2S_READ_CHUNK, levelDbfs(buf, bytesRead), WiFi.RSSI(),
            (tcpClient && tcpClient.connected()) ? " [client connected]" : " [no client]");
    }

    // Without this, a loop() that never blocks for real can spin at 100% CPU
    // and starve the idle task, tripping the watchdog (reset) and running hot.
    delay(1);
}
