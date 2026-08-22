# AriaCompanion

A hardware bridge that lets AriaCast cast audio from apps blocked by Samsung's
**AudioHardening** system (Metrolist, YouTube Music ReVanced, and others).

## Why this exists

Samsung injects a flag (`FLAG_NO_MEDIA_PROJECTION`) into the audio tracks of
unofficial or sideloaded apps. This flag silently blocks Android's
`MediaProjection` API — the one AriaCast uses to capture system audio.
Official Play Store apps are exempt. There is no software workaround at the
AriaCast level.

AriaCompanion solves this by moving audio capture off the phone entirely:

```
Phone Bluetooth → ESP32 (BT sink) → I2S → ESP32 (WiFi sender) → WebSocket → AriaCast → Cast destination
```

Your phone plays audio to AriaCompanion as a Bluetooth A2DP sink. One ESP32
decodes it and hands the raw PCM to a second ESP32 over I2S. That second
board then pushes the audio to AriaCast using AriaCast's own native protocol
(the same WebSocket protocol AriaCast uses to cast to a dedicated AriaCast
Receiver) — AriaCompanion connects **to** the phone, not the other way
around. AriaCast then forwards the audio to any cast target (AirPlay, RAOP,
DLNA, etc.) exactly as it would with captured system audio.

---

## Hardware

Two ESP32 boards, wired together over I2S — splitting Bluetooth and WiFi
across two chips avoids the radio contention a single ESP32 hits trying to do
both at once.

| Role | Folder | Board | Notes |
|------|--------|-------|-------|
| **BT sink** | `AriaCompanionBtSink/` | Any classic ESP32 (WROOM/WROVER) | Must support **Classic Bluetooth (BR/EDR)** — S3/C3/C6 do **not** work here, they dropped it in favor of BLE-only |
| **WiFi sender** | `AriaCompanionWifiBridge/` | Any ESP32 — classic, S3, C3, C6 | Only needs WiFi, no Bluetooth, so newer chips are fine (tested on an ESP32-S3-DevKitC-1) |

No SD card, no separate power supply required beyond USB — each board can be
powered from its own USB port, or you can bridge one board's **5V** pin to the
other's **VIN** pin to share a single supply (never bridge 3.3V-to-3.3V — see
Troubleshooting).

---

## Firmware

Two independent [PlatformIO](https://platformio.org/) projects, one per board:

| Folder | Role |
|--------|------|
| `AriaCompanionBtSink/` | Always the Bluetooth A2DP sink ("Scheda A"). Resamples whatever the source negotiates (typically 44100 or 48000 Hz) to a fixed 48 kHz stereo 16-bit and drives the I2S bus as **master**. Never touches WiFi. |
| `AriaCompanionWifiBridge/` | Always the WiFi sender ("Scheda B"). Reads I2S as **slave** at a fixed 48 kHz (no resampling needed — the BT sink already guarantees the rate), packetizes it into 20 ms / 3840-byte frames, and streams them to AriaCast as a **Sender** over WebSocket. Exposes a small REST API (port 8081) the app uses to tell it which AriaCast instance to stream to. Never touches Bluetooth. |

Build each independently:

```bash
cd AriaCompanionBtSink && pio run -t upload -t monitor
cd AriaCompanionWifiBridge && pio run -t upload -t monitor
```

### Libraries (fetched automatically by PlatformIO via `platformio.ini`)

- **BT sink board:** `ESP32-A2DP` (pschatzmann) and its dependency
  `arduino-audio-tools` (pschatzmann).
- **WiFi sender board:** `WebSockets` (links2004) and `ArduinoJson`
  (bblanchon). WiFi/mDNS/Preferences come from the ESP32 Arduino core.

### Wiring between the two boards

Match signal to signal — the GPIO numbers only need to match each other if
both boards are the same chip family; see the pin constants at the top of
each source file for the exact GPIOs on your specific board/variant.

| Signal | BT sink (`main.cpp`) | WiFi sender (`i2s_input.h`) |
| ------ | --------------------- | ---------------------------- |
| I2S BCK  | GPIO 26 | GPIO 4 |
| I2S WS   | GPIO 25 | GPIO 5 |
| I2S DATA | GPIO 22 | GPIO 6 (RX input) |
| GND      | common  | common (**mandatory** reference) |

The BT sink board is the I2S **master** (`is_master = true`) — clock and
word-select are generated there; the WiFi sender is the **slave**
(`I2S_ROLE_SLAVE`) and just reads whatever arrives. A missing/loose ground
reference is a common cause of garbled or missing audio that looks like a
completely different bug.

If you change pins, update both `AriaCompanionBtSink/src/main.cpp`
(`PIN_I2S_*`) and `AriaCompanionWifiBridge/src/i2s_input.h` (`I2S_IN_PIN_*`).

---

## First-time setup

### 1. Power on the WiFi sender board first

On first boot it has no saved WiFi credentials, so it opens an access point:

- Creates a WiFi access point named **`AriaCast-Setup`** (no password)
- IP address: `192.168.4.1` (ESP32 default softAP IP)

### 2. Connect your phone to the AP

Join **`AriaCast-Setup`** from your phone's WiFi settings. There is no
password. Android may warn the network has no internet — that's expected,
ignore it and stay connected.

### 3. Enter your home WiFi credentials

Open AriaCast → **Settings** → **AriaCompanion**, fill in your home network's
SSID and password, and tap **Configure**. The app POSTs directly to
`http://192.168.4.1/save` — no browser/captive portal page needed. The WiFi
sender board saves the credentials to flash and reboots onto your home
network, then announces itself via mDNS as `_ariacompanion._tcp` on its REST
API port (8081).

### 4. Power on the BT sink board and pair Bluetooth

The BT sink board starts advertising Bluetooth immediately as **`AriaCast-BT`**
(rename it via `BT_DEVICE_NAME` in `AriaCompanionBtSink/src/main.cpp`). On
your phone, open **Bluetooth settings**, scan for new devices, and select
**AriaCast-BT**. No PIN is required.

### 5. Enable in AriaCast

1. Open AriaCast → **Settings** → **AriaCompanion**
2. Tap **Scan** — the WiFi sender board appears automatically via mDNS
   (or enter its IP manually if mDNS doesn't work on your router)
3. Enable **Use as Audio Source**

From now on, when you tap Cast in AriaCast, it will:
1. Start AriaCast's own local receiver and tell the WiFi sender board (via
   its REST API) to connect there
2. Accept the incoming AriaCast stream from AriaCompanion instead of
   capturing system audio
3. Forward it to your selected cast target as usual

---

## Daily use

1. Power on both boards (or leave them always on)
2. Set your phone's **audio output to AriaCast-BT** via Bluetooth
3. Play audio in any app — even ones blocked by AudioHardening
4. Tap **Cast** in AriaCast as normal

The Bluetooth volume on your phone controls the level. Keep it reasonably
high for best quality; adjust volume on the cast destination instead — very
low BT volume combined with the fixed-gain I2S link can make background
hiss more noticeable.

---

## REST API (WiFi sender board, port 8081)

The WiFi sender board does no discovery of its own — that stays the app's
job (it already speaks AriaCast's own discovery protocols). This API just
receives the host/port the app has chosen for its local receiver.

```
GET http://<board-ip>:8081/api/status
```
```json
{
  "wifi": { "connected": true, "ip": "192.168.1.42", "ssid": "HomeNetwork" },
  "receiver": { "host": "192.168.1.100", "port": 12889, "state": "streaming" },
  "stats": { "framesSent": 15234, "i2sTimeouts": 3 }
}
```

```
POST http://<board-ip>:8081/api/receiver
Content-Type: application/json

{"host": "192.168.1.100", "port": 12889}
```
Saves the receiver to NVS (persists across reboots) and immediately starts a
WebSocket connection to `/audio` on that host/port. AriaCast calls this
automatically with its own IP when you enable AriaCompanion as an audio
source — you never need to call it by hand.

```
DELETE http://<board-ip>:8081/api/receiver
```
Stops streaming and forgets the selected receiver.

```
POST http://<board-ip>:8081/api/wifi/reset
```
Clears the saved WiFi credentials and reboots into the `AriaCast-Setup`
configuration AP (useful if you change routers).

`state` in `/api/status` can be: `idle`, `connecting`, `waiting_handshake`,
`streaming`, `reconnecting`.

---

## Audio format

AriaCompanion streams over AriaCast's native protocol (WebSocket, `/audio`,
JSON `{"status":"READY"}` handshake), not a bespoke raw-TCP format:

| Parameter | Value |
|-----------|-------|
| Sample rate | 48 000 Hz |
| Channels | 2 (stereo) |
| Bit depth | 16-bit signed, little-endian |
| Frame size | 3840 bytes (20 ms), one frame every 20 ms |
| Transport | WebSocket, AriaCompanion connects out to AriaCast |

Because the BT sink board already resamples everything to 48 kHz before it
ever reaches I2S, AriaCast needs no resampling step for AriaCompanion audio —
it's the same rate/format AriaCast uses internally everywhere else.

---

## Troubleshooting

**Loud static/noise instead of audio**
- Almost always an I2S wiring issue: double-check BCK and WS aren't swapped
  between the two boards, and that GND is actually common — a missing ground
  reference produces exactly this symptom.

**Audio works but stutters intermittently**
- Make sure `WiFi.setSleep(false)` is in effect on the WiFi sender board (it
  is by default) — WiFi power-save parks the radio periodically and is a
  classic cause of streaming stutter.
- Check WiFi signal strength/distance from the router; weak signal causes
  retransmissions that a jitter buffer can only absorb so much of.
- Confirm the board isn't overheating/resetting (see below) — a reset drops
  the WebSocket connection and sounds like a stutter followed by silence.

**WiFi sender board runs hot and resets**
- Make sure `loop()` has a `delay()` at the end on both boards — a loop that
  never yields can spin the CPU at 100%, starve the watchdog, and reset.

**"GPIO number error" on boot (gpio_func_sel / gpio_input_enable)**
- A pin passed to the I2S config is invalid for that board/config — check the
  pin constants match your exact module (PSRAM type/pin count vary between
  ESP32-S3 variants especially).

**AriaCast can't find the device (mDNS fails)**
- Some routers block mDNS between clients; enter the IP address manually
  instead (check your router's DHCP client list, or read it off the WiFi
  sender board's Serial Monitor output after connecting).

**Powering both boards from one supply**
- Bridge **5V-to-VIN** (one board's 5V pin to the other's VIN pin), not
  **3.3V-to-3.3V** — VIN is the unregulated input to each board's own
  regulator; feeding 3.3V into it directly can leave the far board without
  enough headroom to regulate properly and it may not power on at all.
