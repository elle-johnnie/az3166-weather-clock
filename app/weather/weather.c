/*
 * Fetches current conditions and today's forecast from Open-Meteo over plain
 * HTTP (no API key, no TLS needed), then parses the JSON with jsmn.
 *
 * SPDX-License-Identifier: MIT
 */

#include "weather.h"

#include <stdio.h>
#include <string.h>

#include "jsmn.h"
#include "nx_api.h"
#include "nx_web_http_client.h"
#include "nxd_dns.h"

#include "cloud_config.h"
#include "wwd_networking.h"

#define WEATHER_HOST    "api.open-meteo.com"
#define WEATHER_PORT    80
#define WEATHER_TIMEOUT (10 * NX_IP_PERIODIC_RATE)

#if FORECAST_DAYS < 1 || FORECAST_DAYS > WEATHER_MAX_DAYS
#error "FORECAST_DAYS in cloud_config.h must be between 1 and WEATHER_MAX_DAYS (7)"
#endif
#define STRINGIFY_(x)     #x
#define STRINGIFY(x)      STRINGIFY_(x)
#define FORECAST_DAYS_STR STRINGIFY(FORECAST_DAYS)

#if WEATHER_USE_IMPERIAL
#define WEATHER_UNITS "&temperature_unit=fahrenheit&wind_speed_unit=mph"
#else
#define WEATHER_UNITS ""
#endif

#define WEATHER_RESOURCE                                                                                 \
    "/v1/forecast?latitude=" WEATHER_LATITUDE "&longitude=" WEATHER_LONGITUDE                          \
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"                         \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"          \
    "wind_speed_10m_max"                                                                                \
    "&timezone=auto&forecast_days=" FORECAST_DAYS_STR WEATHER_UNITS

#define BODY_BUFFER_SIZE 3072
#define MAX_TOKENS       160

static NX_WEB_HTTP_CLIENT http_client;
static char body[BODY_BUFFER_SIZE];
static jsmntok_t tokens[MAX_TOKENS];

// Parse a JSON number (e.g. "-3.6") and round it to the nearest integer.
static long parse_rounded(const char* s, int len)
{
    int i         = 0;
    int negative  = 0;
    long whole    = 0;
    int round_up  = 0;

    if (i < len && s[i] == '-')
    {
        negative = 1;
        i++;
    }
    while (i < len && s[i] >= '0' && s[i] <= '9')
    {
        whole = whole * 10 + (s[i] - '0');
        i++;
    }
    if (i < len - 1 && s[i] == '.')
    {
        round_up = (s[i + 1] >= '5');
    }

    whole += round_up;
    return negative ? -whole : whole;
}

static int token_equals(const char* json, const jsmntok_t* tok, const char* str)
{
    int len = (int)strlen(str);
    return tok->type == JSMN_STRING && (tok->end - tok->start) == len &&
           strncmp(json + tok->start, str, len) == 0;
}

// Find `key` among tokens from index `from` up to the end of the object at
// index `parent` (or the whole document when parent < 0). Returns the index
// of the key's value token, or -1.
static int find_value(const char* json, int count, int parent, const char* key)
{
    int from    = parent < 0 ? 0 : parent + 1;
    int end_pos = parent < 0 ? tokens[0].end : tokens[parent].end;

    for (int i = from; i < count - 1 && tokens[i].start < end_pos; i++)
    {
        if (tokens[i].size == 1 && token_equals(json, &tokens[i], key))
        {
            return i + 1;
        }
    }
    return -1;
}

// Read a number value. For arrays (daily values), reads the first element.
static int read_number(const char* json, int count, int parent, const char* key, long* out)
{
    int idx = find_value(json, count, parent, key);
    if (idx < 0)
    {
        return 0;
    }
    if (tokens[idx].type == JSMN_ARRAY)
    {
        if (tokens[idx].size < 1 || idx + 1 >= count)
        {
            return 0;
        }
        idx++;
    }
    if (tokens[idx].type != JSMN_PRIMITIVE)
    {
        return 0; // e.g. null
    }
    *out = parse_rounded(json + tokens[idx].start, tokens[idx].end - tokens[idx].start);
    return 1;
}

// Read element `index` of the number array `key`, or `fallback` if missing or null.
static int read_array_number(const char* json, int count, int parent, const char* key, int index, int fallback)
{
    int idx = find_value(json, count, parent, key);
    if (idx < 0 || tokens[idx].type != JSMN_ARRAY || index >= tokens[idx].size || idx + 1 + index >= count)
    {
        return fallback;
    }
    idx += 1 + index; // array elements are plain numbers, so they follow the array token in order
    if (tokens[idx].type != JSMN_PRIMITIVE || json[tokens[idx].start] == 'n') // "null"
    {
        return fallback;
    }
    return (int)parse_rounded(json + tokens[idx].start, tokens[idx].end - tokens[idx].start);
}

static UINT http_get(char* buffer, ULONG buffer_size, ULONG* length)
{
    UINT status;
    ULONG ip_address = 0;
    NXD_ADDRESS server;
    NX_PACKET* packet;
    ULONG copied;

    *length = 0;

    if ((status = nx_dns_host_by_name_get(&nx_dns_client, (UCHAR*)WEATHER_HOST, &ip_address, WEATHER_TIMEOUT)))
    {
        printf("ERROR: DNS lookup for %s failed (0x%04x)\r\n", WEATHER_HOST, status);
        return status;
    }

    server.nxd_ip_version    = NX_IP_VERSION_V4;
    server.nxd_ip_address.v4 = ip_address;

    if ((status = nx_web_http_client_create(
             &http_client, "Weather", &nx_ip, nx_ip.nx_ip_default_packet_pool, 4096)))
    {
        printf("ERROR: HTTP client create failed (0x%04x)\r\n", status);
        return status;
    }

    status = nx_web_http_client_get_start(
        &http_client, &server, WEATHER_PORT, WEATHER_RESOURCE, WEATHER_HOST, NX_NULL, NX_NULL, WEATHER_TIMEOUT);

    if (status != NX_SUCCESS)
    {
        printf("ERROR: HTTP GET failed (0x%04x)\r\n", status);
    }
    else
    {
        // Read the body; it may arrive in several packets.
        while (1)
        {
            status = nx_web_http_client_response_body_get(&http_client, &packet, WEATHER_TIMEOUT);

            if (status != NX_SUCCESS && status != NX_WEB_HTTP_GET_DONE)
            {
                printf("ERROR: HTTP response failed (0x%04x)\r\n", status);
                break;
            }

            copied = 0;
            if (*length < buffer_size - 1)
            {
                nx_packet_data_extract_offset(packet, 0, buffer + *length, buffer_size - 1 - *length, &copied);
                *length += copied;
            }
            nx_packet_release(packet);

            if (status == NX_WEB_HTTP_GET_DONE)
            {
                status = NX_SUCCESS;
                break;
            }
        }
    }

    buffer[*length] = 0;
    nx_web_http_client_delete(&http_client);

    return status;
}

UINT weather_fetch(WEATHER_DATA* out)
{
    UINT status;
    ULONG length;
    jsmn_parser parser;
    int count;
    int current;
    int daily;
    long v;
    WEATHER_DATA w;

    printf("Fetching weather...\r\n");

    if ((status = http_get(body, sizeof(body), &length)))
    {
        return status;
    }

    jsmn_init(&parser);
    count = jsmn_parse(&parser, body, length, tokens, MAX_TOKENS);
    if (count < 1 || tokens[0].type != JSMN_OBJECT)
    {
        printf("ERROR: Could not parse weather JSON (%d)\r\n", count);
        return NX_NOT_SUCCESSFUL;
    }

    current = find_value(body, count, -1, "current");
    daily   = find_value(body, count, -1, "daily");
    if (current < 0 || daily < 0)
    {
        printf("ERROR: Unexpected weather response: %s\r\n", body);
        return NX_NOT_SUCCESSFUL;
    }

    memset(&w, 0, sizeof(w));
    w.is_day = 1;

    if (read_number(body, count, -1, "utc_offset_seconds", &v))           w.utc_offset_seconds = v;
    if (read_number(body, count, current, "temperature_2m", &v))          w.temperature        = (int)v;
    if (read_number(body, count, current, "relative_humidity_2m", &v))    w.humidity           = (int)v;
    if (read_number(body, count, current, "wind_speed_10m", &v))          w.wind_speed         = (int)v;
    if (read_number(body, count, current, "weather_code", &v))            w.weather_code       = (int)v;
    if (read_number(body, count, current, "is_day", &v))                  w.is_day             = (int)v;
    // Daily forecast: each field is an array with one entry per day
    int dates = find_value(body, count, daily, "time");
    if (dates < 0 || tokens[dates].type != JSMN_ARRAY)
    {
        printf("ERROR: Weather response has no daily dates\r\n");
        return NX_NOT_SUCCESSFUL;
    }

    w.day_count = tokens[dates].size;
    if (w.day_count > WEATHER_MAX_DAYS)
    {
        w.day_count = WEATHER_MAX_DAYS;
    }

    for (int i = 0; i < w.day_count; i++)
    {
        WEATHER_DAY* d = &w.days[i];
        int t          = dates + 1 + i;

        // Dates look like "2026-11-06"
        if (dates + 1 + i >= count || tokens[t].end - tokens[t].start != 10)
        {
            w.day_count = i;
            break;
        }
        d->year  = (int)parse_rounded(body + tokens[t].start, 4);
        d->month = (int)parse_rounded(body + tokens[t].start + 5, 2);
        d->day   = (int)parse_rounded(body + tokens[t].start + 8, 2);

        d->code        = read_array_number(body, count, daily, "weather_code", i, 0);
        d->high        = read_array_number(body, count, daily, "temperature_2m_max", i, 0);
        d->low         = read_array_number(body, count, daily, "temperature_2m_min", i, 0);
        d->rain_chance = read_array_number(body, count, daily, "precipitation_probability_max", i, WEATHER_UNKNOWN);
        d->wind_max    = read_array_number(body, count, daily, "wind_speed_10m_max", i, 0);
    }

    if (w.day_count == 0)
    {
        printf("ERROR: Weather response has no forecast days\r\n");
        return NX_NOT_SUCCESSFUL;
    }

    *out = w;

    printf("Weather: %d, %s, %d forecast days\r\n", w.temperature, weather_code_text(w.weather_code), w.day_count);
    for (int i = 0; i < w.day_count; i++)
    {
        printf("  %04d-%02d-%02d %-11s hi %d lo %d rain %d%%\r\n",
            w.days[i].year,
            w.days[i].month,
            w.days[i].day,
            weather_code_text(w.days[i].code),
            w.days[i].high,
            w.days[i].low,
            w.days[i].rain_chance);
    }

    return NX_SUCCESS;
}

// WMO weather interpretation codes: https://open-meteo.com/en/docs
const char* weather_code_text(int code)
{
    switch (code)
    {
        case 0:  return "Clear";
        case 1:  return "Mostly clr";
        case 2:  return "Partly cldy";
        case 3:  return "Overcast";
        case 45:
        case 48: return "Fog";
        case 51:
        case 53:
        case 55: return "Drizzle";
        case 56:
        case 57: return "Frz drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66:
        case 67: return "Frz rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80:
        case 81: return "Showers";
        case 82: return "Hvy showers";
        case 85:
        case 86: return "Snow showrs";
        case 95: return "T-storms";
        case 96:
        case 99: return "Hail storm";
        default: return "Unknown";
    }
}
