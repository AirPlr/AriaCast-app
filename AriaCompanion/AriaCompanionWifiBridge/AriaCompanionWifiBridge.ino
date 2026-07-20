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
// Requires PSRAM enabled in the Arduino IDE board menu (Tools > PSRAM >
// "QSPI PSRAM" for a quad-PSRAM board like the N8R2) — the ~1s jitter
// buffer below is allocated there.
// No extra libraries needed — uses ESP-IDF's built-in driver/i2s.h and the
// ESP32 core's WiFi/WebServer/DNSServer/Preferences/ESPmDNS directly. This
// board runs standalone (never shares a binary with the BT-sink board's
// Bluetooth stack), so there's no reason to pull in arduino-audio-tools
// just for a raw I2S read loop — one less dependency, one less thing that
// can change API between versions.
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
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include <esp_heap_caps.h>

#define PIN_I2S_BCK  4
#define PIN_I2S_WS   5
#define PIN_I2S_DATA 6
#define PIN_RGB_LED  48 // onboard WS2812 on this board, used as the status LED
#define STATUS_LED_BRIGHTNESS 38 // ~15% of 255 — full brightness is blinding at night

Adafruit_NeoPixel rgbLed(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

void setStatusLed(bool on) {
    uint8_t v = on ? STATUS_LED_BRIGHTNESS : 0;
    rgbLed.setPixelColor(0, rgbLed.Color(v, v, v));
    rgbLed.show();
}

#define TCP_PORT       7001   // must match AudioCastService.COMPANION_STREAM_PORT
#define SAMPLE_RATE    44100  // must match what AriaCast expects from a companion (README)
#define I2S_READ_CHUNK 3528   // 20ms @ 44.1kHz/16-bit/stereo, same granularity AriaCast reads in
#define I2S_DMA_BUF_COUNT 16
#define I2S_DMA_BUF_LEN   256  // frames (stereo samples) per buffer; 256*4 bytes = 1024 bytes/buffer

// A ~1s jitter buffer between the I2S read and the TCP write, like a "real"
// network receiver would use — decouples the two completely, so a WiFi
// stall of up to ~1s gets absorbed instead of glitching audio. Lives in
// PSRAM (requires PSRAM enabled in the Arduino IDE board menu) since ~172KB
// is a lot to ask of internal SRAM alongside the WiFi stack.
#define JITTER_BUFFER_BYTES (SAMPLE_RATE * 2 /*ch*/ * 2 /*bytes/sample*/) // ~1 second
uint8_t* jitterBuf = nullptr;
size_t jitterHead = 0;   // next write position
size_t jitterTail = 0;   // next read position
size_t jitterFill = 0;   // bytes currently buffered
bool jitterPrimed = false; // true once we've filled a full second at least once

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
    // Modem sleep (WiFi power-save) is on by default and periodically parks
    // the radio to save power, introducing latency spikes of tens to
    // hundreds of ms — a classic cause of intermittent stutter in real-time
    // streaming over ESP32 WiFi. We're USB-powered, so there's no need for it.
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300);
        Serial.print(".");
        setStatusLed((millis() / 150) % 2); // fast blink while connecting
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
        setStatusLed((millis() / 500) % 2); // slow blink while waiting for setup
    }
    Serial.println("Credentials saved, rebooting to connect...");
    delay(500);
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    delay(500); // give the USB-serial chip time to enumerate so early prints aren't lost

    rgbLed.begin();
    setStatusLed(false);

    jitterBuf = (uint8_t*)heap_caps_malloc(JITTER_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    if (jitterBuf == nullptr) {
        Serial.println("PSRAM allocation failed for jitter buffer — is PSRAM enabled in Tools > PSRAM?");
    }

    loadCreds();
    if (savedSsid.length() == 0 || !tryConnectWifi(savedSsid, savedPass)) {
        runApSetupPortal(); // blocks until credentials arrive, then restarts
    }

    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    setStatusLed(true); // solid: connected and about to start serving

    MDNS.begin("ariacompanion");
    MDNS.addService("ariacompanion", "tcp", TCP_PORT);
    Serial.println("mDNS service _ariacompanion._tcp announced, TCP server starting...");

    // Slave RX: clock/WS come from the BT-sink board over the shared wires,
    // we just read whatever arrives. DMA buffer sized generously (16 x 1024
    // bytes = 16KB) so a brief WiFi stall (tcpClient.write() blocking for a
    // few ms under congestion) doesn't overflow it and glitch the audio.
    i2s_config_t i2sCfg = {
        .mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_DMA_BUF_COUNT,
        .dma_buf_len = I2S_DMA_BUF_LEN
    };
    i2s_driver_install(I2S_NUM_0, &i2sCfg, 0, NULL);

    i2s_pin_config_t pinCfg = {
        .bck_io_num = PIN_I2S_BCK,
        .ws_io_num = PIN_I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_I2S_DATA
    };
    i2s_set_pin(I2S_NUM_0, &pinCfg);

    tcpServer.begin();
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
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, buf, I2S_READ_CHUNK, &bytesRead, portMAX_DELAY);

    if (jitterBuf == nullptr) {
        // PSRAM wasn't available — fall back to direct passthrough rather than crash.
        if (tcpClient && tcpClient.connected() && bytesRead > 0) {
            tcpClient.write(buf, bytesRead);
        }
    } else {
        // Push into the ring buffer, overwriting the oldest bytes if it's
        // completely full (shouldn't happen once primed, since drain rate
        // tracks fill rate — this is just a safety net).
        for (size_t i = 0; i < bytesRead; i++) {
            jitterBuf[jitterHead] = buf[i];
            jitterHead = (jitterHead + 1) % JITTER_BUFFER_BYTES;
            if (jitterFill < JITTER_BUFFER_BYTES) {
                jitterFill++;
            } else {
                jitterTail = (jitterTail + 1) % JITTER_BUFFER_BYTES;
            }
        }

        if (!jitterPrimed && jitterFill >= JITTER_BUFFER_BYTES) {
            jitterPrimed = true;
            Serial.println("Jitter buffer primed (~1s), starting playback drain.");
        }

        if (jitterPrimed && tcpClient && tcpClient.connected() && jitterFill > 0) {
            static uint8_t sendBuf[I2S_READ_CHUNK];
            size_t toSend = min(jitterFill, (size_t)I2S_READ_CHUNK);
            for (size_t i = 0; i < toSend; i++) {
                sendBuf[i] = jitterBuf[jitterTail];
                jitterTail = (jitterTail + 1) % JITTER_BUFFER_BYTES;
            }
            jitterFill -= toSend;
            tcpClient.write(sendBuf, toSend);
        }
    }

    // Without this, a loop() that never blocks for real can spin at 100% CPU
    // and starve the idle task, tripping the watchdog (reset) and running hot.
    delay(1);
}
