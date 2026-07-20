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
Phone Bluetooth → ESP32 (BT sink) → I2S → ESP32 (WiFi bridge) → TCP → AriaCast → Cast destination
```

Your phone plays audio to AriaCompanion as a Bluetooth A2DP sink. One ESP32
decodes it and hands the raw PCM to a second ESP32 over I2S, which streams it
to AriaCast over your local network. AriaCast then forwards it to any cast
target (AirPlay, RAOP, AriaCast Receiver, etc.) exactly as it would with
captured system audio.

---

## Hardware

Two ESP32 boards, wired together over I2S — splitting Bluetooth and WiFi
across two chips avoids the radio contention a single ESP32 hits trying to do
both at once.

| Role | Board | Notes |
|------|-------|-------|
| **BT sink** | Any classic ESP32 (WROOM/WROVER) | Must support **Classic Bluetooth (BR/EDR)** — S3/C3/C6 do **not** work here, they dropped it in favor of BLE-only |
| **WiFi bridge** | Any ESP32 — classic, S3, C3, C6 | Only needs WiFi, no Bluetooth, so newer chips are fine (tested on a YD-ESP32-S3 N8R2 clone) |

No SD card, no separate power supply required beyond USB — each board can be
powered from its own USB port, or you can bridge one board's **5V** pin to the
other's **VIN** pin to share a single supply (never bridge 3.3V-to-3.3V — see
Troubleshooting).

---

## Firmware

Two Arduino sketches, one per board:

| Sketch | Role |
|--------|------|
| `AriaCompanionBtSink/AriaCompanionBtSink.ino` | Always the Bluetooth A2DP sink |
| `AriaCompanionWifiBridge/AriaCompanionWifiBridge.ino` | Always the WiFi/TCP bridge |

### Libraries (Arduino Library Manager)

- **BT sink board:** `ESP32-A2DP` (pschatzmann) and its dependency
  `arduino-audio-tools` (pschatzmann).
- **WiFi bridge board:** none — it uses ESP-IDF's built-in `driver/i2s.h` and
  the ESP32 core's WiFi/WebServer/DNSServer/Preferences/ESPmDNS directly.

### Wiring between the two boards

Match signal to signal — the GPIO numbers only need to match each other if
both boards are the same chip family; see the pin tables at the top of each
`.ino` file for the exact GPIOs on your specific board/variant:

| Signal | BT sink | WiFi bridge |
|--------|---------|-------------|
| I2S BCK | drives it | reads it |
| I2S WS | drives it | reads it |
| I2S DATA | drives it | reads it |
| GND | common | common |

The WiFi bridge is the I2S slave (`is_master = false`) — clock and word-select
come from the BT sink board over these wires, so get GND common right first;
a missing/loose ground reference is a common cause of garbled or missing
audio that looks like a completely different bug.

Open each sketch in the Arduino IDE, select the right board (classic
"ESP32 Dev Module" for the BT sink, your specific S3/etc. for the WiFi
bridge), flash, and power both up.

---

## First-time setup

### 1. Power on both boards

The BT sink board starts advertising Bluetooth immediately. The WiFi bridge
board checks for saved WiFi credentials; on first boot it has none, so it
starts an access point instead:

- Creates a WiFi access point named **`AriaCompanion-XXXX`** (last 4 hex
  chars of its MAC address)
- IP address: `192.168.4.1` (ESP32 default softAP IP)

### 2. Connect your phone to the AP

Join **`AriaCompanion-XXXX`** from your phone's WiFi settings. There is no
password. Android may warn the network has no internet — that's expected,
ignore it and stay connected.

### 3. Enter your home WiFi credentials

Open AriaCast → **Settings** → **AriaCompanion**, fill in your home network's
SSID and password, and tap **Configure**. The app POSTs directly to
`http://192.168.4.1/wifi` — no browser/captive portal page needed. The WiFi
bridge board saves the credentials to flash and reboots onto your home
network.

### 4. Pair Bluetooth

On your phone, open **Bluetooth settings** and scan for new devices. Select
**AriaCompanion**. No PIN is required.

### 5. Enable in AriaCast

1. Open AriaCast → **Settings** → **AriaCompanion**
2. Tap **Scan** — the device appears automatically via mDNS
   (or enter its IP manually if mDNS doesn't work on your router)
3. Enable **Use as Audio Source**

From now on, when you tap Cast in AriaCast, it will:
1. Pull audio from AriaCompanion over TCP instead of capturing system audio
2. Forward it to your cast target as usual

---

## Daily use

1. Power on both boards (or leave them always on)
2. Set your phone's **audio output to AriaCompanion** via Bluetooth
3. Play audio in any app — even ones blocked by AudioHardening
4. Tap **Cast** in AriaCast as normal

The Bluetooth volume on your phone controls the level. Keep it reasonably
high for best quality; adjust volume on the cast destination instead — very
low BT volume combined with the fixed-gain I2S link can make background
hiss more noticeable.

---

## Resetting WiFi

There's no reset button/endpoint yet — clear the WiFi bridge board's saved
credentials by re-flashing it, or add an `ariaCompanionPrefs.clear()`-style
reset trigger of your own (e.g. a boot-time button check) if you need this
often.

---

## Audio format

AriaCompanion streams raw PCM with no header:

| Parameter | Value |
|-----------|-------|
| Sample rate | 44 100 Hz |
| Channels | 2 (stereo) |
| Bit depth | 16-bit signed, little-endian |
| Encoding | None (raw PCM) |
| Port | TCP 7001 |

AriaCast upsamples from 44 100 → 48 000 Hz internally using the polyphase FIR
resampler before forwarding to the cast destination. No configuration needed.

The WiFi bridge board is the TCP **server**; AriaCast connects to it as a
**client** and reads a continuous stream — the board accepts one connection
at a time and simply forwards whatever PCM arrives over I2S.

---

## Troubleshooting

**Loud static/noise instead of audio**
- Almost always an I2S wiring issue: double-check BCK and WS aren't swapped
  between the two boards, and that GND is actually common — a missing ground
  reference produces exactly this symptom.

**Audio works but stutters intermittently**
- Make sure `WiFi.setSleep(false)` is in the WiFi bridge sketch (it is by
  default) — WiFi power-save parks the radio periodically and is a classic
  cause of streaming stutter.
- Check WiFi signal strength/distance from the router; weak signal causes
  retransmissions that a jitter buffer can only absorb so much of.
- Confirm the board isn't overheating/resetting (see below) — a reset drops
  the TCP connection and sounds like a stutter followed by silence.

**WiFi bridge board runs hot and resets**
- Make sure `loop()` has a `delay(1)` (or similar) at the end — a loop that
  never yields can spin the CPU at 100%, starve the watchdog, and reset.

**"GPIO number error" on boot (gpio_func_sel / gpio_input_enable)**
- A pin passed to the I2S config is invalid for that board/config — check the
  pin table at the top of the sketch matches your exact module (PSRAM
  type/pin count vary between ESP32-S3 variants especially).

**AriaCast can't find the device (mDNS fails)**
- Some routers block mDNS between clients; enter the IP address manually
  instead (check your router's DHCP client list, or read it off the WiFi
  bridge board's Serial Monitor output after connecting).

**Powering both boards from one supply**
- Bridge **5V-to-VIN** (one board's 5V pin to the other's VIN pin), not
  **3.3V-to-3.3V** — VIN is the unregulated input to each board's own
  regulator; feeding 3.3V into it directly can leave the far board without
  enough headroom to regulate properly and it may not power on at all.
