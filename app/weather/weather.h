/*
 * Fetches current conditions and a multi-day forecast from Open-Meteo.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _WEATHER_H
#define _WEATHER_H

#include "tx_api.h"

// Most days the forecast can hold. FORECAST_DAYS in cloud_config.h picks how
// many are actually fetched (up to this limit).
#define WEATHER_MAX_DAYS 7

#define WEATHER_UNKNOWN (-999) // value missing from the response

typedef struct
{
    int year, month, day; // date of this forecast day (month 1-12)
    int code;             // WMO weather code
    int high;
    int low;
    int rain_chance;      // percent, or WEATHER_UNKNOWN
    int wind_max;
} WEATHER_DAY;

typedef struct
{
    // Current conditions
    int temperature;      // degrees, rounded
    int humidity;         // percent
    int wind_speed;       // mph or km/h, rounded
    int weather_code;     // WMO weather code
    int is_day;           // 1 = daytime, 0 = night (for sun/moon icons)

    // Daily forecast; days[0] is the first day returned (normally today)
    WEATHER_DAY days[WEATHER_MAX_DAYS];
    int day_count;

    // Offset of the location's local time from UTC, in seconds (includes DST)
    long utc_offset_seconds;
} WEATHER_DATA;

// Fetch the latest weather. Returns NX_SUCCESS (0) on success.
UINT weather_fetch(WEATHER_DATA* out);

// Short description of a WMO weather code, at most 11 characters.
const char* weather_code_text(int code);

#endif // _WEATHER_H
