// AriaCompanionBtSink.ino
//
// Fixed-role half of the two-ESP32 AriaCompanion bridge: this board is
// ALWAYS the Bluetooth A2DP sink. Pair your phone with it over Bluetooth
// (BT_DEVICE_NAME below), it decodes A2DP audio and outputs raw PCM over
// I2S to the other board, which runs AriaCompanionWifiBridge.ino and
// forwards it to AriaCast over WiFi.
//
// Board: "ESP32 Dev Module" (classic ESP32 — needs Classic BT, so this
// will NOT work on S3/C3/C6, which dropped BR/EDR).
// Libraries needed (Arduino Library Manager): "ESP32-A2DP" by pschatzmann,
// and its dependency "arduino-audio-tools" by pschatzmann.
//
// Wiring to the WiFi-bridge board (same GPIO number on both sides, all on
// the same header row on a typical 30-pin ESP32 DevKit — check your
// board's silkscreen, layouts vary between DevKitC/NodeMCU-32S/DOIT clones):
//   GPIO18 <-> GPIO18   I2S BCK
//   GPIO19 <-> GPIO19   I2S WS
//   GPIO23 <-> GPIO23   I2S DATA (this board drives it, the WiFi board reads it)
//   GND    <-> GND
// Power each board separately (USB each, or bridge 5V-to-VIN — not 3.3V-to-3.3V).

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

#define BT_DEVICE_NAME "AriaCompanion"

#define PIN_I2S_BCK  18
#define PIN_I2S_WS   19
#define PIN_I2S_DATA 23
#define PIN_LED      2

I2SStream i2s_out;
BluetoothA2DPSink a2dp_sink(i2s_out);

void setup() {
    Serial.begin(115200);
    delay(500); // give the USB-serial chip time to enumerate so early prints aren't lost
    pinMode(PIN_LED, OUTPUT);

    auto cfg = i2s_out.defaultConfig();
    cfg.pin_bck = PIN_I2S_BCK;
    cfg.pin_ws = PIN_I2S_WS;
    cfg.pin_data = PIN_I2S_DATA;
    i2s_out.begin(cfg);

    a2dp_sink.start(BT_DEVICE_NAME);
    Serial.println("A2DP sink started as \"" BT_DEVICE_NAME "\" — pair with it from your phone's Bluetooth settings.");
}

void loop() {
    // Heartbeat blink (1s) just to show the sketch is alive — all the real
    // work happens on ESP32-A2DP's own task in the background.
    digitalWrite(PIN_LED, (millis() / 1000) % 2);
}
