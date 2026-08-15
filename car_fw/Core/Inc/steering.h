#ifndef STEERING_H
#define STEERING_H

#include <stdint.h>

void Steering_Init(void);
void Steering_Enable(void);
void Steering_Disable(void);
void Steering_Update(void);

// Steering position scale:
//   0   = left
//   50  = center
//   100 = right
void Steering_SetPosition(uint8_t position);

// Current commanded target, 0..100 scale (what we're steering towards).
uint8_t Steering_GetTargetPosition(void);

// Current commanded position, 0..100 scale.
// This is where STM32 is currently commanding the servo after slew-rate limiting.
// It is not external measured physical feedback from the servo.
uint8_t Steering_GetCurrentPosition(void);

// Calibration only: command a raw pulse width, bypassing the 0..100 scale.
// Clamped to the servo's absolute range and reached through the normal slew.
void Steering_SetPulseRawUs(uint16_t pulse_us);

// Raw current pulse width in microseconds (for diagnostics/logging).
uint16_t Steering_GetCurrentPulseUs(void);

// Non-zero while current position has not yet reached the target
// (servo is still slewing towards it).
uint8_t Steering_IsMoving(void);

#endif