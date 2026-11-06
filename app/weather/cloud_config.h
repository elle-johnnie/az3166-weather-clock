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
#define WEATHER_LATITUDE  "47.61"
#define WEATHER_LONGITUDE "-122.33"

// 1 = Fahrenheit and mph, 0 = Celsius and km/h
#define WEATHER_USE_IMPERIAL 1

// 1 = 12-hour clock (3:45 PM), 0 = 24-hour clock (15:45)
#define CLOCK_12_HOUR 1

// How often to fetch new weather data, in minutes
#define WEATHER_REFRESH_MINUTES 15

// How long the forecast screen stays up before returning to the clock, in seconds
#define FORECAST_SCREEN_SECONDS 15

#endif // _CLOUD_CONFIG_H
