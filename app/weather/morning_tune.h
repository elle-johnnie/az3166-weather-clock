/*
 * Morning tune: a short rising "good morning" phrase followed by a motif
 * that hints at the day's weather.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _MORNING_TUNE_H
#define _MORNING_TUNE_H

typedef enum
{
    MOOD_COLD,
    MOOD_COOL,
    MOOD_WARM,
    MOOD_HOT,
    MOOD_RAINY
} WEATHER_MOOD;

// Starts playing the tune for a mood (in the background). Returns 0 if started.
int morning_tune_play(WEATHER_MOOD mood);

// "cold", "cool", "warm", "hot" or "rainy"
const char* weather_mood_name(WEATHER_MOOD mood);

#endif // _MORNING_TUNE_H
