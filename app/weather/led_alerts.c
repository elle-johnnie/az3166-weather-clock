/*
 * Weather alerts on the RGB LED: a slow, gentle "breathing" pulse.
 *
 * The LED's red, green and blue channels are PWM outputs from 0 (off) to
 * 2047 (full), set up in board_init.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "led_alerts.h"

#include "board_init.h"
#include "cloud_config.h"

#define PWM_MAX 2047

typedef struct
{
    int r, g, b; // relative mix, 0-255
} COLOR;

static const COLOR RAIN_COLOR   = {0, 70, 255};    // blue
static const COLOR FREEZE_COLOR = {150, 220, 255}; // icy white

static void set_led(const COLOR* c, ULONG level) // level 0-1000
{
    ULONG scale = (ULONG)LED_MAX_BRIGHTNESS * level / 1000;
    RGB_LED_SET_R(c->r * scale / 255);
    RGB_LED_SET_G(c->g * scale / 255);
    RGB_LED_SET_B(c->b * scale / 255);
}

void led_alerts_update(int alerts, ULONG ticks)
{
    static const COLOR OFF = {0, 0, 0};

    if (alerts == ALERT_NONE)
    {
        set_led(&OFF, 0);
        return;
    }

    const ULONG period = LED_BREATH_SECONDS * TX_TIMER_TICKS_PER_SECOND;
    ULONG cycle        = ticks / period;
    ULONG phase        = ticks % period; // 0 .. period-1

    // Triangle wave 0 -> 1000 -> 0, then squared so the fade looks even to
    // the eye (LEDs look much brighter than their duty cycle at low levels).
    ULONG tri   = phase < period / 2 ? phase * 2000 / period : (period - phase) * 2000 / period;
    ULONG level = tri * tri / 1000;

    // Both alerts: alternate colors each breath. The LED is fully dark at the
    // end of a cycle, so the color switch is never visible as a jump.
    const COLOR* color;
    if ((alerts & ALERT_RAIN) && (alerts & ALERT_FREEZE))
    {
        color = (cycle % 2) ? &FREEZE_COLOR : &RAIN_COLOR;
    }
    else if (alerts & ALERT_RAIN)
    {
        color = &RAIN_COLOR;
    }
    else
    {
        color = &FREEZE_COLOR;
    }

    set_led(color, level);
}
