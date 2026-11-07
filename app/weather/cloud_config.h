/*
 * Weather clock configuration.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _CLOUD_CONFIG_H
#define _CLOUD_CONFIG_H

typedef enum
{
    None         = 0,
    WEP          = 1,
    WPA_PSK_TKIP = 2,
    WPA2_PSK_AES = 3
} WiFi_Mode;

// ----------------------------------------------------------------------------
// WiFi connection config (2.4 GHz networks only)
// ----------------------------------------------------------------------------
#define HOSTNAME      "weather-clock"
// WIFI_SSID and WIFI_PASSWORD live in wifi_secrets.h, which git ignores.
// Copy wifi_secrets.example.h to wifi_secrets.h and fill it in.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Missing app/weather/wifi_secrets.h: copy wifi_secrets.example.h to wifi_secrets.h and add your WiFi details"
#endif
#define WIFI_MODE     WPA2_PSK_AES

// ----------------------------------------------------------------------------
// Weather config
//
// Weather comes from Open-Meteo (https://open-meteo.com), which is free and
// needs no API key. Set your location as decimal degrees (south and west are
// negative). Find yours at https://www.latlong.net
// ----------------------------------------------------------------------------
#define WEATHER_LATITUDE  "47.42"
#define WEATHER_LONGITUDE "-120.31"

// 1 = Fahrenheit and mph, 0 = Celsius and km/h
#define WEATHER_USE_IMPERIAL 1

// 1 = 12-hour clock (3:45 PM), 0 = 24-hour clock (15:45)
#ifndef CLOCK_12_HOUR
#define CLOCK_12_HOUR 0
#endif

// Clock screen layout. The screen's top 16 rows are always yellow and the rest
// always blue, so each layout keeps text inside one color band.
//   1 = time, day and date in the yellow header; current temperature and
//       today's high/low/rain chance below in blue
//   2 = day, date and temperature in the yellow header; big clock and
//       humidity below in blue
//   3 = day, date and temperature in the yellow header; big clock with
//       today's high/low/rain chance beside it in blue
#ifndef CLOCK_LAYOUT
#define CLOCK_LAYOUT 1
#endif

// How often to fetch new weather data, in minutes
#define WEATHER_REFRESH_MINUTES 30

// Number of forecast days to show (1-7). Button A steps through them.
#define FORECAST_DAYS 5

// How long the forecast screens stay up after the last button press before
// returning to the clock, in seconds
#define FORECAST_SCREEN_SECONDS 15

// ----------------------------------------------------------------------------
// RGB LED weather alerts
//
// The LED slowly "breathes" blue when rain is likely and icy white when it
// will be near freezing. With both, it alternates between the two colors.
// ----------------------------------------------------------------------------

// 1 = alerts on, 0 = LED always off
#define LED_ALERTS_ENABLED 1

// Rain alert when the day's chance of rain is at least this percent
#define RAIN_ALERT_PERCENT 50

// Freezing alert when the day's low (or the current temperature) is at or
// below this, in your display units (F or C)
#if WEATHER_USE_IMPERIAL
#define FREEZE_ALERT_TEMP 34
#else
#define FREEZE_ALERT_TEMP 1
#endif

// From this hour on (0-23), LED alerts and the clock screen's forecast icon
// are about tomorrow instead of today, so in the evening they tell you what
// to expect in the morning.
// The LED also stays off during quiet hours (QUIET_START_HOUR, below).
#define ALERT_TOMORROW_FROM_HOUR 18

// Peak brightness, 0-2047. The LED is very bright at full power.
#define LED_MAX_BRIGHTNESS 500

// Length of one slow breath (fade in and out), in seconds
#define LED_BREATH_SECONDS 4

// ----------------------------------------------------------------------------
// Quiet hours (local time, 0-23): the LED stays off, and if
// SCREEN_OFF_DURING_QUIET_HOURS is 1, the screen saver turns the screen fully
// off. Set both to the same value to never go quiet.
// ----------------------------------------------------------------------------
#define QUIET_START_HOUR 22
#define QUIET_END_HOUR   7

// ----------------------------------------------------------------------------
// Screen saver (prevents OLED burn-in)
//
// After SCREENSAVER_MINUTES with no button press or movement, the screen dims
// and shows a small clock that slowly drifts around. A button press, a tap on
// the board, or picking it up / bumping it wakes it.
// ----------------------------------------------------------------------------

// Minutes of inactivity before the screen saver starts (0 = never)
#define SCREENSAVER_MINUTES 1

// Screen brightness while the screen saver runs, 0-255 (normal is 255)
#define SCREENSAVER_CONTRAST 16

// The drifting clock moves one pixel every this many seconds
#define SCREENSAVER_STEP_SECONDS 2

// 1 = turn the screen fully off during quiet hours (while the saver is on)
#define SCREEN_OFF_DURING_QUIET_HOURS 1

// Motion sensitivity. Lower = more sensitive.
//   MOTION_MOVE_THRESHOLD: 1-63, steps of 31 mg (bumps, pick-ups)
//   MOTION_TAP_THRESHOLD:  1-31, steps of 62.5 mg (taps on the board)
#define MOTION_MOVE_THRESHOLD 3
#define MOTION_TAP_THRESHOLD  8

#endif // _CLOUD_CONFIG_H
