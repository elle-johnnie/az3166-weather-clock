# AZ3166 Weather Clock

A WiFi weather clock for the [MXChip AZ3166](https://docs.mxchip.com/en/nr6ggk/blyezpv6gkqicywi.html) IoT DevKit, running [Eclipse ThreadX](https://github.com/eclipse-threadx) and NetX Duo.

- **Clock screen:** local time, day and date, current temperature and conditions, humidity and wind.
- **Button A:** today's forecast: conditions, high/low, chance of rain and max wind. Press A again, or wait 15 seconds, to return to the clock.
- **Button B:** refresh the weather now (it otherwise updates every 15 minutes).

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
| `FORECAST_SCREEN_SECONDS` | How long the forecast screen stays up |

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
