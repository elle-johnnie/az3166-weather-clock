/*
 * Motion wake-up using the LSM6DSL accelerometer's built-in tap and
 * movement detection.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _MOTION_H
#define _MOTION_H

// Set up tap and movement detection. Returns 0 on success.
int motion_init(void);

// Returns 1 if a tap or movement happened since the last call.
int motion_detected(void);

#endif // _MOTION_H
