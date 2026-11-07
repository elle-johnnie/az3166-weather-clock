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
 *   RGB LED:                breathes blue when rain is likely and icy white
 *                           near freezing (today, or tomorrow in the evening),
 *                           off during quiet hours. See cloud_config.h.
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
#include "audio.h"
#include "led_alerts.h"
#include "morning_tune.h"
#include "motion.h"
#include "weather.h"
#include "weather_icons.h"
#include "wwd_networking.h"

#include "cloud_config.h"

#define APP_THREAD_STACK_SIZE 8192
#define APP_THREAD_PRIORITY   4

#define LOOP_INTERVAL_TICKS   (TX_TIMER_TICKS_PER_SECOND / 20) // 50 ms: button polling rate
#define RETRY_SECONDS         60                               // retry delay after a failed fetch

typedef enum
{
    SCREEN_CLOCK,
    SCREEN_FORECAST,
    SCREEN_SAVER
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

// The OLED is a two-color panel: rows 0-15 always light up yellow and rows
// 16-63 always light up blue. Layouts keep each element inside one band.
#define YELLOW_BAND_BOTTOM 15
#define BLUE_BAND_TOP      17 // first row safely below the boundary

static void draw_text_right(int right_x, int y, const char* text, FontDef font)
{
    draw_text(right_x - (int)strlen(text) * font.FontWidth, y, text, font);
}

static void format_time(const LOCAL_TIME* lt, char* buf, size_t size, int with_ampm)
{
#if CLOCK_12_HOUR
    int hour12 = lt->hour % 12;
    if (hour12 == 0)
    {
        hour12 = 12;
    }
    if (with_ampm)
    {
        snprintf(buf, size, "%d:%02d%s", hour12, lt->minute, lt->hour < 12 ? "AM" : "PM");
    }
    else
    {
        snprintf(buf, size, "%d:%02d", hour12, lt->minute);
    }
#else
    (void)with_ampm;
    snprintf(buf, size, "%02d:%02d", lt->hour, lt->minute);
#endif
}

static int first_upcoming_day(void);

// Not every layout uses every helper below; this keeps -Werror quiet.
#define MAYBE_UNUSED __attribute__((unused))

// 5x6 icons for the 7x10 font, one byte per row, bit 4 = leftmost pixel
MAYBE_UNUSED static const uint8_t ICON_UP[6]   = {0x04, 0x0E, 0x1F, 0x04, 0x04, 0x04};
MAYBE_UNUSED static const uint8_t ICON_DOWN[6] = {0x04, 0x04, 0x04, 0x1F, 0x0E, 0x04};
MAYBE_UNUSED static const uint8_t ICON_DROP[6] = {0x04, 0x04, 0x0E, 0x1F, 0x1F, 0x0E};

// Draws an icon at the height of a 7x10 text line starting at row y.
MAYBE_UNUSED static void draw_icon(int x, int y, const uint8_t icon[6])
{
    for (int row = 0; row < 6; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            if (icon[row] & (0x10 >> col))
            {
                ssd1306_DrawPixel(x + col, y + 2 + row, White);
            }
        }
    }
}

// Today's high, low and rain chance as a small icon column at x.
MAYBE_UNUSED static void draw_today_column(int x, int y)
{
    char buf[12];
    int today = first_upcoming_day();
    if (today >= weather.day_count)
    {
        return;
    }
    const WEATHER_DAY* d = &weather.days[today];

    draw_icon(x, y, ICON_UP);
    snprintf(buf, sizeof(buf), "%d%s", d->high, TEMP_UNIT);
    draw_text(x + 8, y, buf, Font_7x10);

    draw_icon(x, y + 11, ICON_DOWN);
    snprintf(buf, sizeof(buf), "%d%s", d->low, TEMP_UNIT);
    draw_text(x + 8, y + 11, buf, Font_7x10);

    if (d->rain_chance != WEATHER_UNKNOWN)
    {
        draw_icon(x, y + 22, ICON_DROP);
        snprintf(buf, sizeof(buf), "%d%%", d->rain_chance);
        draw_text(x + 8, y + 22, buf, Font_7x10);
    }
}

// Current conditions and wind along the bottom row.
// The forecast day that matters next: today, or tomorrow once it's evening
// (from ALERT_TOMORROW_FROM_HOUR). Returns an index into weather.days, which
// may be past the end if there is no data for that day.
static int coming_up_day(void)
{
    LOCAL_TIME lt;
    get_local_time(&lt);

    int day = first_upcoming_day();
    if (lt.hour >= ALERT_TOMORROW_FROM_HOUR)
    {
        day++;
    }
    return day;
}

// Bottom row: current conditions icon -> coming-up forecast icon, then wind.
//
//   [cloud] -> [snow]          6mph
// Current conditions icon -> coming-up forecast icon, 46 px wide and 16 tall,
// with its top-left corner at (x, y).
static void draw_icon_pair(int x, int y)
{
    weather_icon_draw(weather_icon(weather.weather_code, weather.is_day), x, y);

    int day = coming_up_day();
    if (day < weather.day_count)
    {
        // Arrow, 8 px wide
        ssd1306_Line(x + 19, y + 7, x + 26, y + 7, White);
        ssd1306_Line(x + 23, y + 4, x + 26, y + 7, White);
        ssd1306_Line(x + 23, y + 10, x + 26, y + 7, White);

        weather_icon_draw(weather_icon(weather.days[day].code, 1), x + 30, y);
    }
}

MAYBE_UNUSED static void draw_conditions_row(void)
{
    char buf[12];

    draw_icon_pair(0, 47); // 16-px icons fill rows 47-62

    snprintf(buf, sizeof(buf), "%d%s", weather.wind_speed, SPEED_UNIT);
    draw_text_right(128, 53, buf, Font_7x10);
}

#if CLOCK_LAYOUT == 1
// Layout 1: time (medium font) plus day and date in the yellow header;
// current temperature and today's forecast in the blue area.
//
//   11:12    Fri Nov 6     <- yellow
//   ------------------
//   50F       Hi 54F       <- blue
//             Lo 48F
//             Rain 73%
//   Drizzle      5mph
static void draw_clock_screen(void)
{
    LOCAL_TIME lt;
    char buf[24];

    get_local_time(&lt);
    ssd1306_Fill(Black);

    // The 11x18 font's digits only use rows 1-14, so they fit the yellow band.
    format_time(&lt, buf, sizeof(buf), 0);
    draw_text(0, 0, buf, Font_11x18);
    int used = (int)strlen(buf) * 11;
#if CLOCK_12_HOUR
    draw_text(used + 2, 4, lt.hour < 12 ? "AM" : "PM", Font_7x10);
    used += 2 + 14;
#endif

    snprintf(buf, sizeof(buf), "%s %s %d", DAY_NAMES[lt.weekday], MONTH_NAMES[lt.month - 1], lt.day);
    if (used + 6 + (int)strlen(buf) * 7 > 128)
    {
        snprintf(buf, sizeof(buf), "%s %d", DAY_NAMES[lt.weekday], lt.day); // short form if needed
    }
    draw_text_right(128, 4, buf, Font_7x10);

    if (!weather_valid)
    {
        draw_text(0, 30, "Getting weather...", Font_7x10);
        ssd1306_UpdateScreen();
        return;
    }

    snprintf(buf, sizeof(buf), "%d%s", weather.temperature, TEMP_UNIT);
    draw_text(0, BLUE_BAND_TOP + 2, buf, Font_16x26);

    int today = first_upcoming_day();
    if (today < weather.day_count)
    {
        const WEATHER_DAY* d = &weather.days[today];
        snprintf(buf, sizeof(buf), "Hi %d%s", d->high, TEMP_UNIT);
        draw_text(64, BLUE_BAND_TOP, buf, Font_7x10);
        snprintf(buf, sizeof(buf), "Lo %d%s", d->low, TEMP_UNIT);
        draw_text(64, BLUE_BAND_TOP + 11, buf, Font_7x10);
        if (d->rain_chance != WEATHER_UNKNOWN)
        {
            snprintf(buf, sizeof(buf), "Rain %d%%", d->rain_chance);
            draw_text(64, BLUE_BAND_TOP + 22, buf, Font_7x10);
        }
    }

    draw_conditions_row();
    ssd1306_UpdateScreen();
}
#elif CLOCK_LAYOUT == 3
// Layout 3: day, date and temperature in the yellow header; big clock with
// today's high/low/rain beside it in the blue area.
//
//   Fri Nov 6       50F    <- yellow
//   ------------------
//   11:12      ^54F        <- blue
//              v48F
//              o73%
//   Drizzle      5mph
static void draw_clock_screen(void)
{
    LOCAL_TIME lt;
    char buf[24];

    get_local_time(&lt);
    ssd1306_Fill(Black);

#if CLOCK_12_HOUR
    snprintf(buf, sizeof(buf), "%s %s %d %s",
        DAY_NAMES[lt.weekday], MONTH_NAMES[lt.month - 1], lt.day, lt.hour < 12 ? "AM" : "PM");
#else
    snprintf(buf, sizeof(buf), "%s %s %d", DAY_NAMES[lt.weekday], MONTH_NAMES[lt.month - 1], lt.day);
#endif
    draw_text(0, 3, buf, Font_7x10);
    if (weather_valid)
    {
        snprintf(buf, sizeof(buf), "%d%s", weather.temperature, TEMP_UNIT);
        draw_text_right(128, 3, buf, Font_7x10);
    }

    format_time(&lt, buf, sizeof(buf), 0);
    draw_text(0, BLUE_BAND_TOP + 2, buf, Font_16x26);

    if (weather_valid)
    {
        draw_today_column(86, BLUE_BAND_TOP + 1);
        draw_conditions_row();
    }
    else
    {
        draw_text(0, 53, "Getting weather...", Font_7x10);
    }

    ssd1306_UpdateScreen();
}
#else
// Layout 2: day, date and temperature in the yellow header; a big clock and
// current conditions in the blue area.
//
//   Fri Nov 6       50F    <- yellow
//   ------------------
//   11:12       AM         <- blue
//               89%
//   Drizzle      5mph
static void draw_clock_screen(void)
{
    LOCAL_TIME lt;
    char buf[24];

    get_local_time(&lt);
    ssd1306_Fill(Black);

    snprintf(buf, sizeof(buf), "%s %s %d", DAY_NAMES[lt.weekday], MONTH_NAMES[lt.month - 1], lt.day);
    draw_text(0, 3, buf, Font_7x10);
    if (weather_valid)
    {
        snprintf(buf, sizeof(buf), "%d%s", weather.temperature, TEMP_UNIT);
        draw_text_right(128, 3, buf, Font_7x10);
    }

    format_time(&lt, buf, sizeof(buf), 0);
    draw_text(0, BLUE_BAND_TOP + 2, buf, Font_16x26);
#if CLOCK_12_HOUR
    draw_text(86, BLUE_BAND_TOP + 4, lt.hour < 12 ? "AM" : "PM", Font_7x10);
#endif

    if (weather_valid)
    {
        snprintf(buf, sizeof(buf), "%d%%RH", weather.humidity);
        draw_text(86, BLUE_BAND_TOP + 17, buf, Font_7x10);

        draw_conditions_row();
    }
    else
    {
        draw_text(0, 53, "Getting weather...", Font_7x10);
    }

    ssd1306_UpdateScreen();
}
#endif

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
        draw_text(0, 3, "Forecast", Font_7x10);
        draw_text(0, BLUE_BAND_TOP + 8, "No data yet", Font_11x18);
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
    draw_text(0, 3, buf, Font_7x10);

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
                ssd1306_Line(x - half, 7 + dy, x + half, 7 + dy, White);
            }
        }
        else
        {
            ssd1306_DrawPixel(x, 7, White);
        }
    }
    draw_text(0, BLUE_BAND_TOP, weather_code_text(d->code), Font_11x18);

    snprintf(buf, sizeof(buf), "Hi %d%s  Lo %d%s", d->high, TEMP_UNIT, d->low, TEMP_UNIT);
    draw_text(0, 39, buf, Font_7x10);

    if (d->rain_chance == WEATHER_UNKNOWN)
    {
        snprintf(buf, sizeof(buf), "Rain --  Wind %d", d->wind_max);
    }
    else
    {
        snprintf(buf, sizeof(buf), "Rain %d%% Wind %d", d->rain_chance, d->wind_max);
    }
    draw_text(0, 52, buf, Font_7x10);

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

// 1 if hour is in [start, end). The range may wrap past midnight (e.g. 22 -> 7);
// start == end means never.
static int hour_in_range(int hour, int start, int end)
{
    if (start == end)
    {
        return 0;
    }
    if (start < end)
    {
        return hour >= start && hour < end;
    }
    return hour >= start || hour < end;
}

// Quiet hours (night): LED off, and optionally the screen saver goes dark
MAYBE_UNUSED static int in_quiet_hours(const LOCAL_TIME* lt)
{
    return hour_in_range(lt->hour, QUIET_START_HOUR, QUIET_END_HOUR);
}

// Whether the screen saver should turn the screen fully off right now:
// during quiet hours (if enabled) and during away hours
MAYBE_UNUSED static int screen_saver_dark(const LOCAL_TIME* lt)
{
    return (SCREEN_OFF_DURING_QUIET_HOURS && in_quiet_hours(lt)) ||
           hour_in_range(lt->hour, AWAY_START_HOUR, AWAY_END_HOUR);
}

// ---------------------------------------------------------------------------
// Screen saver: a small clock with the weather icons that slowly drifts and
// bounces around a dimmed screen, so no pixel stays lit in one place for long.
//
//   14:34 47F
//   [cloud] -> [snow]
// ---------------------------------------------------------------------------
#define SAVER_ICON_OFFSET 17 // icon row starts this far below the time
#define SAVER_MAX_Y       (64 - SAVER_ICON_OFFSET - 16) // keep the icons on screen

static int saver_x  = 20;
static int saver_y  = 20;
static int saver_dx = 1;
static int saver_dy = 1;

static void draw_screen_saver(void)
{
    LOCAL_TIME lt;
    char time_buf[12];
    char temp_buf[8] = "";

    get_local_time(&lt);
    format_time(&lt, time_buf, sizeof(time_buf), 0);
    if (weather_valid)
    {
        snprintf(temp_buf, sizeof(temp_buf), "%d%s", weather.temperature, TEMP_UNIT);
    }

    int width = (int)strlen(time_buf) * 11 + (temp_buf[0] ? 4 + (int)strlen(temp_buf) * 7 : 0);

    // Move one pixel, bouncing off the edges
    if (saver_x + saver_dx < 0 || saver_x + saver_dx + width > 128)
    {
        saver_dx = -saver_dx;
    }
    if (saver_y + saver_dy < 0 || saver_y + saver_dy > SAVER_MAX_Y)
    {
        saver_dy = -saver_dy;
    }
    saver_x += saver_dx;
    saver_y += saver_dy;
    if (saver_x + width > 128) // the width can grow, e.g. 9:59 -> 10:00
    {
        saver_x = 128 - width;
    }

    ssd1306_Fill(Black);
    draw_text(saver_x, saver_y, time_buf, Font_11x18);
    if (temp_buf[0])
    {
        draw_text(saver_x + (int)strlen(time_buf) * 11 + 4, saver_y + 4, temp_buf, Font_7x10);
    }
    if (weather_valid)
    {
        draw_icon_pair(saver_x, saver_y + SAVER_ICON_OFFSET);
    }
    ssd1306_UpdateScreen();
}

static void screen_saver_start(void)
{
    LOCAL_TIME lt;
    get_local_time(&lt);

    printf("Screen saver on\r\n");
    ssd1306_SetContrast(SCREENSAVER_CONTRAST);
    if (screen_saver_dark(&lt))
    {
        ssd1306_SetDisplayOn(0);
        return;
    }
    draw_screen_saver();
}

static void screen_saver_stop(void)
{
    printf("Screen saver off\r\n");
    ssd1306_SetContrast(0xFF); // driver default
    ssd1306_SetDisplayOn(1);
}

// Which weather alerts the RGB LED should show right now.
static int current_alerts(void)
{
#if !LED_ALERTS_ENABLED
    return ALERT_NONE;
#else
    LOCAL_TIME lt;

    if (!weather_valid)
    {
        return ALERT_NONE;
    }

    get_local_time(&lt);

    if (in_quiet_hours(&lt))
    {
        return ALERT_NONE;
    }

    int day = coming_up_day();

    int alerts = ALERT_NONE;
    if (day < weather.day_count)
    {
        const WEATHER_DAY* d = &weather.days[day];
        if (d->rain_chance != WEATHER_UNKNOWN && d->rain_chance >= RAIN_ALERT_PERCENT)
        {
            alerts |= ALERT_RAIN;
        }
        if (d->low <= FREEZE_ALERT_TEMP)
        {
            alerts |= ALERT_FREEZE;
        }
    }
    if (weather.temperature <= FREEZE_ALERT_TEMP)
    {
        alerts |= ALERT_FREEZE;
    }
    return alerts;
#endif
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

// The day's general weather for the morning tune: snow counts as cold, then
// rain, then the day's high picks cold / cool / warm / hot.
static WEATHER_MOOD todays_mood(void)
{
    int day = first_upcoming_day();
    if (!weather_valid || day >= weather.day_count)
    {
        return MOOD_COOL; // neutral if there's no forecast
    }

    const WEATHER_DAY* d = &weather.days[day];
    int c                = d->code;
    int snowy            = (c >= 71 && c <= 77) || c == 85 || c == 86;
    int wet              = (d->rain_chance != WEATHER_UNKNOWN && d->rain_chance >= RAIN_ALERT_PERCENT) ||
                (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;

    if (snowy)
    {
        return MOOD_COLD;
    }
    if (wet)
    {
        return MOOD_RAINY;
    }
    if (d->high >= TUNE_HOT_TEMP)
    {
        return MOOD_HOT;
    }
    if (d->high >= TUNE_WARM_TEMP)
    {
        return MOOD_WARM;
    }
    if (d->high >= TUNE_COOL_TEMP)
    {
        return MOOD_COOL;
    }
    return MOOD_COLD;
}

static void play_morning_tune(void)
{
    WEATHER_MOOD mood = todays_mood();
    printf("Morning tune: %s\r\n", weather_mood_name(mood));
    morning_tune_play(mood);
}

// 1 once a day at MORNING_TUNE_HOUR:MORNING_TUNE_MINUTE (local time)
static int morning_tune_due(void)
{
#if SOUND_ENABLED && MORNING_TUNE_ENABLED
    static long last_day = -1;
    LOCAL_TIME lt;

    if (!weather_valid) // need the time zone (and forecast) first
    {
        return 0;
    }
    get_local_time(&lt);
    long today = days_from_civil(lt.year, lt.month, lt.day);

    if (lt.hour != MORNING_TUNE_HOUR || lt.minute != MORNING_TUNE_MINUTE || today == last_day)
    {
        return 0;
    }
    last_day = today;
#if !MORNING_TUNE_WEEKENDS
    if (lt.weekday == 0 || lt.weekday == 6)
    {
        return 0;
    }
#endif
    return 1;
#else
    return 0;
#endif
}

static void app_thread_entry(ULONG parameter)
{
    UINT status;
    SCREEN screen          = SCREEN_CLOCK;
    ULONG forecast_shown   = 0;
    int forecast_page      = 0;
    int last_alerts        = -1;
    ULONG last_activity    = 0;
    ULONG saver_last_step  = 0;
    ULONG next_fetch       = 0;
    int last_drawn_minute  = -1;
    int a_was_pressed      = 0;
    int b_was_pressed      = 0;
    ULONG b_down_since     = 0;
    int b_handled          = 0; // this B press already did something
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

#if SCREENSAVER_MINUTES > 0
    motion_init();
#endif
#if SOUND_ENABLED
    audio_init();
#endif
    last_activity = tx_time_get();

    while (1)
    {
        ULONG now = tx_time_get();

        // --- Buttons (active low, polled with edge detection) ---
        int a_pressed = BUTTON_A_IS_PRESSED;
        int b_pressed = BUTTON_B_IS_PRESSED;
        int a_clicked = a_pressed && !a_was_pressed;
        int b_clicked = b_pressed && !b_was_pressed;
        int b_was_pressed_before = b_was_pressed;
        a_was_pressed = a_pressed;
        b_was_pressed = b_pressed;

        // --- Screen saver: wake on a button, a tap or movement ---
#if SCREENSAVER_MINUTES > 0
        int moved = motion_detected();
        if (a_clicked || b_clicked || moved)
        {
            last_activity = now;
            if (screen == SCREEN_SAVER)
            {
                screen_saver_stop();
                screen    = SCREEN_CLOCK;
                redraw    = 1;
                a_clicked = 0; // a press that wakes the screen does nothing else
                b_clicked = 0;
                b_handled = 1;
            }
        }
        else if (screen == SCREEN_CLOCK && now - last_activity >= SCREENSAVER_MINUTES * 60UL * TX_TIMER_TICKS_PER_SECOND)
        {
            screen          = SCREEN_SAVER;
            saver_last_step = now;
            screen_saver_start();
        }
#endif

        if (a_clicked)
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

        // Button B: short press = refresh weather (on release),
        //           hold 1 second = play the morning tune now
        if (b_clicked)
        {
            b_down_since = now;
        }
        if (b_pressed && !b_handled && now - b_down_since >= TX_TIMER_TICKS_PER_SECOND)
        {
            b_handled = 1;
#if SOUND_ENABLED
            play_morning_tune();
#endif
        }
        if (!b_pressed && b_was_pressed_before)
        {
            if (!b_handled)
            {
                draw_status("Updating", "weather...");
                next_fetch = now; // fetch right away
                redraw     = 1;
            }
            b_handled = 0;
        }

        // --- Morning tune ---
        if (morning_tune_due())
        {
            play_morning_tune();
#if SCREENSAVER_MINUTES > 0
            last_activity = now; // show the clock while it plays
            if (screen == SCREEN_SAVER)
            {
                screen_saver_stop();
                screen = SCREEN_CLOCK;
                redraw = 1;
            }
#endif
        }
#if SOUND_ENABLED
        audio_update();
#endif

        // --- Weather refresh ---
        if ((LONG)(now - next_fetch) >= 0)
        {
            ULONG delay_seconds = refresh_weather() ? WEATHER_REFRESH_MINUTES * 60 : RETRY_SECONDS;
            next_fetch          = tx_time_get() + delay_seconds * TX_TIMER_TICKS_PER_SECOND;
            redraw              = screen != SCREEN_SAVER;

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
        if (screen == SCREEN_SAVER)
        {
            LOCAL_TIME lt;
            get_local_time(&lt);
            // Turn the panel off or back on as quiet/away hours begin and end
            int dark = screen_saver_dark(&lt);
            if (dark == ssd1306_GetDisplayOn())
            {
                ssd1306_SetDisplayOn(!dark);
            }
            if (!dark && now - saver_last_step >= SCREENSAVER_STEP_SECONDS * TX_TIMER_TICKS_PER_SECOND)
            {
                draw_screen_saver();
                saver_last_step = now;
            }
        }
        else if (screen == SCREEN_CLOCK)
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

        // --- RGB LED alerts ---
        int alerts = current_alerts();
        if (alerts != last_alerts)
        {
            printf("LED alerts: %s%s%s\r\n",
                alerts == ALERT_NONE ? "none" : "",
                (alerts & ALERT_RAIN) ? "rain " : "",
                (alerts & ALERT_FREEZE) ? "freezing" : "");
            last_alerts = alerts;
        }
        led_alerts_update(alerts, tx_time_get());

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
