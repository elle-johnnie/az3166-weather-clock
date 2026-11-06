/*
 * Weather clock for the MXChip AZ3166.
 *
 *   Clock screen (default): local time, day/date, current temperature,
 *                           conditions, humidity and wind.
 *   Button A:               steps through the daily forecast: today,
 *                           tomorrow, and on (FORECAST_DAYS in cloud_config.h),
 *                           then back to the clock. Each day shows conditions,
 *                           high/low, rain chance and max wind. After
 *                           FORECAST_SCREEN_SECONDS with no press, it returns
 *                           to the clock on its own.
 *   Button B:               refresh the weather now.
 *
 * Time comes from SNTP; the local time zone (including daylight saving)
 * comes from the weather service for the configured location.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "tx_api.h"

#include "board_init.h"
#include "cmsis_utils.h"
#include "screen.h"
#include "sntp_client.h"
#include "ssd1306.h"
#include "weather.h"
#include "wwd_networking.h"

#include "cloud_config.h"

#define APP_THREAD_STACK_SIZE 8192
#define APP_THREAD_PRIORITY   4

#define LOOP_INTERVAL_TICKS   (TX_TIMER_TICKS_PER_SECOND / 20) // 50 ms: button polling rate
#define RETRY_SECONDS         60                               // retry delay after a failed fetch

typedef enum
{
    SCREEN_CLOCK,
    SCREEN_FORECAST
} SCREEN;

static TX_THREAD app_thread;
static ULONG app_thread_stack[APP_THREAD_STACK_SIZE / sizeof(ULONG)];

static WEATHER_DATA weather;
static int weather_valid = 0;

static const char* const DAY_NAMES[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* const MONTH_NAMES[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

#if WEATHER_USE_IMPERIAL
#define TEMP_UNIT  "F"
#define SPEED_UNIT "mph"
#else
#define TEMP_UNIT  "C"
#define SPEED_UNIT "kmh"
#endif

typedef struct
{
    int year, month, day; // month 1-12
    int hour, minute, second;
    int weekday; // 0 = Sunday
} LOCAL_TIME;

// Convert a Unix timestamp to calendar fields (proleptic Gregorian calendar).
static void unix_to_local(long long t, LOCAL_TIME* lt)
{
    long long days = t / 86400;
    long secs      = (long)(t % 86400);
    if (secs < 0)
    {
        secs += 86400;
        days -= 1;
    }

    lt->hour    = secs / 3600;
    lt->minute  = (secs / 60) % 60;
    lt->second  = secs % 60;
    lt->weekday = (int)((days + 4) % 7); // 1970-01-01 was a Thursday
    if (lt->weekday < 0)
    {
        lt->weekday += 7;
    }

    // Days to civil date (Howard Hinnant's algorithm)
    long long z    = days + 719468;
    long long era  = (z >= 0 ? z : z - 146096) / 146097;
    long doe       = (long)(z - era * 146097);
    long yoe       = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy       = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp        = (5 * doy + 2) / 153;
    lt->day        = (int)(doy - (153 * mp + 2) / 5 + 1);
    lt->month      = (int)(mp < 10 ? mp + 3 : mp - 9);
    lt->year       = (int)(yoe + era * 400 + (lt->month <= 2));
}

static void get_local_time(LOCAL_TIME* lt)
{
    long long now = (long long)sntp_time_get();
    if (weather_valid)
    {
        now += weather.utc_offset_seconds;
    }
    unix_to_local(now, lt);
}

static void draw_text(int x, int y, const char* text, FontDef font)
{
    ssd1306_SetCursor(x, y);
    ssd1306_WriteString((char*)text, font, White);
}

static void draw_clock_screen(void)
{
    LOCAL_TIME lt;
    char buf[24];

    get_local_time(&lt);
    ssd1306_Fill(Black);

    // Big time, top left
#if CLOCK_12_HOUR
    int hour12 = lt.hour % 12;
    if (hour12 == 0)
    {
        hour12 = 12;
    }
    snprintf(buf, sizeof(buf), "%d:%02d", hour12, lt.minute);
#else
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.hour, lt.minute);
#endif
    draw_text(0, 0, buf, Font_16x26);

    // Small AM/PM + day and date, top right
#if CLOCK_12_HOUR
    draw_text(86, 0, lt.hour < 12 ? "AM" : "PM", Font_7x10);
#endif
    draw_text(86, 10, DAY_NAMES[lt.weekday], Font_7x10);
    snprintf(buf, sizeof(buf), "%s %d", MONTH_NAMES[lt.month - 1], lt.day);
    draw_text(86, 20, buf, Font_7x10);

    if (weather_valid)
    {
        // Temperature + conditions
        snprintf(buf, sizeof(buf), "%d%s", weather.temperature, TEMP_UNIT);
        draw_text(0, 31, buf, Font_11x18);
        draw_text(48, 35, weather_code_text(weather.weather_code), Font_7x10);

        // Humidity + wind
        snprintf(buf, sizeof(buf), "Hum %d%%  %d%s", weather.humidity, weather.wind_speed, SPEED_UNIT);
        draw_text(0, 53, buf, Font_7x10);
    }
    else
    {
        draw_text(0, 36, "Getting", Font_7x10);
        draw_text(0, 48, "weather...", Font_7x10);
    }

    ssd1306_UpdateScreen();
}

// Days since 1970-01-01 for a calendar date (inverse of unix_to_local's date part).
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Index of the first forecast day that is today or later. Data fetched before
// midnight still starts at "yesterday" until the next refresh, so skip it.
static int first_upcoming_day(void)
{
    LOCAL_TIME lt;
    get_local_time(&lt);
    long today = days_from_civil(lt.year, lt.month, lt.day);

    for (int i = 0; i < weather.day_count; i++)
    {
        const WEATHER_DAY* d = &weather.days[i];
        if (days_from_civil(d->year, d->month, d->day) >= today)
        {
            return i;
        }
    }
    return weather.day_count; // all stale
}

// Number of forecast pages button A can step through.
static int forecast_page_count(void)
{
    return weather_valid ? weather.day_count - first_upcoming_day() : 0;
}

// Draws forecast page `page` (0 = today).
static void draw_forecast_screen(int page)
{
    char buf[24];
    LOCAL_TIME lt;
    int pages = forecast_page_count();

    get_local_time(&lt);
    ssd1306_Fill(Black);

    if (page >= pages)
    {
        draw_text(0, 0, "Forecast", Font_7x10);
        ssd1306_Line(0, 11, 127, 11, White);
        draw_text(0, 24, "No data yet", Font_11x18);
        ssd1306_UpdateScreen();
        return;
    }

    const WEATHER_DAY* d = &weather.days[first_upcoming_day() + page];
    long day_number      = days_from_civil(d->year, d->month, d->day);
    long days_ahead      = day_number - days_from_civil(lt.year, lt.month, lt.day);
    const char* weekday  = DAY_NAMES[((day_number + 4) % 7 + 7) % 7];

    // Header: "Today Thu", "Tomorrow Fri", or "Sat Nov 8"
    if (days_ahead == 0)
    {
        snprintf(buf, sizeof(buf), "Today %s", weekday);
    }
    else if (days_ahead == 1)
    {
        snprintf(buf, sizeof(buf), "Tomorrow %s", weekday);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s %s %d", weekday, MONTH_NAMES[d->month - 1], d->day);
    }
    draw_text(0, 0, buf, Font_7x10);

    // Page dots, top right: filled for the day shown
    for (int i = 0; i < pages; i++)
    {
        int x = 127 - 3 - (pages - 1 - i) * 7;
        if (i == page)
        {
            // Solid round dot, 5 px across
            for (int dy = -2; dy <= 2; dy++)
            {
                int half = (dy == -2 || dy == 2) ? 1 : 2;
                ssd1306_Line(x - half, 4 + dy, x + half, 4 + dy, White);
            }
        }
        else
        {
            ssd1306_DrawPixel(x, 4, White);
        }
    }
    ssd1306_Line(0, 11, 127, 11, White);

    draw_text(0, 15, weather_code_text(d->code), Font_11x18);

    snprintf(buf, sizeof(buf), "Hi %d%s  Lo %d%s", d->high, TEMP_UNIT, d->low, TEMP_UNIT);
    draw_text(0, 36, buf, Font_7x10);

    if (d->rain_chance == WEATHER_UNKNOWN)
    {
        snprintf(buf, sizeof(buf), "Rain --  Wind %d", d->wind_max);
    }
    else
    {
        snprintf(buf, sizeof(buf), "Rain %d%% Wind %d", d->rain_chance, d->wind_max);
    }
    draw_text(0, 50, buf, Font_7x10);

    ssd1306_UpdateScreen();
}

static void draw_status(const char* line1, const char* line2)
{
    ssd1306_Fill(Black);
    draw_text(0, 10, line1, Font_11x18);
    if (line2)
    {
        draw_text(0, 36, line2, Font_7x10);
    }
    ssd1306_UpdateScreen();
}

static int refresh_weather(void)
{
    WEATHER_DATA fresh;

    CLOUD_LED_ON();
    UINT status = weather_fetch(&fresh);
    CLOUD_LED_OFF();

    if (status == NX_SUCCESS)
    {
        weather       = fresh;
        weather_valid = 1;
        return 1;
    }
    return 0;
}

static void app_thread_entry(ULONG parameter)
{
    UINT status;
    SCREEN screen          = SCREEN_CLOCK;
    ULONG forecast_shown   = 0;
    int forecast_page      = 0;
    ULONG next_fetch       = 0;
    int last_drawn_minute  = -1;
    int a_was_pressed      = 0;
    int b_was_pressed      = 0;
    int redraw             = 1;

    printf("Starting weather clock\r\n");

    draw_status("Connecting", WIFI_SSID);

    if ((status = wwd_network_init(WIFI_SSID, WIFI_PASSWORD, WIFI_MODE)))
    {
        printf("ERROR: Failed to initialize the network (0x%08x)\r\n", status);
        draw_status("WiFi error", "Check cloud_config.h");
        return;
    }

    // Connects WiFi, gets an IP address and syncs the clock via SNTP
    if ((status = wwd_network_connect()))
    {
        printf("ERROR: Failed to connect (0x%08x)\r\n", status);
        draw_status("Net error", "Press reset to retry");
        return;
    }
    WIFI_LED_ON();

    while (1)
    {
        ULONG now = tx_time_get();

        // --- Buttons (active low, polled with edge detection) ---
        int a_pressed = BUTTON_A_IS_PRESSED;
        int b_pressed = BUTTON_B_IS_PRESSED;

        if (a_pressed && !a_was_pressed)
        {
            // Clock -> today -> tomorrow -> ... -> last day -> clock
            if (screen == SCREEN_CLOCK)
            {
                screen        = SCREEN_FORECAST;
                forecast_page = 0;
            }
            else if (forecast_page + 1 < forecast_page_count())
            {
                forecast_page++;
            }
            else
            {
                screen = SCREEN_CLOCK;
            }
            forecast_shown = now; // timeout counts from the last press
            redraw         = 1;
        }

        if (b_pressed && !b_was_pressed)
        {
            draw_status("Updating", "weather...");
            next_fetch = now; // fetch right away
            redraw     = 1;
        }

        a_was_pressed = a_pressed;
        b_was_pressed = b_pressed;

        // --- Weather refresh ---
        if ((LONG)(now - next_fetch) >= 0)
        {
            ULONG delay_seconds = refresh_weather() ? WEATHER_REFRESH_MINUTES * 60 : RETRY_SECONDS;
            next_fetch          = tx_time_get() + delay_seconds * TX_TIMER_TICKS_PER_SECOND;
            redraw              = 1;

            // New data may have fewer upcoming days than the page being shown
            if (forecast_page >= forecast_page_count() && forecast_page > 0)
            {
                forecast_page = forecast_page_count() > 0 ? forecast_page_count() - 1 : 0;
            }
        }

        // --- Forecast screen timeout ---
        if (screen == SCREEN_FORECAST &&
            tx_time_get() - forecast_shown >= FORECAST_SCREEN_SECONDS * TX_TIMER_TICKS_PER_SECOND)
        {
            screen = SCREEN_CLOCK;
            redraw = 1;
        }

        // --- Draw ---
        if (screen == SCREEN_CLOCK)
        {
            LOCAL_TIME lt;
            get_local_time(&lt);
            if (redraw || lt.minute != last_drawn_minute)
            {
                draw_clock_screen();
                last_drawn_minute = lt.minute;
            }
        }
        else if (redraw)
        {
            draw_forecast_screen(forecast_page);
            last_drawn_minute = -1;
        }
        redraw = 0;

        tx_thread_sleep(LOOP_INTERVAL_TICKS);
    }
}

void tx_application_define(void* first_unused_memory)
{
    systick_interval_set(TX_TIMER_TICKS_PER_SECOND);

    UINT status = tx_thread_create(&app_thread,
        "Weather Clock",
        app_thread_entry,
        0,
        app_thread_stack,
        APP_THREAD_STACK_SIZE,
        APP_THREAD_PRIORITY,
        APP_THREAD_PRIORITY,
        TX_NO_TIME_SLICE,
        TX_AUTO_START);

    if (status != TX_SUCCESS)
    {
        printf("ERROR: Weather clock thread creation failed\r\n");
    }
}

int main(void)
{
    board_init();
    tx_kernel_enter();
    return 0;
}
