/*
 * Fetches current conditions and today's forecast from Open-Meteo.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _WEATHER_H
#define _WEATHER_H

#include "tx_api.h"

typedef struct
{
    // Current conditions
    int temperature;      // degrees, rounded
    int humidity;         // percent
    int wind_speed;       // mph or km/h, rounded
    int weather_code;     // WMO weather code

    // Today's forecast
    int today_code;       // WMO weather code
    int today_high;
    int today_low;
    int today_rain_chance; // percent
    int today_wind_max;

    // Offset of the location's local time from UTC, in seconds (includes DST)
    long utc_offset_seconds;
} WEATHER_DATA;

// Fetch the latest weather. Returns NX_SUCCESS (0) on success.
UINT weather_fetch(WEATHER_DATA* out);

// Short description of a WMO weather code, at most 11 characters.
const char* weather_code_text(int code);

#endif // _WEATHER_H
