/*
 * Weather alerts on the RGB LED: a slow, gentle "breathing" pulse.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _LED_ALERTS_H
#define _LED_ALERTS_H

#include "tx_api.h"

typedef enum
{
    ALERT_NONE   = 0,
    ALERT_RAIN   = 1 << 0,
    ALERT_FREEZE = 1 << 1
} ALERT_FLAGS;

// Call often (every loop iteration) with the alerts to show and the current
// ThreadX tick count. With more than one alert, colors alternate each breath.
// Pass ALERT_NONE to turn the LED off.
void led_alerts_update(int alerts, ULONG ticks);

#endif // _LED_ALERTS_H
