#ifndef MOTOR_DC_H
#define MOTOR_DC_H

#include "main.h"
#include <stdint.h>

// Sign convention, stated once so it cannot drift between files:
//   speed > 0  -> TIM1_CH1 / PA8 / RPWM active
//   speed < 0  -> TIM1_CH2 / PA9 / LPWM active
//   speed == 0 -> both compares zero = both low sides on = BRAKE, not coast.
// Which of these is physically "forward" depends on the motor wiring; the
// UART layer in main.c owns that mapping and currently inverts the protocol
// percent before calling here.
//
// Range is +/-5000 (MOTOR_MAX), which is 59 % real duty against ARR 8500.

void MotorDC_Init(void);

// Sets the TARGET speed. The actual compare value slews towards it in
// MotorDC_Update() at roughly full scale per second, with a forced stop at
// zero on any direction change. No caller can produce a current step.
void MotorDC_SetSpeed(int speed);

// Drops PWM to zero immediately and clears the ramp state. For fault paths.
void MotorDC_Stop(void);

// Compare value actually applied right now (not the target).
int16_t MotorDC_GetSpeed(void);

// Must be called every 2 ms. Currently driven from the TIM4 interrupt in
// main.c, alongside Steering_Update().
void MotorDC_Update(void);

#endif
