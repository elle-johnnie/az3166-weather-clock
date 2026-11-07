/*
 * Motion wake-up using the LSM6DSL accelerometer's built-in tap and
 * movement ("wake-up") detection. The chip watches for events on its own and
 * latches them, so a quick tap is caught even though we only check every
 * 50 ms. Register values follow ST application note AN5040.
 *
 * SPDX-License-Identifier: MIT
 */

#include "motion.h"

#include <stdio.h>

#include "lsm6dsl_reg.h"
#include "stm32f4xx_hal.h"

#include "cloud_config.h"

extern I2C_HandleTypeDef I2cHandle;

static int32_t i2c_write(void* handle, uint8_t reg, uint8_t* buf, uint16_t len)
{
    return HAL_I2C_Mem_Write(handle, LSM6DSL_I2C_ADD_L, reg, I2C_MEMADD_SIZE_8BIT, buf, len, 100) == HAL_OK ? 0 : -1;
}

static int32_t i2c_read(void* handle, uint8_t reg, uint8_t* buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(handle, LSM6DSL_I2C_ADD_L, reg, I2C_MEMADD_SIZE_8BIT, buf, len, 100) == HAL_OK ? 0 : -1;
}

static stmdev_ctx_t ctx = {i2c_write, i2c_read, &I2cHandle};
static int ready        = 0;

static int write_reg(uint8_t reg, uint8_t value)
{
    return lsm6dsl_write_reg(&ctx, reg, &value, 1);
}

int motion_init(void)
{
    uint8_t id = 0;

    if (lsm6dsl_device_id_get(&ctx, &id) != 0 || id != LSM6DSL_ID)
    {
        printf("ERROR: Motion sensor not found (id 0x%02x)\r\n", id);
        return -1;
    }

    int err = 0;
    err |= write_reg(LSM6DSL_CTRL1_XL, 0x60);    // accelerometer 416 Hz, +/-2 g (tap detection needs a high rate)
    err |= write_reg(LSM6DSL_TAP_CFG, 0x8F);     // enable embedded functions, tap on X/Y/Z, latch events until read
    err |= write_reg(LSM6DSL_TAP_THS_6D, MOTION_TAP_THRESHOLD & 0x1F); // tap threshold, 1 = 62.5 mg
    err |= write_reg(LSM6DSL_INT_DUR2, 0x06);    // tap shock/quiet windows
    err |= write_reg(LSM6DSL_WAKE_UP_THS, MOTION_MOVE_THRESHOLD & 0x3F); // single tap only; movement threshold, 1 = 31 mg
    err |= write_reg(LSM6DSL_WAKE_UP_DUR, 0x00); // react to the first sample over the threshold

    if (err)
    {
        printf("ERROR: Motion sensor setup failed\r\n");
        return -1;
    }

    ready = 1;
    printf("SUCCESS: Motion wake-up ready\r\n");
    return 0;
}

int motion_detected(void)
{
    uint8_t wake_src = 0;
    uint8_t tap_src  = 0;

    if (!ready)
    {
        return 0;
    }

    // Reading these registers also clears the latched events
    lsm6dsl_read_reg(&ctx, LSM6DSL_WAKE_UP_SRC, &wake_src, 1);
    lsm6dsl_read_reg(&ctx, LSM6DSL_TAP_SRC, &tap_src, 1);

    return (wake_src & 0x08) != 0  // WU_IA: movement over threshold
        || (tap_src & 0x40) != 0;  // TAP_IA: tap
}
