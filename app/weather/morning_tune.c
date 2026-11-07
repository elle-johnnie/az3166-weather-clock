/*
 * Morning tune: a short rising "good morning" phrase followed by a motif
 * that hints at the day's weather.
 *
 * Each tune is a list of {frequency in Hz, length in ms}; 0 Hz is a rest.
 * Feel free to write your own. Audio runs at 8 kHz, so keep notes below
 * about 3000 Hz.
 *
 * SPDX-License-Identifier: MIT
 */

#include "morning_tune.h"

#include "audio.h"

// Note frequencies (Hz)
#define E4 330
#define G4 392
#define A4 440
#define B4 494
#define C5 523
#define D5 587
#define E5 659
#define F5 698
#define G5 784
#define A5 880
#define B5 988
#define C6 1047
#define D6 1175
#define E6 1319
#define G6 1568
#define A6 1760
#define C7 2093
#define REST 0

#define INTRO {C5, 150}, {E5, 150}, {G5, 150}, {REST, 120}

// Cold: drops into a low minor chord, slow - "brr, bundle up"
static const AUDIO_NOTE TUNE_COLD[] = {
    INTRO,
    {A4, 260}, {C5, 260}, {E4, 900},
};

// Cool: an open, mellow suspended phrase - "grab a jacket"
static const AUDIO_NOTE TUNE_COOL[] = {
    INTRO,
    {D5, 220}, {G5, 220}, {A5, 220}, {G5, 700},
};

// Warm: a bright major resolution - "nice day"
static const AUDIO_NOTE TUNE_WARM[] = {
    INTRO,
    {C6, 180}, {A5, 180}, {B5, 180}, {C6, 800},
};

// Hot: quick sparkly climb to the top - "sunscreen!"
static const AUDIO_NOTE TUNE_HOT[] = {
    INTRO,
    {C6, 100}, {E6, 100}, {G6, 100}, {C7, 120}, {REST, 60}, {C7, 700},
};

// Rainy: raindrops plinking down from high to low - "umbrella"
static const AUDIO_NOTE TUNE_RAINY[] = {
    INTRO,
    {G6, 90}, {REST, 70}, {E6, 90}, {REST, 70}, {C6, 90}, {REST, 110},
    {A5, 90}, {REST, 70}, {G5, 700},
};

#define PLAY(tune) audio_play(tune, sizeof(tune) / sizeof(tune[0]))

int morning_tune_play(WEATHER_MOOD mood)
{
    switch (mood)
    {
        case MOOD_COLD:  return PLAY(TUNE_COLD);
        case MOOD_COOL:  return PLAY(TUNE_COOL);
        case MOOD_WARM:  return PLAY(TUNE_WARM);
        case MOOD_HOT:   return PLAY(TUNE_HOT);
        case MOOD_RAINY: return PLAY(TUNE_RAINY);
    }
    return -1;
}

const char* weather_mood_name(WEATHER_MOOD mood)
{
    switch (mood)
    {
        case MOOD_COLD:  return "cold";
        case MOOD_COOL:  return "cool";
        case MOOD_WARM:  return "warm";
        case MOOD_HOT:   return "hot";
        case MOOD_RAINY: return "rainy";
    }
    return "?";
}
