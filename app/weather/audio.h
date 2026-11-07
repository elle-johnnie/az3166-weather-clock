/*
 * Simple tone player for the AZ3166 headphone jack.
 *
 * The board's NAU88C10 audio chip is set up over I2C and fed 8 kHz audio over
 * I2S. Notes are synthesized on the fly (a soft bell-like tone), so tunes
 * take almost no memory.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _AUDIO_H
#define _AUDIO_H

#include <stdint.h>

typedef struct
{
    uint16_t freq_hz; // 0 = rest (silence)
    uint16_t ms;      // length
} AUDIO_NOTE;

// Set up the audio chip and I2S. Returns 0 on success.
int audio_init(void);

// Start playing a tune in the background. The notes must stay valid while it
// plays (use static/const arrays). Returns 0 if it started.
int audio_play(const AUDIO_NOTE* notes, int count);

// 1 while a tune is playing.
int audio_is_playing(void);

// Call regularly (every loop); shuts the audio output down after a tune ends.
void audio_update(void);

#endif // _AUDIO_H
