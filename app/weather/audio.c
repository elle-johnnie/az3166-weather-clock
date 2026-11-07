/*
 * Simple tone player for the AZ3166 headphone jack.
 *
 * Hardware setup (pins, clocks and the NAU88C10 register sequence) follows
 * Microsoft's original IoT DevKit SDK (MIT licensed):
 * https://github.com/microsoft/devkit-sdk/tree/master/AZ3166/src/libraries/AudioV2
 *
 *   I2S2: PB12 = WS, PB13 = CK, PB15 = SD, PC6 = MCK (all AF5)
 *   DMA1 Stream 4, channel 0 = SPI2/I2S2 TX
 *   NAU88C10 on the shared I2C bus at 0x1A (0x34 write address)
 *
 * Audio streams through a small circular DMA buffer; each half is refilled
 * with freshly synthesized samples from the DMA interrupt.
 *
 * SPDX-License-Identifier: MIT
 */

#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "tx_api.h"

#include "cloud_config.h"

extern I2C_HandleTypeDef I2cHandle;

#define SAMPLE_RATE    8000
#define CODEC_ADDR     0x34
#define HALF_FRAMES    128               // stereo frames per half buffer (16 ms)
#define ATTACK_SAMPLES (SAMPLE_RATE / 100) // 10 ms fade in
#define RELEASE_SAMPLES (SAMPLE_RATE * 15 / 1000) // 15 ms fade out
#define PEAK_LEVEL     24000             // note peak, out of 65536 (about -9 dB)
#define DECAY_Q16      65520             // per-sample decay; halves in ~350 ms

static I2S_HandleTypeDef i2s;
static DMA_HandleTypeDef dma_tx;
static int16_t dma_buffer[2 * HALF_FRAMES * 2]; // two halves, L+R per frame
static int ready;

// Sequencer state (used from the DMA interrupt)
static const AUDIO_NOTE* volatile seq_notes;
static volatile int seq_count;
static volatile int seq_index;
static volatile int seq_done;          // all notes finished
static volatile int silent_halves;     // halves filled with silence since then
static uint32_t phase, phase_inc;
static int32_t note_samples, note_pos, env;

static const int16_t SINE[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

// ---------------------------------------------------------------------------
// Codec (NAU88C10) over I2C: 7-bit register, 9-bit value
// ---------------------------------------------------------------------------
static int codec_write(uint8_t reg, uint16_t value)
{
    uint8_t low = (uint8_t)value;
    uint16_t addr = (uint16_t)((reg << 1) | ((value >> 8) & 1));
    return HAL_I2C_Mem_Write(&I2cHandle, CODEC_ADDR, addr, I2C_MEMADD_SIZE_8BIT, &low, 1, 100) == HAL_OK ? 0 : -1;
}

static int codec_init(void)
{
    // Register sequence from the original SDK (nau88c10_Init), headphone output
    static const uint16_t REGS[][2] = {
        {0x00, 0x000}, // reset
        {0x01, 0x15D}, {0x02, 0x015}, {0x03, 0x0ED}, // power management
        {0x04, 0x010}, {0x05, 0x000}, {0x06, 0x00C}, {0x07, 0x00A}, // I2S 16-bit, 8 kHz
        {0x08, 0x000}, {0x09, 0x000}, {0x0A, 0x008}, {0x0B, 0x1FF},
        {0x0C, 0x000}, {0x0D, 0x000}, {0x0E, 0x108}, {0x0F, 0x1FF},
        {0x12, 0x12C}, {0x13, 0x02C}, {0x14, 0x02C}, {0x15, 0x02C}, {0x16, 0x02C}, // EQ
        {0x18, 0x032}, {0x19, 0x000}, // DAC limiter
        {0x1B, 0x000}, {0x1C, 0x000}, {0x1D, 0x000}, {0x1E, 0x000}, // notch filter
        {0x20, 0x038}, {0x21, 0x00B}, {0x22, 0x032}, {0x23, 0x000}, // ALC
        {0x24, 0x008}, {0x25, 0x00C}, {0x26, 0x093}, {0x27, 0x0E9}, // PLL (unused)
        {0x28, 0x000},
        {0x2C, 0x003}, {0x2D, 0x010}, {0x2E, 0x000}, {0x2F, 0x100}, // input mixer
        {0x30, 0x000}, {0x31, 0x002}, {0x32, 0x001}, {0x33, 0x000}, // output mixer
        {0x34, 0x040}, {0x35, 0x040}, {0x36, 0x0BF}, {0x37, 0x040},
        {0x38, 0x001}, // DAC to mono out (headphone)
        {0x3C, 0x004},
    };

    for (unsigned i = 0; i < sizeof(REGS) / sizeof(REGS[0]); i++)
    {
        if (codec_write((uint8_t)REGS[i][0], REGS[i][1]))
        {
            return -1;
        }
    }

    // Volume: DAC digital volume, 255 = 0 dB, 0.5 dB steps (SDK: volume + 155)
    int vol = SOUND_VOLUME < 0 ? 0 : (SOUND_VOLUME > 100 ? 100 : SOUND_VOLUME);
    return codec_write(0x0B, (uint16_t)(0x100 | (vol == 0 ? 0 : vol + 155)));
}

// ---------------------------------------------------------------------------
// Synthesis
// ---------------------------------------------------------------------------
static void start_note(int index)
{
    const AUDIO_NOTE* n = &seq_notes[index];
    note_samples = (int32_t)n->ms * SAMPLE_RATE / 1000;
    note_pos     = 0;
    env          = 0;
    phase        = 0;
    phase_inc    = (uint32_t)(((uint64_t)n->freq_hz << 32) / SAMPLE_RATE);
}

static int16_t next_sample(void)
{
    while (!seq_done && note_pos >= note_samples)
    {
        if (++seq_index >= seq_count)
        {
            seq_done = 1;
            break;
        }
        start_note(seq_index);
    }
    if (seq_done)
    {
        return 0;
    }

    int32_t out = 0;
    if (seq_notes[seq_index].freq_hz != 0)
    {
        // Envelope: quick fade in, then a slow bell-like decay
        if (note_pos < ATTACK_SAMPLES)
        {
            env = PEAK_LEVEL * (note_pos + 1) / ATTACK_SAMPLES;
        }
        else
        {
            env = (int32_t)(((int64_t)env * DECAY_Q16) >> 16);
        }

        // Fundamental plus a little second harmonic for a softer, chime-like tone
        int32_t s = (3 * SINE[phase >> 24] + SINE[(phase << 1) >> 24]) >> 2;
        out       = (s * env) >> 16;

        int32_t remaining = note_samples - note_pos;
        if (remaining < RELEASE_SAMPLES)
        {
            out = out * remaining / RELEASE_SAMPLES; // fade out, no click
        }
        phase += phase_inc;
    }
    note_pos++;
    return (int16_t)out;
}

static void fill_half(int16_t* dst)
{
    for (int i = 0; i < HALF_FRAMES; i++)
    {
        int16_t s  = next_sample();
        dst[2 * i]     = s; // left
        dst[2 * i + 1] = s; // right (the codec is mono)
    }
    if (seq_done)
    {
        silent_halves++;
    }
}

void HAL_I2S_TxHalfCpltCallback(I2S_HandleTypeDef* hi2s)
{
    fill_half(&dma_buffer[0]);
}

void HAL_I2S_TxCpltCallback(I2S_HandleTypeDef* hi2s)
{
    fill_half(&dma_buffer[HALF_FRAMES * 2]);
}

void DMA1_Stream4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&dma_tx);
}

// ---------------------------------------------------------------------------
// Hardware setup
// ---------------------------------------------------------------------------
static void start_stream(void)
{
    HAL_I2S_Transmit_DMA(&i2s, (uint16_t*)dma_buffer, sizeof(dma_buffer) / sizeof(dma_buffer[0]));
}

int audio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef clk;

    if (HAL_I2C_IsDeviceReady(&I2cHandle, CODEC_ADDR, 2, 100) != HAL_OK)
    {
        printf("ERROR: Audio chip not found\r\n");
        return -1;
    }

    // I2S clock: HSE 26 MHz / 26 * 344 / 7 = 49.14 MHz (8/16/48 kHz family)
    HAL_RCCEx_GetPeriphCLKConfig(&clk);
    clk.PeriphClockSelection  = RCC_PERIPHCLK_I2S_APB1 | RCC_PERIPHCLK_PLLI2S;
    clk.I2sApb1ClockSelection = RCC_I2SAPB1CLKSOURCE_PLLI2S;
    clk.PLLI2SSelection       = RCC_PLLI2SCLKSOURCE_PLLSRC;
    clk.PLLI2S.PLLI2SM        = 26;
    clk.PLLI2S.PLLI2SN        = 344;
    clk.PLLI2S.PLLI2SQ        = 4;
    clk.PLLI2S.PLLI2SR        = 7;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        printf("ERROR: Audio clock setup failed\r\n");
        return -1;
    }

    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    gpio.Pin       = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_6;
    HAL_GPIO_Init(GPIOC, &gpio);

    dma_tx.Instance                 = DMA1_Stream4;
    dma_tx.Init.Channel             = DMA_CHANNEL_0;
    dma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    dma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    dma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    dma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    dma_tx.Init.Mode                = DMA_CIRCULAR;
    dma_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    dma_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    HAL_DMA_Init(&dma_tx);
    __HAL_LINKDMA(&i2s, hdmatx, dma_tx);

    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);

    i2s.Instance            = SPI2;
    i2s.Init.Mode           = I2S_MODE_MASTER_TX;
    i2s.Init.Standard       = I2S_STANDARD_PHILIPS;
    i2s.Init.DataFormat     = I2S_DATAFORMAT_16B;
    i2s.Init.MCLKOutput     = I2S_MCLKOUTPUT_ENABLE;
    i2s.Init.AudioFreq      = I2S_AUDIOFREQ_8K;
    i2s.Init.CPOL           = I2S_CPOL_LOW;
    i2s.Init.ClockSource    = I2S_CLOCK_PLL;
    i2s.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
    if (HAL_I2S_Init(&i2s) != HAL_OK)
    {
        printf("ERROR: I2S setup failed\r\n");
        return -1;
    }

    // The codec needs the I2S master clock running while it is configured,
    // so stream silence during setup (as the original SDK does).
    memset(dma_buffer, 0, sizeof(dma_buffer));
    seq_done = 1;
    start_stream();
    tx_thread_sleep(2);
    int err = codec_init();
    tx_thread_sleep(2);
    HAL_I2S_DMAStop(&i2s);

    if (err)
    {
        printf("ERROR: Audio chip setup failed\r\n");
        return -1;
    }

    ready = 1;
    printf("SUCCESS: Audio ready\r\n");
    return 0;
}

int audio_play(const AUDIO_NOTE* notes, int count)
{
    if (!ready || count <= 0 || audio_is_playing())
    {
        return -1;
    }

    seq_notes     = notes;
    seq_count     = count;
    seq_index     = 0;
    seq_done      = 0;
    silent_halves = 0;
    start_note(0);

    // Pre-fill both halves, then let the DMA interrupt keep them topped up
    fill_half(&dma_buffer[0]);
    fill_half(&dma_buffer[HALF_FRAMES * 2]);
    start_stream();
    return 0;
}

int audio_is_playing(void)
{
    return ready && HAL_I2S_GetState(&i2s) != HAL_I2S_STATE_READY;
}

void audio_update(void)
{
    // Stop once the tune has ended and both halves have played out silence
    if (audio_is_playing() && seq_done && silent_halves >= 3)
    {
        HAL_I2S_DMAStop(&i2s);
    }
}
