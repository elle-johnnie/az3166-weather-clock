/*
 * 16x16 weather icons for the OLED.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _WEATHER_ICONS_H
#define _WEATHER_ICONS_H

#include <stdint.h>

// One row per uint16_t, top row first; bit 15 is the leftmost pixel.
typedef const uint16_t WEATHER_ICON[16];

// Icon for a WMO weather code. is_day picks a sun or moon for clear and
// partly cloudy skies.
const uint16_t* weather_icon(int code, int is_day);

// Draws a 16x16 icon with its top-left corner at (x, y).
void weather_icon_draw(const uint16_t* icon, int x, int y);

#endif // _WEATHER_ICONS_H
