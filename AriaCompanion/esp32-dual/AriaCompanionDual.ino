// AriaCompanionDual.ino
//
// Same firmware flashed on both ESP32 boards. At boot the two units
// exchange their MAC over a wired UART link; whoever has the higher
// MAC becomes the Bluetooth A2DP sink (I2S master), the other becomes
// the WiFi bridge (I2S slave + TCP server on port 7001, matching
// AudioCastService.COMPANION_STREAM_PORT / the "_ariacompanion._tcp"
// mDNS service AriaCompanionActivity scans for).
//
// WiFi provisioning mirrors the Pi build: the WiFi-bridge role has no
// hardcoded credentials. If none are saved in NVS, it starts an AP
// ("AriaCompanion-XXXX") and a captive-portal-style web server; the
// AriaCast app's AriaCompanionActivity.sendWifiCredentials() already
// POSTs ssid/pass to http://192.168.4.1/wifi expecting a 200, so that
// exact route is what's implemented here. Once saved, the credentials
// are also forwarded to the peer board over the handshake UART, so
// whichever physical unit gets elected WiFi bridge after a board swap
// already has them and can skip AP setup.
//
// Board: "ESP32 Dev Module" (classic ESP32 — needs Classic BT, so this
// will NOT work on S3/C3/C6, which dropped BR/EDR).
// Libraries needed (Arduino Library Manager): "ESP32-A2DP" by pschatzmann,
// and its dependency "arduino-audio-tools" by pschatzmann (both provide
// WebServer/DNSServer/Preferences from the ESP32 core, nothing extra to install).
//
// Wiring between the two boards (same GPIO number on both sides, all on the
// same header row on a typical 30-pin ESP32 DevKit — check your board's
// silkscreen, layouts vary between DevKitC/NodeMCU-32S/DOIT clones):
//   GPIO18 <-> GPIO18   I2S BCK
//   GPIO19 <-> GPIO19   I2S WS
//   GPIO23 <-> GPIO23   I2S DATA (BT board drives it, WiFi board reads it)
//   GPIO17  -> GPIO16   UART TX -> RX (handshake)
//   GPIO16 <-  GPIO17   UART RX <- TX (handshake)
//   GND    <-> GND
// Power: USB -> 5V pin (board A) -> VIN pin (board B), GND common.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

#define BT_DEVICE_NAME "AriaCompanion"

// ---- shared pin map (identical on both boards) ----
#define PIN_I2S_BCK  18
#define PIN_I2S_WS   19
#define PIN_I2S_DATA 23
#define PIN_HS_RX    16
#define PIN_HS_TX    17
#define PIN_LED      2

#define TCP_PORT       7001   // must match AudioCastService.COMPANION_STREAM_PORT
#define SAMPLE_RATE    44100  // must match what AriaCast expects from a companion (README)
#define I2S_READ_CHUNK 3528   // 20ms @ 44.1kHz/16-bit/stereo, same granularity AriaCast reads in

#define FRAME_MAC   0xAA
#define FRAME_CREDS 0xCC

enum Role { ROLE_BT_SINK, ROLE_WIFI_BRIDGE };
Role myRole;

I2SStream i2s_out;
BluetoothA2DPSink a2dp_sink(i2s_out);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;
HardwareSerial hs(2); // handshake link: role election at boot, then ongoing WiFi-credential sync

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

void sendCredsToPeer() {
    hs.write(FRAME_CREDS);
    hs.write((uint8_t)savedSsid.length());
    hs.write((const uint8_t*)savedSsid.c_str(), savedSsid.length());
    hs.write((uint8_t)savedPass.length());
    hs.write((const uint8_t*)savedPass.c_str(), savedPass.length());
}

uint8_t hsReadBlocking() {
    while (!hs.available()) delay(1);
    return hs.read();
}

String hsReadStringBlocking(uint8_t len) {
    String s;
    for (uint8_t i = 0; i < len; i++) s += (char)hsReadBlocking();
    return s;
}

// Non-blocking except for the few bytes right after a real FRAME_CREDS
// marker, which arrive back-to-back within milliseconds of each other.
// Called continuously from loop() (both roles) so a WiFi bridge that
// learns new credentials later (via AP setup) can still push them to
// an already-running BT-sink peer.
void checkForCredentialSync() {
    if (!hs.available()) return;
    uint8_t marker = hs.read();
    if (marker != FRAME_CREDS) return; // stray byte (e.g. late MAC frame) — ignore

    uint8_t ssidLen = hsReadBlocking();
    String ssid = hsReadStringBlocking(ssidLen);
    uint8_t passLen = hsReadBlocking();
    String pass = hsReadStringBlocking(passLen);

    if (ssidLen > 0 && ssid != savedSsid) {
        Serial.println("Received WiFi credentials from peer, saving for next time I'm elected WiFi bridge.");
        saveCreds(ssid, pass);
    }
}

// Right after role election, give both boards a chance to converge on
// whichever WiFi credentials either of them already knows — important
// after swapping one unit for a blank replacement.
void syncCredsWithPeer() {
    sendCredsToPeer();
    uint32_t start = millis();
    while (millis() - start < 1000) {
        checkForCredentialSync();
    }
}

// Broadcast our MAC on the handshake UART every 300ms and listen for the
// peer's. The higher MAC becomes the BT sink. Blinks while searching so an
// unpaired unit is obvious; goes solid once a role is picked.
void printMac(const char* label, uint8_t* mac) {
    Serial.printf("%s %02X:%02X:%02X:%02X:%02X:%02X\n", label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

Role electRole() {
    uint8_t myMac[6];
    WiFi.macAddress(myMac);
    printMac("My MAC:", myMac);
    Serial.println("Waiting for peer on the handshake UART...");

    uint8_t peerMac[6];
    bool gotPeer = false;
    uint32_t lastSend = 0;
    uint8_t rxBuf[7];
    uint8_t rxLen = 0;

    while (!gotPeer) {
        digitalWrite(PIN_LED, (millis() / 250) % 2);

        if (millis() - lastSend > 300) {
            hs.write(FRAME_MAC);   // frame marker so a partial byte can't be misread as a MAC
            hs.write(myMac, 6);
            lastSend = millis();
        }

        while (hs.available()) {
            uint8_t b = hs.read();
            if (rxLen == 0 && b != FRAME_MAC) continue;
            rxBuf[rxLen++] = b;
            if (rxLen == 7) {
                memcpy(peerMac, rxBuf + 1, 6);
                gotPeer = true;
                break;
            }
        }
    }

    digitalWrite(PIN_LED, HIGH);
    printMac("Peer MAC:", peerMac);

    for (int i = 0; i < 6; i++) {
        if (myMac[i] != peerMac[i]) {
            Role r = (myMac[i] > peerMac[i]) ? ROLE_BT_SINK : ROLE_WIFI_BRIDGE;
            Serial.println(r == ROLE_BT_SINK ? "Role: BT_SINK" : "Role: WIFI_BRIDGE");
            return r;
        }
    }
    Serial.println("Role: BT_SINK (identical MAC?! unreachable in practice)");
    return ROLE_BT_SINK; // unreachable: MACs are unique per chip
}

// BT sink: decode A2DP straight to I2S. ESP32-A2DP runs its own task after
// start(), so there is nothing left to drive from loop().
void setupBtSink() {
    auto cfg = i2s_out.defaultConfig();
    cfg.pin_bck = PIN_I2S_BCK;
    cfg.pin_ws = PIN_I2S_WS;
    cfg.pin_data = PIN_I2S_DATA;
    i2s_out.begin(cfg);

    a2dp_sink.start(BT_DEVICE_NAME);
    Serial.println("A2DP sink started, pair with it from your phone's Bluetooth settings.");
}

bool tryConnectWifi(const String& ssid, const String& pass) {
    Serial.print("Connecting to saved WiFi \"" + ssid + "\"");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

// AP + captive-portal-style setup, mirroring the Pi build. The AriaCast app's
// AriaCompanionActivity.sendWifiCredentials() already POSTs ssid/pass to
// http://192.168.4.1/wifi (the ESP32 default softAP IP) expecting a 200, so
// that's the exact route implemented here — no app-side changes needed.
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
        sendCredsToPeer();
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
    }
    Serial.println("Credentials saved, rebooting to connect...");
    delay(500);
    ESP.restart();
}

// WiFi bridge: read PCM as an I2S slave (clock comes from the BT board over
// the shared wires) and forward it raw to whichever client connects.
// AriaCast is the one that connects out to us — see
// AudioCastService.startCompanionAudioCapture(), which just opens a Socket
// to this IP:port and reads a continuous raw 44.1kHz/16-bit/stereo stream.
void setupWifiBridge() {
    if (savedSsid.length() == 0 || !tryConnectWifi(savedSsid, savedPass)) {
        runApSetupPortal(); // blocks until credentials arrive, then restarts
    }
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    MDNS.begin("ariacompanion");
    MDNS.addService("ariacompanion", "tcp", TCP_PORT);
    Serial.println("mDNS service _ariacompanion._tcp announced, TCP server starting...");

    // Uses the same I2SStream (new IDF I2S driver) as the BT-sink role — mixing
    // this with the legacy driver/i2s.h API in the same binary aborts at boot
    // with "CONFLICT! The new i2s driver can't work along with the legacy i2s driver",
    // even though only one role's I2S code path actually runs per board.
    auto cfg = i2s_out.defaultConfig(RX_MODE);
    cfg.is_master = false; // clock/WS come from the BT-sink board over the shared wires
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = 16;
    cfg.channels = 2;
    cfg.pin_bck = PIN_I2S_BCK;
    cfg.pin_ws = PIN_I2S_WS;
    cfg.pin_data = PIN_I2S_DATA;
    i2s_out.begin(cfg);

    tcpServer.begin();
}

void loopWifiBridge() {
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient newClient = tcpServer.available();
        if (newClient) {
            tcpClient = newClient;
            Serial.println("AriaCast connected.");
        }
    }

    static uint8_t buf[I2S_READ_CHUNK];
    size_t bytesRead = i2s_out.readBytes(buf, I2S_READ_CHUNK);

    // Drop samples when nobody's connected instead of blocking the I2S read
    // loop — keeps the DMA buffers from backing up while AriaCast isn't casting.
    if (tcpClient && tcpClient.connected() && bytesRead > 0) {
        tcpClient.write(buf, bytesRead);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500); // give the USB-serial chip time to enumerate so early prints aren't lost
    pinMode(PIN_LED, OUTPUT);
    WiFi.mode(WIFI_STA); // needed before macAddress() is valid, even on the BT-sink board

    hs.begin(115200, SERIAL_8N1, PIN_HS_RX, PIN_HS_TX);
    loadCreds();

    myRole = electRole();
    syncCredsWithPeer(); // pick up saved WiFi creds from the peer if we don't have our own

    if (myRole == ROLE_BT_SINK) {
        setupBtSink();
    } else {
        setupWifiBridge();
    }
}

void loop() {
    checkForCredentialSync(); // keep listening even after boot, e.g. peer just ran AP setup
    if (myRole == ROLE_WIFI_BRIDGE) {
        loopWifiBridge();
    }
    // ROLE_BT_SINK: nothing else to do here, ESP32-A2DP streams from its own task.
}
