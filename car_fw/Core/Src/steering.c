#include "steering.h"
#include "tim.h"

#include <stdint.h>

// -----------------------------------------------------------------------------
// A35BHLP 3-wire digital servo steering
// -----------------------------------------------------------------------------
// Signal pin: PA1 / TIM2_CH2
// TIM2 setup: 1 us timer tick, 3000 us frame (~333 Hz), CCR = pulse width in us.
//
// Public steering scale used by UART and tests:
//   0   = left
//   50  = center
//   100 = right
//
// These three values are CALIBRATION POINTS for the real steering mechanics.
// They are pulse widths in microseconds, not PWM duty percentages.
//
// Important: the numeric order does NOT matter. For some installations the
// physical right direction may require a smaller pulse than center, or larger
// pulse than center. The interpolation below uses signed math and therefore
// supports any valid order:
//   LEFT_US > CENTER_US > RIGHT_US
//   LEFT_US < CENTER_US < RIGHT_US
//   or asymmetric values on both sides.
// -----------------------------------------------------------------------------

#define STEERING_LEFT_US          2200U
#define STEERING_CENTER_US        1550U
#define STEERING_RIGHT_US         1050U

#define STEERING_ABS_MIN_US        500U
#define STEERING_ABS_MAX_US       2500U

#define STEERING_MIN_POSITION        0U
#define STEERING_CENTER_POSITION    50U
#define STEERING_MAX_POSITION      100U

// TIM4 calls Steering_Update() every 2 ms.
// The servo is stepped only once per STEERING_SLEW_TICK_DIVIDER timer ticks.
// With 10 us every 4 ms, center->left takes about 232 ms and center->right about 188 ms.
#define STEERING_SLEW_US_PER_STEP      8U
#define STEERING_SLEW_TICK_DIVIDER      2U

#if (STEERING_LEFT_US < STEERING_ABS_MIN_US) || (STEERING_LEFT_US > STEERING_ABS_MAX_US)
#error "STEERING_LEFT_US must be inside the absolute servo signal range"
#endif

#if (STEERING_CENTER_US < STEERING_ABS_MIN_US) || (STEERING_CENTER_US > STEERING_ABS_MAX_US)
#error "STEERING_CENTER_US must be inside the absolute servo signal range"
#endif

#if (STEERING_RIGHT_US < STEERING_ABS_MIN_US) || (STEERING_RIGHT_US > STEERING_ABS_MAX_US)
#error "STEERING_RIGHT_US must be inside the absolute servo signal range"
#endif

#if (STEERING_ABS_MAX_US >= 3000U)
#error "STEERING_ABS_MAX_US must fit inside the 3000 us TIM2 servo frame"
#endif

// Steering_PulseToPosition() divides by (LEFT - CENTER) and (RIGHT - CENTER).
// Neither may be zero. On Cortex-M4 a zero divisor silently yields 0 rather
// than faulting, so catch it here instead of shipping a wrong position.
#if (STEERING_LEFT_US == STEERING_CENTER_US)
#error "STEERING_LEFT_US must differ from STEERING_CENTER_US"
#endif

#if (STEERING_RIGHT_US == STEERING_CENTER_US)
#error "STEERING_RIGHT_US must differ from STEERING_CENTER_US"
#endif

// NOT CALIBRATED YET -- the values above (2200 / 1550 / 1050) disagree with
// the comment in main.c's self-test block, which states the measured safe
// limits are 2130 / 1550 / 1080. If main.c is the correct one, positions 0
// and 100 currently command 70 us past the left stop and 30 us past the
// right stop. A servo held against its stop stalls and draws its full stall
// current continuously from the same 2S pack -- a raised baseline rather
// than a spike, so it does not look like a fault on a scope.
//
// Measure it on the bench, then fix the values here AND the comment in
// main.c together, and delete this warning.
#warning "Steering limits are UNVERIFIED: steering.c says 2200/1050 us, main.c comment says 2130/1080 us. Measure before driving to positions 0 or 100."

static volatile uint8_t steering_enabled = 0U;
static volatile uint16_t current_pulse_us = STEERING_CENTER_US;
static volatile uint16_t target_pulse_us  = STEERING_CENTER_US;

static uint16_t Steering_ClampPulse(int32_t pulse_us)
{
    if (pulse_us < (int32_t)STEERING_ABS_MIN_US)
    {
        return STEERING_ABS_MIN_US;
    }

    if (pulse_us > (int32_t)STEERING_ABS_MAX_US)
    {
        return STEERING_ABS_MAX_US;
    }

    return (uint16_t)pulse_us;
}

static uint16_t Steering_InterpolatePulse(uint16_t from_us,
                                          uint16_t to_us,
                                          uint8_t step,
                                          uint8_t total_steps)
{
    if (step > total_steps)
    {
        step = total_steps;
    }

    const int32_t from = (int32_t)from_us;
    const int32_t delta = (int32_t)to_us - (int32_t)from_us;

    // Signed interpolation. This is the critical part: it works correctly even
    // when to_us is numerically smaller than from_us.
    int32_t pulse_us = from + ((delta * (int32_t)step) / (int32_t)total_steps);

    return Steering_ClampPulse(pulse_us);
}

static uint16_t Steering_PositionToPulse(uint8_t position)
{
    if (position > STEERING_MAX_POSITION)
    {
        position = STEERING_MAX_POSITION;
    }

    if (position < STEERING_CENTER_POSITION)
    {
        // 0..50 maps LEFT -> CENTER.
        return Steering_InterpolatePulse(STEERING_LEFT_US,
                                         STEERING_CENTER_US,
                                         position,
                                         STEERING_CENTER_POSITION);
    }

    if (position > STEERING_CENTER_POSITION)
    {
        // 50..100 maps CENTER -> RIGHT.
        return Steering_InterpolatePulse(STEERING_CENTER_US,
                                         STEERING_RIGHT_US,
                                         (uint8_t)(position - STEERING_CENTER_POSITION),
                                         (uint8_t)(STEERING_MAX_POSITION - STEERING_CENTER_POSITION));
    }

    // 50 must be exact center, without interpolation rounding.
    return STEERING_CENTER_US;
}

static void Steering_WritePulse(uint16_t pulse_us)
{
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, pulse_us);
}

// Inverse of Steering_PositionToPulse(): given a pulse width, recover the
// 0..100 position scale. Used so the rest of the firmware (PING replies,
// diagnostics/logging) can ask "where is the servo right now" without
// duplicating the calibration math.
//
// Works regardless of whether LEFT_US/RIGHT_US are numerically bigger or
// smaller than CENTER_US, same as the forward interpolation above.
static uint8_t Steering_PulseToPosition(uint16_t pulse_us)
{
    const int32_t pulse  = (int32_t)pulse_us;
    const int32_t center = (int32_t)STEERING_CENTER_US;
    const int32_t left   = (int32_t)STEERING_LEFT_US;
    const int32_t right  = (int32_t)STEERING_RIGHT_US;
    const int32_t diff   = pulse - center;

    if (diff == 0)
    {
        return STEERING_CENTER_POSITION;
    }

    // Pick the calibration segment (LEFT..CENTER or CENTER..RIGHT) whose
    // direction from center matches the sign of diff.
    int32_t position;

    if ((diff > 0) == (left > center))
    {
        // pulse lies on the LEFT..CENTER segment -> position in 0..50.
        const int32_t span = left - center; // same sign as diff, non-zero
        position = (int32_t)STEERING_CENTER_POSITION -
                   (diff * (int32_t)STEERING_CENTER_POSITION) / span;
    }
    else
    {
        // pulse lies on the CENTER..RIGHT segment -> position in 50..100.
        const int32_t span = right - center; // same sign as diff, non-zero
        position = (int32_t)STEERING_CENTER_POSITION +
                   (diff * (int32_t)STEERING_CENTER_POSITION) / span;
    }

    if (position < (int32_t)STEERING_MIN_POSITION)
    {
        position = (int32_t)STEERING_MIN_POSITION;
    }
    else if (position > (int32_t)STEERING_MAX_POSITION)
    {
        position = (int32_t)STEERING_MAX_POSITION;
    }

    return (uint8_t)position;
}

void Steering_Init(void)
{
    steering_enabled = 0U;
    current_pulse_us = STEERING_CENTER_US;
    target_pulse_us  = STEERING_CENTER_US;

    Steering_WritePulse(STEERING_CENTER_US);

    // Start PA1/TIM2_CH2 PWM here. main.c only configures TIM2.
    if (HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2) != HAL_OK)
    {
        Error_Handler();
    }
}

void Steering_Enable(void)
{
    steering_enabled = 1U;
}

void Steering_Disable(void)
{
    // For the car, disable means safe center/hold, not PWM-off.
    // Stopping PWM would remove servo holding torque and can leave the wheels turned.
    steering_enabled = 0U;
    target_pulse_us  = STEERING_CENTER_US;
    current_pulse_us = STEERING_CENTER_US;
    Steering_WritePulse(STEERING_CENTER_US);
}

void Steering_SetPosition(uint8_t position)
{
    target_pulse_us = Steering_PositionToPulse(position);
}

void Steering_Update(void)
{
    static uint8_t slew_tick_counter = 0U;

    if (!steering_enabled)
    {
        slew_tick_counter = 0U;
        return;
    }

    // Non-blocking version of the user's "delay t_d" idea:
    // TIM4 calls us every 2 ms, but we make one servo step only every
    // STEERING_SLEW_TICK_DIVIDER ticks. Main loop and UART stay free.
    slew_tick_counter++;
    if (slew_tick_counter < STEERING_SLEW_TICK_DIVIDER)
    {
        return;
    }
    slew_tick_counter = 0U;

    uint16_t current = current_pulse_us;
    const uint16_t target = target_pulse_us;

    if (current < target)
    {
        const uint16_t diff = (uint16_t)(target - current);
        current = (diff > STEERING_SLEW_US_PER_STEP) ?
                  (uint16_t)(current + STEERING_SLEW_US_PER_STEP) : target;
    }
    else if (current > target)
    {
        const uint16_t diff = (uint16_t)(current - target);
        current = (diff > STEERING_SLEW_US_PER_STEP) ?
                  (uint16_t)(current - STEERING_SLEW_US_PER_STEP) : target;
    }
    else
    {
        return;
    }

    current_pulse_us = current;
    Steering_WritePulse(current);
}

uint8_t Steering_GetTargetPosition(void)
{
    return Steering_PulseToPosition(target_pulse_us);
}

uint8_t Steering_GetCurrentPosition(void)
{
    return Steering_PulseToPosition(current_pulse_us);
}

// Calibration only: sets the slew TARGET directly in microseconds, bypassing
// the 0..100 position scale. Used to find the mechanical stops after the
// steering geometry changes -- the 0..100 scale is meaningless until the
// limits it interpolates between are known. Still clamped to the servo's
// absolute pulse range, and still reached through the normal slew, so it
// cannot slam the linkage.
void Steering_SetPulseRawUs(uint16_t pulse_us)
{
    target_pulse_us = Steering_ClampPulse((int32_t)pulse_us);
}

uint16_t Steering_GetCurrentPulseUs(void)
{
    return current_pulse_us;
}

uint8_t Steering_IsMoving(void)
{
    return (current_pulse_us != target_pulse_us) ? 1U : 0U;
}