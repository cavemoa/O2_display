# O2 Display

ESP32-S2 firmware for an Adafruit Feather ESP32-S2 Reverse TFT that reads environmental and oxygen-sensor data, shows key values on the built-in TFT, and serves a live web dashboard over Wi-Fi.

The actual PlatformIO project is in [`O2_display/`](./O2_display/).

## Overview

This project combines:

- An Adafruit Feather ESP32-S2 Reverse TFT as the main controller and display
- An Adafruit SCD-30 sensor for CO2, humidity, and temperature
- An Adafruit ADS1115 ADC for oxygen-cell voltage measurement
- One real AP Diving APD16 oxygen cell on ADS1115 `A0`
- Two synthetic oxygen-cell channels derived from the real cell for early development and UI testing
- A built-in Wi-Fi web server with live values, history graphs, and calibration controls

The TFT is intentionally kept simple and readable. History graphs are shown on the web page rather than on the TFT.

## Current Features

- Reads CO2, humidity, and temperature from the SCD-30 over I2C
- Reads oxygen-cell voltage from the ADS1115 over I2C
- Generates two synthetic oxygen-cell channels with small offset and lag relative to the real cell
- Shows four equal-height rows on the TFT in portrait orientation:
  - Oxygen
  - CO2
  - Temperature
  - Humidity
- Shows oxygen in raw `mV` before calibration and in `ppO2` after calibration
- Uses red oxygen text on the TFT before calibration and green after calibration
- Hosts a web dashboard with:
  - Summary cards
  - Multi-range history graphs
  - Sample-rate control
  - Oxygen calibration controls
- Supports selectable web history ranges of `5`, `15`, `30`, or `60` minutes
- Uses multi-resolution history buckets for the web graphs
- Uses New Zealand local time (`Pacific/Auckland`) for the graph time axis when NTP is available
- Falls back cleanly to elapsed uptime-based timing when internet time is not available
- Keeps USB serial logging enabled for debugging and runtime verification

## Hardware

## Main Controller

- Adafruit Feather ESP32-S2 Reverse TFT

## Sensors

- Adafruit SCD-30 NDIR CO2 / temperature / humidity sensor
- Adafruit ADS1115 16-bit ADC
- AP Diving APD16 oxygen cell connected to ADS1115 `A0`

## I2C / STEMMA QT

The current setup uses the Feather's I2C bus for both:

- SCD-30
- ADS1115

Both devices can share the same STEMMA QT / Qwiic bus.

## Oxygen Channels

The web UI and calibration logic operate on three oxygen channels:

- `Cell 1`: real APD16 sensor voltage from ADS1115 `A0`
- `Cell 2`: synthetic development channel
- `Cell 3`: synthetic development channel

The synthetic channels exist to let the application behave as if a 3-cell system is already present, even while only one real cell is connected.

## Software Stack

- PlatformIO
- Arduino framework for ESP32
- Adafruit GFX
- Adafruit ST7735 and ST7789 Library
- Adafruit SCD30
- Adafruit ADS1X15

Project environment:

- Board: `adafruit_feather_esp32s2_reversetft`
- Serial monitor: `115200`
- USB CDC on boot: enabled

## Folder Layout

```text
O2_display/
|-- README.md                 <- this file
`-- O2_display/
    |-- platformio.ini
    |-- include/
    |   `-- secrets.h
    |-- src/
    |   `-- main.cpp
    `-- .gitignore
```

Important files:

- [`O2_display/platformio.ini`](./O2_display/platformio.ini): PlatformIO environment and libraries
- [`O2_display/src/main.cpp`](./O2_display/src/main.cpp): all firmware logic, TFT UI, web UI, API, and calibration logic
- [`O2_display/include/secrets.h`](./O2_display/include/secrets.h): Wi-Fi credentials
- [`O2_display/.gitignore`](./O2_display/.gitignore): excludes local secrets and build output

## Wi-Fi Configuration

Wi-Fi credentials are stored in:

- [`O2_display/include/secrets.h`](./O2_display/include/secrets.h)

Current format:

```cpp
#pragma once

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
```

This file is already ignored by git.

## TFT Behavior

The TFT is optimized for at-a-glance local status rather than detailed analysis.

It currently displays:

- Oxygen mean value
- CO2
- Temperature
- Humidity

Display behavior:

- Portrait orientation
- Four equal-height rows
- Label at top-left of each row
- Unit at top-right of each row
- Centered measurement value
- Three significant figures for displayed values

Oxygen-specific behavior:

- Before calibration:
  - Unit is `mV`
  - Value is red
- After calibration:
  - Unit is `ppO2`
  - Value is green

## Web Dashboard

The web dashboard is served directly from the ESP32-S2.

Typical access methods:

- `http://<device-ip>/`
- `http://o2-display.local/` if mDNS resolves correctly on the local network

The page currently includes:

- Live summary cards for oxygen mean, CO2, temperature, and humidity
- A time-range dropdown for `5 / 15 / 30 / 60 min`
- Oxygen calibration controls
- A multi-trace oxygen history chart
- CO2, temperature, and humidity charts
- A time axis on the humidity chart
- Device status text including connectivity and sampling information

## Time Handling

The firmware supports two time modes:

1. Confirmed time

- When Wi-Fi is connected and NTP succeeds, graph axes use New Zealand local time
- Time zone: `Pacific/Auckland`

2. Fallback time

- If Wi-Fi has no internet access, or NTP has not yet succeeded, the firmware falls back to elapsed uptime
- Graphing and history continue working without confirmed wall-clock time

This avoids depending on internet access for basic operation while still using real local time whenever it is available.

## History and Graphing

The TFT and web page do not use the same history model.

## TFT History

- The TFT keeps a small raw sample history internally
- The TFT currently does not display graphs
- The raw history is still retained in memory for internal use and future extension

## Web History

The web dashboard uses multi-resolution history buckets so longer time ranges do not require storing all raw samples.

Current web views:

- `5 min`
- `15 min`
- `30 min`
- `60 min`

Current bucket strategy:

- `5 min` view uses `5 s` buckets
- `15 min` view uses `15 s` buckets
- `30 min` view uses `30 s` buckets
- `60 min` view uses `60 s` buckets

Each web view stores up to `60` points.

## Oxygen Calibration

The firmware currently supports two oxygen-calibration paths.

## 1. Calibrate in Air

The user can calibrate the oxygen channels in air using an assumed oxygen partial pressure of:

- `0.2099 ppO2`

Behavior:

- Before calibration, oxygen values are shown in `mV`
- The calibration computes a per-cell scale factor
- After calibration, oxygen values are displayed in `ppO2`

This applies to:

- Web summary card
- Web oxygen history chart
- TFT oxygen row

## 2. High-O2 Calibration

The web page also supports a higher-oxygen calibration workflow intended for gases such as `98% O2`.

User flow:

1. Enter the calibration gas percentage
2. Click `Begin calibration`
3. Apply the gas to the sensor system
4. The firmware watches the oxygen-cell voltage rise
5. Once the signal becomes stable, calibration is applied automatically

Stability logic:

- Uses a sliding window over the oxygen-cell voltages
- Computes a peak-to-peak measure across the window
- Waits for a meaningful rise above baseline before allowing calibration
- Requires the signal to remain stable before scales are applied

Current guard rails:

- Minimum gas percentage: `21%`
- Maximum gas percentage: `100%`
- Minimum required rise: `1.0 mV`
- Stable-window threshold: `0.2 mV` peak-to-peak
- Minimum calibration duration: `20 s`
- No-rise timeout: `120 s`
- Total calibration timeout: `300 s`
- Invalid reading handling: calibration aborts if sensor values become unusable
- Manual cancellation: supported from the web page

Important note:

- A real successful high-O2 calibration still requires physically applying the target gas and letting the sensor stabilise.

## Serial Logging

USB serial remains enabled for development and troubleshooting.

Typical serial output includes:

- Wi-Fi connection state
- Assigned IP address
- NTP time-source state
- SCD-30 readings
- Oxygen-cell voltages
- Oxygen mean value
- Calibration status messages

This is the main way to verify the live firmware behavior during development.

## HTTP API

The firmware exposes a small HTTP API for the web page and for debugging.

## Pages and Health

- `GET /` - main web dashboard
- `GET /health` - plain-text health response

## Data and Settings

- `GET /api/readings?view=<minutes>` - live values and history for a selected time range
- `POST /api/settings` - update the SCD-30 measurement interval

## Calibration

- `POST /api/calibrate-air` - apply air calibration
- `POST /api/calibrate-high-o2/start` - begin high-O2 calibration
- `POST /api/calibrate-high-o2/cancel` - cancel high-O2 calibration

## Building and Uploading

Open a terminal in:

- [`O2_display/`](./O2_display/)

Standard PlatformIO commands:

```powershell
pio run
pio run -t upload --upload-port COM6
pio device monitor -b 115200 -p COM6
```

The project environment is:

```ini
[env:adafruit_feather_esp32s2_reversetft]
```

If `pio` is not available on `PATH`, use one of these approaches:

- Open the project in Visual Studio Code with the PlatformIO extension and build/upload from there
- Run PlatformIO through your local Python or virtual-environment setup
- Add `pio` to `PATH` and use the standard commands above

## First-Time Bring-Up

1. Connect the Feather ESP32-S2 Reverse TFT by USB.
2. Connect the SCD-30 and ADS1115 to the I2C/STEMMA QT bus.
3. Connect the real oxygen cell to ADS1115 `A0`.
4. Edit [`O2_display/include/secrets.h`](./O2_display/include/secrets.h) with the correct Wi-Fi credentials.
5. Build the firmware.
6. Upload the firmware.
7. Open the serial monitor.
8. Confirm:
   - the sensors are detected
   - the device joins Wi-Fi
   - an IP address is printed
   - the web page loads
   - live readings appear on TFT and web

## Expected Runtime Behavior

On successful startup you should typically see:

- SCD-30 detection
- ADS1115 detection
- Wi-Fi connection attempt
- IP address announcement
- HTTP server startup
- Optional mDNS startup
- Optional NTP synchronization
- Periodic sensor logs over serial

## Known Design Choices

- The TFT is intentionally text-only to keep the small screen readable.
- The web page is the main analysis interface for history and calibration workflows.
- Synthetic oxygen cells are a development convenience and do not represent independent physical sensors.
- Wi-Fi is useful but not required for core sensing and TFT display.
- Confirmed wall-clock time is preferred, but uptime fallback is always available.

## Known Limitations

- Only one real oxygen cell is currently connected; the other two channels are synthetic.
- High-O2 calibration success requires real calibration gas to be applied to the sensor.
- Calibration constants are intended for runtime use; if long-term persistence is needed, that should be added explicitly.
- The nested `O2_display/O2_display` folder layout is functional but not ideal and may be worth flattening later.

## Next Likely Improvements

- Persist oxygen calibration constants across reboot
- Add support for 3 real oxygen cells on the ADS1115
- Add per-cell diagnostics on the web page
- Add clearer high-O2 calibration progress states
- Add fault detection for sensor disagreement between cells
- Flatten the repository layout if the project is going to be shared more broadly
