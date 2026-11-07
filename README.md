# AZ3166 Weather Clock

A WiFi weather clock for the [MXChip AZ3166](https://docs.mxchip.com/en/nr6ggk/blyezpv6gkqicywi.html) IoT DevKit, running [Eclipse ThreadX](https://github.com/eclipse-threadx) and NetX Duo.

- **Clock screen:** local time, day and date, current temperature, today's high/low/rain chance and wind. A pair of icons shows the current conditions and what's coming, e.g. cloud → snow (today's forecast, or tomorrow's from 6 PM). Clear skies show a moon at night.
- **Button A:** steps through the daily forecast (today, tomorrow, and the next few days), then back to the clock. Each day shows conditions, high/low, chance of rain and max wind. Dots in the corner show which day you're on. With no press for 15 seconds, it returns to the clock.
- **Button B:** refresh the weather now (it otherwise updates every 15 minutes).
- **Screen saver:** after 10 minutes without a button press or movement, the screen dims and shows a small clock that slowly drifts around, to prevent OLED burn-in. During quiet hours the screen turns fully off. A button press, a tap on the board, or picking it up or bumping it wakes it.
- **RGB LED alerts:** the LED slowly breathes blue when rain is likely and icy white when it will be near freezing, alternating if both. From 6 PM it warns about tomorrow instead of today, and it stays off during quiet hours (10 PM to 7 AM by default).

Time is synced over SNTP. Weather comes from [Open-Meteo](https://open-meteo.com), which is free and needs no API key. The local time zone, including daylight saving, comes from the weather service based on your location.

## Getting the code

ThreadX and NetX Duo are git submodules, so clone with:

```bash
git clone --recurse-submodules https://github.com/elle-johnnie/az3166-weather-clock.git
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

## Tools

You need CMake, Ninja, and the Arm GNU toolchain (`arm-none-eabi-gcc`).

**macOS**

```bash
brew install cmake ninja
```

Then install the Arm GNU toolchain from [developer.arm.com](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (for Apple silicon: `arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi.pkg`) and make sure its `bin` folder is on your `PATH`, or set `ARM_GCC_PATH` to it.

**Ubuntu**

```bash
sudo apt install cmake ninja-build
```

Then download the Arm GNU toolchain for Linux from the link above and add its `bin` folder to your `PATH`.

**Windows**

```powershell
winget install --id=Arm.GnuArmEmbeddedToolchain -e
winget install --id=Ninja-build.Ninja -e
winget install --id=Kitware.CMake -e
```

## Configuration

### WiFi

WiFi details are kept in `app/weather/wifi_secrets.h`, which git ignores so they are never committed. Create it from the template:

```bash
cp app/weather/wifi_secrets.example.h app/weather/wifi_secrets.h
```

Then fill in `WIFI_SSID` and `WIFI_PASSWORD`. The board only supports **2.4 GHz** networks with WPA2 (WPA3-only networks will not work). If `wifi_secrets.h` is missing, the build stops with a message telling you to create it.

### Location and display

Edit `app/weather/cloud_config.h`:

| Setting | What it does |
|---|---|
| `WEATHER_LATITUDE`, `WEATHER_LONGITUDE` | Your location in decimal degrees (south and west are negative). Find yours at [latlong.net](https://www.latlong.net). |
| `WEATHER_USE_IMPERIAL` | `1` for °F and mph, `0` for °C and km/h |
| `CLOCK_12_HOUR` | `1` for a 12-hour clock, `0` for 24-hour |
| `WEATHER_REFRESH_MINUTES` | How often to fetch new weather |
| `FORECAST_DAYS` | How many forecast days button A steps through (1–7) |
| `FORECAST_SCREEN_SECONDS` | How long the forecast screens stay up after the last press |
| `LED_ALERTS_ENABLED` | `1` for LED weather alerts, `0` to keep the LED off |
| `RAIN_ALERT_PERCENT` | Rain chance (%) that triggers the blue alert |
| `FREEZE_ALERT_TEMP` | Low temperature that triggers the icy white alert |
| `ALERT_TOMORROW_FROM_HOUR` | Hour from which alerts are about tomorrow |
| `QUIET_START_HOUR`, `QUIET_END_HOUR` | Quiet hours: LED off, and the screen saver turns the screen off |
| `LED_MAX_BRIGHTNESS` | Peak LED brightness, 0-2047 |
| `SCREENSAVER_MINUTES` | Minutes without a press or movement before the screen saver starts (`0` = never) |
| `SCREENSAVER_CONTRAST` | Screen brightness during the screen saver, 0-255 |
| `SCREEN_OFF_DURING_QUIET_HOURS` | `1` to turn the screen fully off at night instead of showing the screen saver |
| `MOTION_MOVE_THRESHOLD`, `MOTION_TAP_THRESHOLD` | How much movement or tapping wakes the screen (lower = more sensitive) |

## Build and flash

Build:

```bash
bash scripts/build.sh weather
```

The build script only applies the configuration name the first time it sets up the `build/` folder. When you switch configurations, or after moving the project folder, add `rebuild`:

```bash
bash scripts/build.sh weather rebuild
```

Plug the board in over USB. It shows up as a drive named `AZ3166`. Copy the firmware to it:

```bash
bash scripts/deploy.sh /Volumes/AZ3166
```

The board restarts and runs the new firmware. On Windows, use `scripts\build.ps1 -Config weather` and `scripts\deploy.ps1 -Destination D:`.

## Troubleshooting

Connect to the board's USB serial port at **115200 baud** to see detailed log messages (on macOS, `screen /dev/tty.usbmodem* 115200`).

| Screen shows | Likely cause |
|---|---|
| `WiFi error` | `WIFI_SSID` is empty: check `wifi_secrets.h` is filled in and saved, then rebuild. |
| Stuck on `Connecting` | Wrong password, a 5 GHz-only network, or a WPA3-only network. If the password contains `"` or `\`, escape it as `\"` or `\\`. |
| `Getting weather...` for a long time | No internet access, or the weather service is unreachable. It retries every minute. |

## Other configurations

The project also includes the original sample applications, selected the same way (`bash scripts/build.sh <name> rebuild`):

- `starter`: board and WiFi setup only.
- `telemetry`: reads the onboard sensors and prints the values.
- `mqtt`: publishes sensor data over MQTT (builds on `telemetry`).
- `arcade`: a collection of arcade games.

These use `cloud_config.h` in their own folders for WiFi settings.

## Credits

Based on the MXChip AZ3166 sample from [Eclipse ThreadX SampleX](https://github.com/eclipse-threadx/samplex), originally developed by Microsoft and adapted by the Eclipse Foundation. Licensed under the MIT license; see [LICENSE](LICENSE).
