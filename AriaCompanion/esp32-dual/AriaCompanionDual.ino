// AriaCompanionDual.ino
//
// Same firmware flashed on both ESP32 boards. At boot the two units
// exchange their MAC over a wired UART link; whoever has the higher
// MAC becomes the Bluetooth A2DP sink (I2S master), the other becomes
// the WiFi bridge (I2S slave + TCP server on port 7001, matching
// AudioCastService.COMPANION_STREAM_PORT / the "_ariacompanion._tcp"
// mDNS service AriaCompanionActivity scans for).
//
// Board: "ESP32 Dev Module" (classic ESP32 — needs Classic BT, so this
// will NOT work on S3/C3/C6, which dropped BR/EDR).
// Library needed: "ESP32-A2DP" by pschatzmann (Arduino Library Manager).
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
#include <ESPmDNS.h>
#include <driver/i2s.h>
#include "BluetoothA2DPSink.h"

// ---- fill in before flashing ----
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
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

enum Role { ROLE_BT_SINK, ROLE_WIFI_BRIDGE };
Role myRole;

BluetoothA2DPSink a2dp_sink;
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;

// Broadcast our MAC on the handshake UART every 300ms and listen for the
// peer's. The higher MAC becomes the BT sink. Blinks while searching so an
// unpaired unit is obvious; goes solid once a role is picked.
Role electRole() {
    HardwareSerial hs(2);
    hs.begin(115200, SERIAL_8N1, PIN_HS_RX, PIN_HS_TX);

    uint8_t myMac[6];
    WiFi.macAddress(myMac);

    uint8_t peerMac[6];
    bool gotPeer = false;
    uint32_t lastSend = 0;
    uint8_t rxBuf[7];
    uint8_t rxLen = 0;

    while (!gotPeer) {
        digitalWrite(PIN_LED, (millis() / 250) % 2);

        if (millis() - lastSend > 300) {
            hs.write(0xAA);        // frame marker so a partial byte can't be misread as a MAC
            hs.write(myMac, 6);
            lastSend = millis();
        }

        while (hs.available()) {
            uint8_t b = hs.read();
            if (rxLen == 0 && b != 0xAA) continue;
            rxBuf[rxLen++] = b;
            if (rxLen == 7) {
                memcpy(peerMac, rxBuf + 1, 6);
                gotPeer = true;
                break;
            }
        }
    }

    digitalWrite(PIN_LED, HIGH);

    for (int i = 0; i < 6; i++) {
        if (myMac[i] != peerMac[i]) {
            return (myMac[i] > peerMac[i]) ? ROLE_BT_SINK : ROLE_WIFI_BRIDGE;
        }
    }
    return ROLE_BT_SINK; // unreachable: MACs are unique per chip
}

// BT sink: decode A2DP straight to I2S. ESP32-A2DP runs its own task after
// start(), so there is nothing left to drive from loop().
void setupBtSink() {
    i2s_pin_config_t pins = {
        .bck_io_num = PIN_I2S_BCK,
        .ws_io_num = PIN_I2S_WS,
        .data_out_num = PIN_I2S_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    a2dp_sink.set_pin_config(pins);
    a2dp_sink.start(BT_DEVICE_NAME);
}

// WiFi bridge: read PCM as an I2S slave (clock comes from the BT board over
// the shared wires) and forward it raw to whichever client connects.
// AriaCast is the one that connects out to us — see
// AudioCastService.startCompanionAudioCapture(), which just opens a Socket
// to this IP:port and reads a continuous raw 44.1kHz/16-bit/stereo stream.
void setupWifiBridge() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(200);

    MDNS.begin("ariacompanion");
    MDNS.addService("ariacompanion", "tcp", TCP_PORT);

    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

    i2s_pin_config_t pins = {
        .bck_io_num = PIN_I2S_BCK,
        .ws_io_num = PIN_I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_I2S_DATA
    };
    i2s_set_pin(I2S_NUM_0, &pins);

    tcpServer.begin();
}

void loopWifiBridge() {
    if (!tcpClient || !tcpClient.connected()) {
        tcpClient = tcpServer.available();
    }

    static uint8_t buf[I2S_READ_CHUNK];
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, buf, I2S_READ_CHUNK, &bytesRead, portMAX_DELAY);

    // Drop samples when nobody's connected instead of blocking the I2S read
    // loop — keeps the DMA buffers from backing up while AriaCast isn't casting.
    if (tcpClient && tcpClient.connected() && bytesRead > 0) {
        tcpClient.write(buf, bytesRead);
    }
}

void setup() {
    pinMode(PIN_LED, OUTPUT);
    WiFi.mode(WIFI_STA); // needed before macAddress() is valid, even on the BT-sink board

    myRole = electRole();

    if (myRole == ROLE_BT_SINK) {
        setupBtSink();
    } else {
        setupWifiBridge();
    }
}

void loop() {
    if (myRole == ROLE_WIFI_BRIDGE) {
        loopWifiBridge();
    }
    // ROLE_BT_SINK: nothing to do here, ESP32-A2DP streams from its own task.
}
