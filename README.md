# ESP32-S3 16×64 LED Matrix Clock

A feature-rich desk clock built on an ESP32-S3 driving a 16×64 MAX7219 LED
dot-matrix display (two stacked 8×64 rows). Shows time, date, weather,
scrolling news, overhead aircraft (via a self-hosted aviation API), and a
full-screen "Only Clock" mode with a choice of fonts — all configurable
from a built-in web UI, no app or cloud account required.

![status](https://img.shields.io/badge/status-actively--developed-brightgreen)

<img width="500" alt="image" src="https://github.com/user-attachments/assets/2bab6793-f848-4712-9940-251a136d3e8e" />
<img width="500" alt="image" src="https://github.com/user-attachments/assets/325d4393-0535-4efc-b0aa-4c111801c188" />
<img width="500" alt="image" src="https://github.com/user-attachments/assets/1b7b620c-604d-411a-b0a6-83f22048128e" />
<img width="500" alt="image" src="https://github.com/user-attachments/assets/855e46d8-9728-4720-bcf5-91eaafdce4df" />

<img width="500" alt="image" src="https://github.com/user-attachments/assets/563e853d-0a72-4c37-b747-73292fec533e" />


## Features

- **Clock & date** — 12/24-hour, configurable timezone, blinking colon
- **Weather** — animated icons (sun, cloud, rain, snow, fog, storm) via
  [Open-Meteo](https://open-meteo.com), auto day/night brightness using
  real sunrise/sunset times for your location
- **News ticker** — 16 curated RSS sources across Global/Europe/Asia/India,
  sequential or shuffled order, each headline scrolls exactly once
- **Aircraft overhead** — shows carrier, aircraft type, and route when a
  plane passes overhead, via a self-hosted [aviation data
  API](#aircraft-overhead-optional) (optional feature)
- **"Only Clock" master mode** — a big, full-display clock with a choice
  of fonts (see below), everything else hidden
- **Per-task brightness** — independent brightness for clock/news/aircraft,
  plus automatic day/night dimming
- **Web configuration UI** — 7 organized cards (Network, Clock, Weather,
  Feeds, Aircraft, Appearance, System), live status, OTA firmware updates,
  diagnostic log viewer
- **Reliability** — hardware watchdog with automatic recovery, OTA-safe
  updates, backoff on failing network calls so one flaky service can't
  take down the whole device

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3-DevKitC-1 (N16R8) | 16MB flash / 8MB PSRAM variant; other ESP32-S3 boards should work with pin adjustments |
| 16×64 MAX7219 LED matrix | Single PCB, two rows of eight 8×8 modules, one continuous 16-device chain |
| 5V power supply, **3A or higher** | A 2A supply is not enough — it causes brownouts and random reboots under load. This was a real, time-consuming bug in early development; don't skip this. |

### Wiring

| Signal | GPIO |
|---|---|
| DIN | 4 |
| CS | 5 |
| CLK | 8 |

Adjust `DIN_PIN` / `CS_PIN` / `CLK_PIN` near the top of the sketch if your
wiring differs.

## Getting started

1. Open the `.ino` in Arduino IDE with the ESP32 board package installed
   (tested on both ESP32 Arduino core 2.x and 3.x).
2. Install libraries: `MD_MAX72XX` (MajicDesigns).
3. Flash over USB.
4. On first boot, the device can't find WiFi credentials and starts an
   access point called **"Wifi Clock"**. Connect to it with your phone,
   open `192.168.4.1`, and enter your WiFi details in the **Network**
   card.
5. Once connected, find the device's IP on your router (or watch the
   serial monitor) and open it in a browser to reach the full
   configuration UI.

## Configuration

Everything is set from the web UI — no need to edit and reflash the
sketch for day-to-day settings:

- **Network** — WiFi credentials, connection status
- **Clock** — 12/24h, timezone, clock position, Only Clock master mode
  and font choice
- **Weather** — latitude/longitude (used for weather + sunrise/sunset)
- **Feeds** — news source selection, order, scroll speed/spacing
- **Aircraft** — enable/disable, API host, display timings (see below)
- **Appearance** — per-task brightness, day/night auto-dimming
- **System** — firmware info, OTA update, diagnostic logs, test pattern,
  factory reset

Settings persist in flash (NVS) across reboots and firmware updates.

### Aircraft overhead (optional)

This feature polls a **self-hosted** aviation data API (not included in
this repo) for aircraft currently overhead, at endpoints:
- `GET /led-matrix-state` — current aircraft, if any
- `GET /routes/<callsign>` — route lookup (origin/destination)

If you don't have such a service, just leave this feature disabled in
the **Aircraft** card — everything else works independently.

### Font choices for "Only Clock" mode

Three font options, switchable live from the web UI:
- **Original** — the project's first hand-drawn attempt
- **Classic** — a proven font adapted from a real working LED clock
  build (see [Credits](#credits))
- **Bold** — an alternate style, also sourced from a verified public
  font definition

## Project history

This project went through many iterations — including a failed attempt
at moving network fetches to a second CPU core (the ESP32 Arduino
WebServer and WiFiClientSecure libraries aren't safe to use from
different cores simultaneously, which corrupted the web UI), a socket
exhaustion bug that caused periodic reboots, and several font redesigns
based on real hardware photos. See the version history comments at the
top of the `.ino` file for the full changelog — it's a genuinely useful
record of what was tried and why some approaches didn't work.

## Credits

- LED matrix driving via [MD_MAX72XX](https://github.com/MajicDesigns/MD_MAX72XX)
  by Marco Colli (MajicDesigns)
- The "Classic" big-clock font is adapted from a font by Giovanni
  Bernardo ([@cyb3rn0id](https://github.com/cyb3rn0id)), originally
  published for a similar MD_Parola-based LED clock build
- Weather data from [Open-Meteo](https://open-meteo.com) (free, no API
  key required)

## License

No license has been chosen yet for this repository. Until one is added,
standard copyright applies — please ask before reusing substantial
portions. *(Consider adding an MIT or similar permissive license if you
want others to freely build on this.)*

## Security note

The default WiFi SSID/password and aircraft API host in the sketch are
placeholders (`YOUR_WIFI_SSID` / `192.168.1.100` etc.) — set your real
values through the web UI after first boot, not by editing the source.
Your entered values are stored in the device's flash, not in this
repository.
