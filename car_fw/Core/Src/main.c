/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bms_app.h"     // MAX17320 pack monitor on I2C3, console on the VCP
#include "bms_io.h"      // console printing, for the steering calibration keys

#include "steering.h"
#include <string.h>
#include <stdlib.h>
#include "motor_dc.h"
#include "i2c_bus.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define UART_PACKET_START     0xA5
#define UART_PACKET_SIZE      5

#define UART_CMD_CONTROL      0x10
#define UART_CMD_ESTOP        0x11
#define UART_CMD_PING         0x12
#define UART_CMD_STOP         0x13

#define DC_PERCENT_MAX        100
#define DC_PWM_MIN_START      1280
#define DC_PWM_MAX            5000

// Physical direction inversion, in one named place.
//
// The protocol says motor_percent > 0 means DRIVE FORWARD. motor_dc.h says
// speed > 0 energises RPWM (PA8/TIM1_CH1). On this vehicle those two do not
// agree: with the bridge wired as it is, RPWM drives the wheels BACKWARDS.
// So the protocol value is negated exactly once, here, on its way into
// MotorPercent_ToPwm(), and nothing downstream needs to know.
//
// If the motor leads are ever swapped, flip this to +1 and change nothing
// else. It was previously an unexplained unary minus buried in the CONTROL
// handler, which is a bad place for the one line that decides whether "full
// throttle forward" drives the car into the audience.
#define MOTOR_FORWARD_SIGN    (-1)

// Speed ceiling, as a percentage of the span ABOVE the breakaway threshold.
// DC_PWM_MIN_START is never scaled -- below it the motor does not turn at
// all, so shrinking that would produce a dead zone rather than a slow car.
//
// The vehicle weighs about 4 kg (verify -- it has never been put on a
// scale, and the figure quoted here used to be 10 kg, which is either a
// stale number from an earlier chassis or a guess; weigh it before using
// this to reason about anything), and breaking away from standstill needs
// noticeably more torque than keeping it rolling. So the default starts
// HIGH ENOUGH TO DEFINITELY MOVE and is meant to be dialled DOWN on the
// floor -- starting low and creeping up leaves you unable to tell "the
// limit is too low" from "something is mechanically stuck".
//
// What made it violent before was not this ceiling, it was that the ceiling
// was applied instantly. With the ramp, the same top speed is reached over
// a couple of seconds.
//
// Adjustable live from the console with + and - (5 % steps), reported by m.
// Once a good value is found on the floor, put it here so it survives a reset.
#define DC_SPEED_LIMIT_PCT_DEFAULT   40

// Set to 1 only for bench testing without a UART master connected.
// Keep it 0 for normal operation: the demo block below drives the servo
// through Steering_SetPosition() itself and would fight real UART commands,
// plus its HAL_Delay() calls stall UART packet processing for seconds.
//
// While this is 1 the main loop blocks for 4 s per iteration, so the drive
// watchdog, the partial-frame recovery and the pack monitor all stop being
// serviced for that long. The motor is started and hard-braked twice per
// iteration for as long as the board is powered, with nobody watching.
// Never leave this enabled on a board connected to the pack.
#define STEERING_DEMO_SELFTEST   0

#if STEERING_DEMO_SELFTEST
#warning "STEERING_DEMO_SELFTEST is ENABLED: the motor will run on its own, unattended. Bench only."
#endif

// Drive watchdog: how long the motor may keep running without a fresh valid
// CONTROL packet.
//
// This value is COUPLED TO THE RASPBERRY PI and must not be lowered alone.
// pi-control/controller.py sends a packet only when the command changes, plus
// a keepalive every _RESEND_INTERVAL = 0.2 s. So with the stick held still the
// link runs at 5 Hz, not at "tens of Hz" as was once assumed here.
//
// 500 ms is therefore two and a half keepalive intervals of margin. 300 ms
// would leave one and a half: a single dropped packet, or one scheduling
// hiccup on a Pi that is also running a TFLite model under the GIL, would
// stop the car mid-demonstration and look like a random fault.
//
// To go faster, change BOTH sides: _RESEND_INTERVAL = 0.05 on the Pi first,
// then this to 300U. Six intervals of margin and a fail-safe twice as quick.
#define UART_CONTROL_TIMEOUT_MS          500U

// The bench-drive console keys ('t'/'v') get their own, longer deadline.
//
// They are fed by terminal key auto-repeat, and auto-repeat does NOT start at
// the repeat rate -- it starts after the repeat DELAY, which is 660 ms by X11
// default and 500 ms on GNOME. With one shared 500 ms deadline the first
// repeat arrives at or after the deadline, so a *held* key produces
// arm -> fail-safe -> re-arm on every hold: the ramp is reset to zero and the
// pack sees a repeated breakaway load instead of one smooth pull. 900 ms
// clears the worst common repeat delay with margin.
//
// This does not weaken the RPi path: that still uses UART_CONTROL_TIMEOUT_MS.
#define BENCH_DRIVE_TIMEOUT_MS           900U

// How long a bench-drive arm stays valid. The first 't'/'v' keypress only
// arms; driving needs a second press inside this window. One stray byte on
// the console therefore cannot command throttle at all -- which matters
// because, unlike the RPi path, a console key has no start byte, no length
// and no checksum protecting it.
#define BENCH_ARM_WINDOW_MS             3000U

// Recovery from a packet that started but was not completed. At 115200 baud
// a 5-byte packet takes about 0.43 ms, so 10 ms is over twenty frame times:
// generous enough never to chop a frame that is merely in flight, short
// enough to resynchronise long before the drive watchdog above expires.
#define UART_PARTIAL_FRAME_TIMEOUT_MS      10U

// --- Pack telemetry interlock -----------------------------------------------
//
// bms_app_pack_state() reports BMS_PACK_UNKNOWN whenever it has no fresh
// sample, i.e. whenever the I2C link to the gauge is down. Treating that as
// "carry on driving" makes the interlock fail-open: a pulled I2C wire silently
// removes the only protection that watches the pack, and the car keeps its
// full-authority bridge enabled with nobody looking at the cells.
//
// So UNKNOWN is given a short grace period (the poll runs every 250 ms and the
// monitor calls a sample stale after 1.5 s, so 2 s is about one missed sample
// beyond the monitor's own staleness limit -- long enough that a single
// hiccup does not stop the car, short enough that a real disconnection does).
// After that the drive is dropped, and re-arming needs a positive, fresh
// BMS_PACK_OK -- not merely "not BLOCKED", which UNKNOWN also satisfies.
//
// Set to 0 to build a firmware that drives on blind. This is the escape hatch
// for the case where the interlock itself is the failure: a broken I2C wire
// discovered ten minutes before a live demonstration must not be able to
// immobilise the vehicle. There is a runtime escape too (console key 'i'),
// which is preferable because it needs no rebuild and announces itself loudly.
//
// Note that neither escape touches BMS_PACK_BLOCKED. BLOCKED is a positive
// statement that the protector has opened the discharge path, and dropping the
// bridge on it is the behaviour that keeps the load off the protector during
// its recovery -- the cycle that destroyed the previous board. That stays
// unconditional.
#ifndef BMS_INTERLOCK_REQUIRE_TELEMETRY
#define BMS_INTERLOCK_REQUIRE_TELEMETRY  1
#endif

#define BMS_TELEMETRY_GRACE_MS         2000U

// --- Independent watchdog ---------------------------------------------------
//
// IWDG runs off the LSI, which is independent of the PLL and of every clock
// this firmware configures, so it survives a clock failure as well as a
// software livelock. Nothing else in this build can recover a main loop that
// stops looping: the drive watchdog, the partial-frame recovery and the pack
// interlock are all evaluated *by* that loop.
//
// Timeout choice. The longest legitimate main-loop iteration is not the drive
// path (microseconds) but the pack monitor's console commands, which do
// blocking I2C: the NV config check reads ten registers, each of which can sit
// in its 50 ms HAL timeout on a sick bus, and the probe/bus-check keys add
// their own hundreds of milliseconds. Round that worst case up to ~1 s. The
// period below is 2.5 s nominal, so roughly 2.5x the worst legitimate
// iteration -- a console command cannot trip it -- while a genuine hang is
// caught in under three seconds.
//
// 2.5 s at LSI 32 kHz: prescaler /128 gives a 250 Hz count, so 625 counts.
// LSI is specified 31.04..32.96 kHz on this part, which puts the real window
// at 2.43..2.58 s. Both ends are comfortably clear of the ~1 s worst case.
#define IWDG_KR_RELOAD    0x0000AAAAU
#define IWDG_KR_ENABLE    0x0000CCCCU
#define IWDG_KR_UNLOCK    0x00005555U
#define IWDG_PRESCALER_128        0x05U   // PR[2:0] = 5 -> LSI/128
#define IWDG_RELOAD_COUNTS         625U   // 625 / (32000/128) = 2.5 s

// Contact bounce on the blue USER button is a burst of edges over a few
// milliseconds. Latching an emergency stop twice is harmless, so this window
// exists only to keep one press from printing several times; it is not a
// correctness requirement.
#define ESTOP_BUTTON_DEBOUNCE_MS    200U

// How long after latching a stop the same button will refuse to clear it.
// Long enough that clearing is unmistakably a second, deliberate press rather
// than a bounce, a knock, or the tail of a panicked double-tap.
#define ESTOP_BUTTON_REARM_MS      1500U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/


/* USER CODE BEGIN PV */

uint8_t rx_byte;

uint8_t uart_packet[UART_PACKET_SIZE];
volatile uint8_t uart_packet_index = 0;

static volatile uint8_t uart_packet_ready = 0;
static uint8_t uart_packet_rx[UART_PACKET_SIZE];

// UART safety state.
// drive_watchdog_armed is set only by valid CONTROL packets. PING/unknown
// packets must not keep the car driving.
static volatile uint32_t last_control_packet_tick = 0U;
static volatile uint32_t last_rx_byte_tick = 0U;
static volatile uint8_t drive_watchdog_armed = 0U;

// Which deadline the CONTROL watchdog is currently enforcing. Set by whichever
// source last commanded the drive, so the RPi keeps its tight 500 ms while the
// console bench keys get the longer window auto-repeat needs.
static volatile uint32_t drive_timeout_ms = UART_CONTROL_TIMEOUT_MS;

// RPi link statistics, printed by the 'l' console key.
//
// Without these the only way to tell a live link from a dead one is to watch
// whether the car moves, which means finding out about a broken link with the
// wheels already turning. These separate the failure modes: no bytes at all
// (cable, ground, RPi not booted) reads differently from bytes arriving but
// failing CRC (baud rate, noise, wrong wiring), which reads differently again
// from good frames being rejected on value range (protocol mismatch).
//
// They are plain counters, never cleared, so a rate is the difference between
// two readings. Incrementing a uint32_t is a few cycles and the ones touched
// from the ISR are written only there, so no guarding is needed.
static volatile uint32_t link_bytes       = 0U;  // bytes seen by the RX ISR
static volatile uint32_t link_frames      = 0U;  // complete 5-byte frames assembled
static volatile uint32_t link_frames_used = 0U;  // frames the main loop actually read
/* Five-byte windows rejected by the checksum -- not frames. After a resync the
 * windows overlap, so on an 0xA5-dense line this can rise much faster than one
 * per frame. Compare it against link_frames, never against a frame count from
 * before the sliding resync existed. */
static volatile uint32_t link_crc_err     = 0U;
static volatile uint32_t link_range_err   = 0U;  // CONTROL rejected on value range
static volatile uint32_t link_control_ok  = 0U;  // CONTROL accepted and acted on
static volatile uint32_t link_estop       = 0U;  // ESTOP frames latched in the ISR
static volatile uint32_t link_uart_err    = 0U;  // HAL error callbacks (noise/overrun)
static volatile uint32_t link_partial     = 0U;  // frames that started but never finished
static volatile uint32_t link_resync      = 0U;  // sliding-window resyncs after a bad checksum

// After the pack protector cuts the discharge path, the bridge must stay
// off until the pack has read healthy continuously for this long. Without
// it the RPi's command stream -- controller.py resends at least every 200 ms
// (_RESEND_INTERVAL = 0.2, so ~5 Hz at rest and faster while the joystick is
// moving) -- would re-arm on the very next packet, and the protector would be
// hammered repeatedly, which is precisely the cycle that destroyed the
// previous board. Use the 'l' console key to see the measured rate.
#define PACK_RECOVER_HOLD_MS  1000U

static volatile uint32_t pack_rearm_after_tick = 0U;

// Pack telemetry interlock state.
//   pack_telemetry_ok    -- the most recent evaluation of bms_app_pack_state()
//                           returned a positive BMS_PACK_OK. Re-arming requires
//                           this, so "the monitor is silent" can never be
//                           mistaken for "the pack is fine".
//   pack_unknown_since   -- when the current run of UNKNOWN began, for the
//                           grace period. Only meaningful while
//                           pack_unknown_active is set.
static volatile uint8_t  pack_telemetry_ok    = 0U;
static volatile uint8_t  pack_unknown_active  = 0U;
static volatile uint32_t pack_unknown_since   = 0U;

// Runtime escape from the telemetry half of the interlock, armed from the
// console with 'i'. Session-scoped on purpose: it is deliberately NOT
// persisted anywhere, so a power cycle puts the interlock back.
static volatile uint8_t  bms_telemetry_interlock_off = 0U;

// --- Emergency stop latch ---------------------------------------------------
//
// "Latest packet wins" is the right rule for CONTROL and the wrong rule for
// ESTOP. The USART1 ISR holds exactly one completed packet, so if an ESTOP and
// a CONTROL both land between two passes of the main loop the ESTOP is
// overwritten and never acted on -- the one packet that must never be dropped
// is the one this design drops most easily.
//
// So ESTOP is not carried in that mailbox at all. The ISR sets this flag, and
// it is sticky: it is cleared only by a deliberate re-arm (console 'e', or a
// board reset), never by the CONTROL packets that keep arriving behind it.
// Acting on the same stop twice is harmless; losing it once is not.
//
// estop_handled is main-loop-only bookkeeping so the fail-safe (which sleeps
// 3 ms and prints) runs once per latch rather than every pass.
static volatile uint8_t estop_latched = 0U;
static uint8_t          estop_handled = 0U;

static volatile uint8_t motor_limit_pct = DC_SPEED_LIMIT_PCT_DEFAULT;


// Reflects the state of R_EN/L_EN (PA6/PA7). The bridge is NOT armed at boot:
// it is armed by the first valid CONTROL packet and disarmed by every fault
// path, so there is no window where a live bridge is unsupervised.
static volatile uint8_t drive_enabled = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

static uint8_t UART_CalcCRC(uint8_t start, uint8_t cmd, uint8_t motor, uint8_t steer);
static void UART_Resync(void);
static int16_t MotorPercent_ToPwm(int8_t percent);
static void UART_SendPacket(uint8_t cmd, int8_t motor, uint8_t steer_position);
static void UART_ProcessPacket(uint8_t *packet);
static bool Console_AuxKey(char c);
static bool Console_MayBlock(void);
static void IWDG_Start(void);

// Declared up here because Console_AuxKey() drives the bench-test keys through
// the very same arming path a CONTROL packet uses, and it is defined above the
// drive helpers.
static void Drive_Arm(void);
static void IWDG_Refresh(void);

// True while the telemetry half of the pack interlock is in force. Compiled
// out entirely by BMS_INTERLOCK_REQUIRE_TELEMETRY=0; otherwise it can still be
// waived for the rest of the session from the console.
static inline bool Bms_TelemetryInterlockActive(void)
{
#if BMS_INTERLOCK_REQUIRE_TELEMETRY
    return (bms_telemetry_interlock_off == 0U);
#else
    (void)bms_telemetry_interlock_off;   // kept referenced in the =0 build
    return false;
#endif
}
// -----------------------------------------------------------------------------
// Vehicle console: steering calibration, speed limit, and the two safety
// overrides.
//
//   e     clear a latched emergency stop (deliberate re-arm)
//   i     waive the pack TELEMETRY interlock for the rest of this session,
//         with a confirmation. Never waives BMS_PACK_BLOCKED.
//   m     report motor limit, emergency-stop latch and interlock state
//
// Steering calibration.
//
// The 0..100 position scale interpolates between STEERING_LEFT_US and
// STEERING_RIGHT_US, and those are only valid for one mechanical setup. After
// the steering geometry changes, driving to 0 or 100 can push the servo into a
// stop -- where it stalls, draws its full stall current continuously from the
// pack, and works against the linkage.
//
// So these keys command the pulse width directly, over the monitor's console
// on the ST-Link VCP, and the stops can be found without rebuilding:
//
//   >  <   step +/- 10 us        =   back to the compiled centre
//   .  ,   step +/- 50 us        s   report the commanded pulse width
//
// Procedure: unload the linkage (or lift the wheels), press '=', then step
// outwards a little at a time. Stop at the FIRST sign of buzzing or of the
// current rising -- that is the stop, not the limit. Back off 50..80 us from
// it and use that as the new STEERING_LEFT_US / STEERING_RIGHT_US.
// -----------------------------------------------------------------------------
static bool Console_AuxKey(char c)
{
    // Tracked separately from Steering_GetCurrentPulseUs(), which lags while
    // the slew is still running -- stepping off the lagging value would make
    // each press move less than it says.
    static uint16_t calib_us = 0U;

    // Waiving the telemetry interlock is a two-key sequence, because it turns
    // off a protection for the rest of the session and a stray keystroke must
    // not be able to do that. The pending state expires on its own so it
    // cannot silently swallow a later 'y' meant for something else.
    static bool     interlock_confirm_pending = false;
    static uint32_t interlock_confirm_tick    = 0U;

    int32_t step = 0;

    if (calib_us == 0U)
    {
        calib_us = Steering_GetCurrentPulseUs();
    }

    if (interlock_confirm_pending)
    {
        if ((int32_t)(HAL_GetTick() - interlock_confirm_tick) >= 0)
        {
            // Expired. Fall through and treat this key as an ordinary command.
            interlock_confirm_pending = false;
        }
        else
        {
            interlock_confirm_pending = false;

            if ((c == 'y') || (c == 'Y'))
            {
                bms_telemetry_interlock_off = 1U;
                bms_printf(
                    "\r\n"
                    "*** PACK TELEMETRY INTERLOCK IS OFF ***\r\n"
                    "*** The car will now drive with NO fresh pack data. Nothing\r\n"
                    "*** is watching cell voltage, current or temperature. The\r\n"
                    "*** protector's own BLOCKED signal is still honoured, but it\r\n"
                    "*** cannot be seen while the I2C link is down.\r\n"
                    "*** Fix the link. Power-cycle the board to restore this.\r\n");
            }
            else
            {
                bms_printf("\r\ninterlock override cancelled -- interlock still ON\r\n");
            }
            return true;
        }
    }

    switch (c)
    {
        case 'l':
        case 'L':
        {
            // Snapshot the counters once. They are written from the RX ISR,
            // so reading each one twice could print a self-inconsistent set
            // (e.g. more frames used than assembled).
            uint32_t now         = HAL_GetTick();
            uint32_t bytes       = link_bytes;
            uint32_t frames      = link_frames;
            uint32_t used        = link_frames_used;
            uint32_t ctrl_ok     = link_control_ok;
            uint32_t crc_bad     = link_crc_err;
            uint32_t range_bad   = link_range_err;
            uint32_t estops      = link_estop;
            uint32_t uart_errs   = link_uart_err;
            uint32_t byte_age    = now - last_rx_byte_tick;
            uint32_t ctrl_age    = now - last_control_packet_tick;

            // Frames the ISR assembled while the main loop was busy, so the
            // mailbox was overwritten before it was read. A few is normal;
            // a large and growing number means the loop is being held up.
            uint32_t overwritten = frames - used;

            // bms_printf() truncates past 256 bytes, so this is deliberately
            // split rather than written as one big format string.
            bms_printf("\r\nRPi link  --  USART1 PB6/PB7 @ 115200 8N1\r\n"
                       "  bytes      %10lu    last byte    %lu ms ago\r\n",
                       (unsigned long)bytes, (unsigned long)byte_age);
            bms_printf("  frames     %10lu    overwritten  %lu\r\n"
                       "  CONTROL ok %10lu    last CONTROL %lu ms ago\r\n",
                       (unsigned long)frames, (unsigned long)overwritten,
                       (unsigned long)ctrl_ok, (unsigned long)ctrl_age);
            bms_printf("  CRC bad(w) %10lu    range bad    %lu\r\n"
                       "  ESTOP rx   %10lu    UART errors  %lu\r\n",
                       (unsigned long)crc_bad, (unsigned long)range_bad,
                       (unsigned long)estops, (unsigned long)uart_errs);
            bms_printf("  truncated  %10lu    resyncs      %lu\r\n",
                       (unsigned long)link_partial, (unsigned long)link_resync);
            bms_printf("  drive watchdog %s  (timeout %u ms)\r\n",
                       drive_watchdog_armed ? "ARMED" : "not armed",
                       (unsigned)UART_CONTROL_TIMEOUT_MS);

            // A verdict, so the numbers do not have to be interpreted under
            // time pressure. Ordered most-fundamental failure first.
            if (bytes == 0U)
            {
                bms_print("  -> NOTHING RECEIVED. Check: RPi booted and its sender\r\n"
                          "     running, PB7 <- RPi TX, PB6 -> RPi RX, common GND.\r\n");
            }
            else if ((ctrl_ok == 0U) && (crc_bad > 0U))
            {
                bms_print("  -> bytes arrive but every frame fails CRC: wrong baud,\r\n"
                          "     TX/RX swapped, or noise on the line.\r\n");
            }
            else if ((ctrl_ok == 0U) && (range_bad > 0U))
            {
                bms_print("  -> frames are valid but values are out of range:\r\n"
                          "     protocol mismatch with the RPi side.\r\n");
            }
            else if (ctrl_ok == 0U)
            {
                bms_print("  -> bytes arrive but no complete CONTROL frame yet.\r\n");
            }
            else if (byte_age > 2000U)
            {
                bms_print("  -> link WAS alive and has gone quiet. RPi stopped\r\n"
                          "     sending, lost power, or the cable came off.\r\n");
            }
            else if (ctrl_age > UART_CONTROL_TIMEOUT_MS)
            {
                bms_print("  -> stale: last good CONTROL is older than the drive\r\n"
                          "     watchdog. The car will not move.\r\n");
            }
            else
            {
                bms_print("  -> link is LIVE\r\n");
            }
            return true;
        }

        case 'h':
        case 'H':
            // Appended under the monitor's own help screen. Kept to a few
            // lines so it does not push the pack readout off a short terminal.
            bms_print("\r\n vehicle keys\r\n"
                      "   t / v   bench drive forward / reverse (hold, no RPi needed)\r\n"
                      "   > <     steer right / left by 10 us      . ,  by 50 us\r\n"
                      "   =       centre steering                  s    show pulse\r\n");
            bms_print("   + -     speed limit up / down 5%         m    show limit\r\n"
                      "   l       RPi link statistics              e    clear e-stop\r\n"
                      "   i       waive pack telemetry interlock (asks to confirm)\r\n");
            return true;

        case 't':
        case 'T':
        case 'v':
        case 'V':
        {
            // Bench drive with no RPi attached.
            //
            // This deliberately reuses the exact path a CONTROL packet takes --
            // Drive_Arm(), the ramp inside MotorDC_SetSpeed(), the same speed
            // limiter and the same drive watchdog -- so what gets tested here
            // is what will actually run in the car, not a bypass around it.
            //
            // Safe by construction: one keypress refreshes the drive watchdog
            // exactly like one packet would. The motor keeps turning only
            // while keys keep arriving; stop pressing and the watchdog opens
            // the bridge after UART_CONTROL_TIMEOUT_MS. There is no state to
            // get stuck in and no way to walk away from a spinning wheel.
            static uint32_t bench_msg_tick = 0U;
            static uint32_t bench_arm_tick = 0U;
            static bool     bench_armed    = false;
            bool forward = ((c == 't') || (c == 'T'));
            uint32_t tick = HAL_GetTick();

            if (estop_latched)
            {
                bench_armed = false;
                bms_print("\r\nemergency stop latched -- clear it with 'e' first\r\n");
                return true;
            }

            // First press only arms; it does not move anything. A console key
            // has no start byte, no length and no checksum behind it, so a
            // single stray byte must not be able to command throttle. The arm
            // expires on its own so it cannot sit waiting indefinitely.
            if (!bench_armed || ((tick - bench_arm_tick) > BENCH_ARM_WINDOW_MS))
            {
                bench_armed   = true;
                bench_arm_tick = tick;
                bms_printf("\r\nbench drive ARMED (%s) -- press the same key again "
                           "within %u ms to actually drive\r\n",
                           forward ? "forward" : "reverse",
                           (unsigned)BENCH_ARM_WINDOW_MS);
                return true;
            }

            bench_arm_tick = tick;

            last_control_packet_tick = tick;
            drive_watchdog_armed = 1U;
            drive_timeout_ms = BENCH_DRIVE_TIMEOUT_MS;

            // Drive_Arm() enforces the pack interlock, so a blocked pack still
            // refuses to drive from here just as it would from a packet.
            Drive_Arm();
            MotorDC_SetSpeed(
                MotorPercent_ToPwm((int8_t)(MOTOR_FORWARD_SIGN * (forward ? 100 : -100))));

            // Held keys arrive several times a second; printing every one
            // would bury the dashboard.
            if ((tick - bench_msg_tick) > 700U)
            {
                bench_msg_tick = tick;
                bms_printf("\r\nbench drive %s at %u%% -- keep the key repeating, "
                           "stops %u ms after the last press\r\n",
                           forward ? "FORWARD" : "REVERSE",
                           (unsigned)motor_limit_pct,
                           (unsigned)BENCH_DRIVE_TIMEOUT_MS);
            }
            return true;
        }

        case 'e':
        case 'E':
            if (!estop_latched)
            {
                bms_printf("\r\nno emergency stop latched\r\n");
            }
            else
            {
                // Only clears the latch. The drive still has to be re-armed by
                // a CONTROL packet, and Drive_Arm() still has to be satisfied
                // about the pack, so this is a permission, not a start.
                //
                // Cleared as one indivisible step so an ESTOP landing between
                // the two stores cannot leave the pair inconsistent. A stop
                // that arrives in the instant BEFORE this is still swallowed --
                // no flag can distinguish "clear the stop I just saw" from
                // "clear the one that arrived a microsecond ago" -- but the ISR
                // has already cut the bridge at register level by then, and the
                // drive only comes back on the next CONTROL packet.
                __disable_irq();
                estop_latched = 0U;
                estop_handled = 0U;
                __enable_irq();
                bms_printf("\r\nemergency stop CLEARED -- drive re-arms on the "
                           "next CONTROL packet\r\n");
            }
            return true;

        case 'i':
        case 'I':
            if (!Bms_TelemetryInterlockActive())
            {
#if BMS_INTERLOCK_REQUIRE_TELEMETRY
                bms_printf("\r\npack telemetry interlock is already OFF for "
                           "this session\r\n");
#else
                bms_printf("\r\npack telemetry interlock is compiled out "
                           "(BMS_INTERLOCK_REQUIRE_TELEMETRY=0)\r\n");
#endif
                return true;
            }

            interlock_confirm_pending = true;
            interlock_confirm_tick    = HAL_GetTick() + 5000U;
            bms_printf("\r\nWAIVE the pack telemetry interlock for the rest of "
                       "this session?\r\n"
                       "The car would then drive with no fresh pack data.\r\n"
                       "Press y within 5 s to confirm, anything else to cancel: ");
            return true;

        case '>': step = +10; break;
        case '<': step = -10; break;
        case '.': step = +50; break;
        case ',': step = -50; break;

        case '=':
            Steering_Enable();
            Steering_SetPosition(50U);
            calib_us = Steering_GetCurrentPulseUs();
            bms_printf("\r\nsteering: centre\r\n");
            return true;

        case '+':
        case '-':
        {
            int32_t lim = (int32_t)motor_limit_pct + ((c == '+') ? 5 : -5);

            if (lim < 0)   { lim = 0; }
            if (lim > 100) { lim = 100; }
            motor_limit_pct = (uint8_t)lim;
            {
                int32_t top = DC_PWM_MIN_START +
                              ((int32_t)(DC_PWM_MAX - DC_PWM_MIN_START) * lim) / 100;
                int32_t arr = (int32_t)__HAL_TIM_GET_AUTORELOAD(&htim1) + 1;

                bms_printf("\r\nmotor limit: %u %%  -> full stick %ld/%ld "
                           "= %ld%% duty\r\n",
                           (unsigned)motor_limit_pct, (long)top, (long)arr,
                           (long)((top * 100) / arr));
            }
            return true;
        }

        case 'm':
        case 'M':
            bms_printf("\r\nmotor limit: %u %%   (+/- to change by 5)\r\n"
                       "bridge: %s   emergency stop: %s   (e to clear)\r\n"
                       "pack telemetry interlock: %s   fresh OK sample: %s\r\n",
                       (unsigned)motor_limit_pct,
                       drive_enabled ? "ARMED" : "off",
                       estop_latched ? "LATCHED" : "clear",
#if BMS_INTERLOCK_REQUIRE_TELEMETRY
                       bms_telemetry_interlock_off ? "WAIVED for this session"
                                                   : "on",
#else
                       "compiled out",
#endif
                       pack_telemetry_ok ? "yes" : "no");
            return true;

        case 's':
        case 'S':
            bms_printf("\r\nsteering: commanded %u us, now at %u us\r\n",
                       (unsigned)calib_us,
                       (unsigned)Steering_GetCurrentPulseUs());
            return true;

        default:
            return false;
    }

    {
        int32_t next = (int32_t)calib_us + step;

        if (next < 0) { next = 0; }
        calib_us = (uint16_t)next;

        Steering_Enable();
        Steering_SetPulseRawUs(calib_us);
        bms_printf("\r\nsteering: %u us  (%+ld)  -- stop at the first buzz "
                   "or rise in current\r\n", (unsigned)calib_us, (long)step);
    }
    return true;
}

// "Is it safe to stall the main loop right now?", asked by the pack monitor
// before every console command that does blocking I2C. Registered in main().
//
// The bridge being armed is the whole test: with R_EN/L_EN low the motor
// cannot move no matter how long this loop is held, and the operator gets
// the full bench console back. With the bridge armed, a second inside one
// I2C burst is a second in which the CONTROL watchdog, the pack interlock
// and the partial-frame recovery are all unevaluated while the wheels turn.
//
// This does NOT cover the monitor's automatic reconnect, which still probes
// on its own every 2 s while the gauge is unreachable -- gating that would
// leave the interlock blind for the rest of the trip after one bus glitch.
static bool Console_MayBlock(void)
{
    return (drive_enabled == 0U);
}

static void Drive_Arm(void);
static void Drive_FailSafe(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_TIM4_Init();
  MX_USART1_UART_Init();
  MX_TIM1_Init();
  MX_I2C3_Init();
  /* USER CODE BEGIN 2 */

  MotorDC_Init();

  // DC motor PWM for BTS7960: PA8/TIM1_CH1 = RPWM, PA9/TIM1_CH2 = LPWM.
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
  MotorDC_SetSpeed(0);

  // R_EN/L_EN deliberately stay LOW here. The bridge is armed by the first
  // valid CONTROL packet (or by the self-test block), never at boot: the
  // drive watchdog does not arm until that packet either, and a live bridge
  // with no supervision is exactly the state to avoid while the RPi boots.
  HAL_Delay(500);

  // A35BHLP steering servo on PA1/TIM2_CH2.
  // Steering scale is 0..100: 0=left, 50=center, 100=right.
  Steering_Init();
  Steering_Enable();
  Steering_SetPosition(50);

  // TIM4 calls Steering_Update() every 2 ms.
  HAL_TIM_Base_Start_IT(&htim4);

  // USART1 PB6/PB7 packet receiver.
  HAL_UART_Receive_IT(&huart1, &rx_byte, 1);

  // Console for the pack monitor. On this board BSP COM1 is LPUART1 on
  // PA2/PA3, which is exactly what the ST-Link exposes as a virtual COM
  // port. The RPi link is USART1 on PB6/PB7 and is not touched by this.
  {
    COM_InitTypeDef BspCOMInit;
    BspCOMInit.BaudRate   = 115200;
    BspCOMInit.WordLength = COM_WORDLENGTH_8B;
    BspCOMInit.StopBits   = COM_STOPBITS_1;
    BspCOMInit.Parity     = COM_PARITY_NONE;
    BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
    if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
    {
      Error_Handler();
    }
  }

  // MAX17320 pack monitor. Shares this loop, so it is built with
  // BMS_REALTIME_HOST: no blocking modes, and the register poll is spread
  // over calls so no single call holds the loop for more than about a
  // millisecond. Its console is LPUART1 (the ST-Link VCP), which is free
  // here -- the RPi link is USART1 on PB6/PB7 and is untouched.
  bms_app_init(&hi2c3, &hcom_uart[COM1]);

  // Steering calibration keys ride on the monitor's console. See
  // Console_AuxKey() -- needed until the limits are re-measured after the
  // steering geometry was opened up.
  bms_app_set_aux_key_handler(Console_AuxKey);

  // The monitor's heavy console commands ('n', 'x', 'c', 'b', 'p') do
  // blocking I2C and can hold this loop for up to a second. That is longer
  // than UART_CONTROL_TIMEOUT_MS, so a keystroke would stall the very loop
  // that is supposed to stop the motor. Refuse them while the bridge is
  // armed; with the drive off they behave exactly as on the bench.
  bms_app_set_block_guard(Console_MayBlock);

  // Watchdog is not armed at boot. It becomes armed only after the first
  // valid CONTROL packet. This avoids a false fail-safe while the RPi boots.
  last_control_packet_tick = HAL_GetTick();
  last_rx_byte_tick = HAL_GetTick();
  drive_watchdog_armed = 0U;

  /* USER CODE END 2 */

  /* Initialize led */
  BSP_LED_Init(LED_GREEN);

  /* Initialize USER push-button, used here as the bench emergency stop.
     See HAL_GPIO_EXTI_Callback(). */
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* USER CODE BEGIN WHILE */

  // Start the hardware watchdog last, so nothing above it -- the 500 ms settle
  // delay, BSP_COM_Init, the monitor's first I2C probe -- can trip it. A hang
  // during init leaves the bridge disabled (R_EN/L_EN are still low and TIM1
  // is at zero duty), which is already the safe state; it is the running loop
  // that needs watching. Once started, IWDG cannot be stopped or reconfigured
  // until the next reset.
  IWDG_Start();

  /* Infinite loop */
  while (1)
  {
    // Pack monitor: one short slice of work per pass, never blocking.
    bms_app_run_once();

    // Pack safety interlock. The protector opens the discharge FET in
    // microseconds on an overcurrent, and the bridge must not stay enabled
    // into that transient -- keeping the load on both fights the
    // protector's own recovery test and leaves the driver's rail collapsing
    // while its enable pin is still held high by this MCU.
    //
    // UNKNOWN means the monitor has no fresh sample, i.e. we are blind. It is
    // given BMS_TELEMETRY_GRACE_MS to come back, and after that it is treated
    // as a fault: driving a pack nobody is watching is the state that this
    // whole interlock exists to prevent. See BMS_INTERLOCK_REQUIRE_TELEMETRY
    // above for the two escape hatches.
    {
        bms_pack_state_t pack = bms_app_pack_state();
        uint32_t pack_now = HAL_GetTick();

        if (pack == BMS_PACK_BLOCKED)
        {
            pack_telemetry_ok   = 0U;
            pack_unknown_active = 0U;

            // Push the re-arm deadline out on every pass the pack is bad,
            // so the hold is measured from the last fault, not the first.
            pack_rearm_after_tick = pack_now + PACK_RECOVER_HOLD_MS;

            if (drive_enabled)
            {
                Drive_FailSafe();
            }
        }
        else if (pack == BMS_PACK_OK)
        {
            // The only place this is ever set. Drive_Arm() requires it, so
            // "the monitor went quiet" can never be read as "the pack is fine".
            pack_telemetry_ok   = 1U;
            pack_unknown_active = 0U;
        }
        else /* BMS_PACK_UNKNOWN -- no fresh sample */
        {
            pack_telemetry_ok = 0U;

            if (!pack_unknown_active)
            {
                pack_unknown_active = 1U;
                pack_unknown_since  = pack_now;
            }

            if (Bms_TelemetryInterlockActive() &&
                ((pack_now - pack_unknown_since) > BMS_TELEMETRY_GRACE_MS) &&
                drive_enabled)
            {
                Drive_FailSafe();
            }
        }
    }

    // UART packet is assembled in USART interrupt, but processed here in main loop.
// Do not process full commands inside the interrupt.
    if (uart_packet_ready)
    {
        uint8_t packet_local[UART_PACKET_SIZE];

        __disable_irq();
        memcpy(packet_local, uart_packet_rx, UART_PACKET_SIZE);
        uart_packet_ready = 0U;
        __enable_irq();

        UART_ProcessPacket(packet_local);
    }

    // --- Emergency stop latch ------------------------------------------
    // Deliberately after packet processing: an ESTOP that the ISR latched
    // while a CONTROL packet was being applied above is undone in the same
    // pass, instead of one pass later with the bridge live in between.
    //
    // The latch itself is not cleared here. It stays set until somebody
    // presses 'e' on the console or resets the board, so the CONTROL packets
    // still streaming in behind the stop cannot drive away from it.
    if (estop_latched && !estop_handled)
    {
        estop_handled = 1U;

        Drive_FailSafe();   // zero PWM, settle, then bridge off -- coast
        Steering_Disable(); // immediate center + PWM hold

        bms_printf("\r\n*** EMERGENCY STOP LATCHED -- drive disabled. "
                   "Press 'e' to re-arm. ***\r\n");
    }

    // --- CONTROL watchdog ---------------------------------------------
    // The drive motor is allowed to keep running only while valid CONTROL
    // packets keep arriving periodically. If the RPi freezes, reboots, or the
    // UART cable is unplugged, this stops the car instead of holding the last
    // speed forever.
    uint32_t now = HAL_GetTick();

    // drive_timeout_ms is UART_CONTROL_TIMEOUT_MS for the RPi and the longer
    // BENCH_DRIVE_TIMEOUT_MS while the console bench keys are driving; see
    // where each one sets it.
    if (drive_watchdog_armed &&
        ((now - last_control_packet_tick) > drive_timeout_ms))
    {
        Drive_FailSafe();   // zero PWM, then R_EN/L_EN low
        Steering_Disable(); // immediate center + PWM hold
    }

    // --- Stalled partial-frame recovery --------------------------------
    // If a frame started but did not finish, drop it after a short timeout so
    // the next real 0xA5 start byte can synchronize the parser again.
    // Keep the check+reset atomic to avoid resetting a freshly updated frame.
    //
    // The tick is read INSIDE the critical section. Read outside it, the
    // USART ISR could fire between the read and __disable_irq() and push
    // last_rx_byte_tick past the sampled "now" -- the subtraction then wraps
    // to a huge unsigned value and a frame that arrived microseconds ago is
    // discarded as stale, right in the middle of the byte stream.
    uint8_t partial_frame_dropped = 0U;
    __disable_irq();
    {
        uint32_t frame_now = HAL_GetTick();

        if ((uart_packet_index != 0U) &&
            ((frame_now - last_rx_byte_tick) > UART_PARTIAL_FRAME_TIMEOUT_MS))
        {
            uart_packet_index = 0U;
            partial_frame_dropped = 1U;
        }
    }
    __enable_irq();

    // A truncated frame is counted, not braked on.
    //
    // This used to call Drive_FailSafe(), which is a hard brake: MotorDC_Stop()
    // puts BOTH compares at zero, so both low-side switches conduct and the
    // motor is shorted through the bridge -- applied within one 50 us PWM
    // period, at whatever speed the car was doing. Recovery then costs about
    // half a second of ramp just to reach breakaway. One glitch on a jumper
    // produced a lurch, a stall and a slow crawl back up.
    //
    // It was a safety mechanism aimed at the wrong event. A truncated frame
    // means ONE command was lost. A dead link is what must drop the bridge,
    // and the CONTROL watchdog above already does that, more reliably. The
    // count is visible on the 'l' screen, which is where a flaky cable should
    // show up -- as evidence, not as a brake.
    if (partial_frame_dropped)
    {
        link_partial++;
    }

#if STEERING_DEMO_SELFTEST
    // ---------------- TEST WHILE BLOCK ----------------
    // DRIVE self-test: it centres the servo and then RUNS THE MOTOR, forward
    // and reverse, at 40 % of DC_PWM_MAX. It is not the "servo-only" test the
    // comment here used to call it -- the wheels turn, so the car must be on
    // blocks. Steering scale: 0 = left, 50 = center, 100 = right; the real
    // pulse range is limited in steering.c to the calibrated safe values
    // 2400 us / 1550 us / 750 us (measured 2026-08-16), not the full
    // 500..2500 us passport range.
    //
    // Only for bench testing: this keeps overwriting the steering target and
    // blocks the loop with HAL_Delay(), so real UART control must stay off
    // (STEERING_DEMO_SELFTEST == 0) whenever a UART master is connected.
    // The bridge is not armed at boot any more, so the self-test has to ask
    // for it. All speed changes below now go through the ramp in motor_dc.c.
    //
    // The watchdog is kicked between the delays: each individual HAL_Delay()
    // is shorter than the IWDG period, but the block as a whole is not, and a
    // bench self-test must not look like a hang.
    //
    // Drive_Arm() now also needs a fresh BMS_PACK_OK, so on a bench with no
    // gauge attached this block will spin without ever driving. Build with
    // -DBMS_INTERLOCK_REQUIRE_TELEMETRY=0 for that case.
    Drive_Arm();
    Steering_Enable();
    Steering_SetPosition(50);
    MotorDC_SetSpeed(0);

    HAL_Delay(1000);
    IWDG_Refresh();

    // Physically forward: see MOTOR_FORWARD_SIGN. Negative compare = LPWM.
    MotorDC_SetSpeed(MOTOR_FORWARD_SIGN * (DC_PWM_MAX * 40 / 100));
    HAL_Delay(500);
    IWDG_Refresh();

    MotorDC_SetSpeed(0);
    HAL_Delay(1000);
    IWDG_Refresh();

    // Physically reverse.
    MotorDC_SetSpeed(-MOTOR_FORWARD_SIGN * (DC_PWM_MAX * 40 / 100));
    HAL_Delay(500);
    IWDG_Refresh();

    MotorDC_SetSpeed(0);
    HAL_Delay(1000);
    // --------------------------------------------------
#endif

    // --- Hardware watchdog ---------------------------------------------
    // Refreshed once per pass, at the end, so the kick is evidence that a
    // whole iteration completed -- monitor slice, pack interlock, packet
    // processing, emergency stop, drive watchdog and frame recovery. Kicking
    // it at the top of the loop instead would keep the board alive through a
    // hang in any of those.
    IWDG_Refresh();

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

static uint8_t UART_CalcCRC(uint8_t start, uint8_t cmd, uint8_t motor, uint8_t steer)
{
    return (uint8_t)(start ^ cmd ^ motor ^ steer);
}

// Arms the BTS7960. PWM is forced to zero first, so the bridge can never come
// up with a non-zero duty already pending.
static void Drive_Arm(void)
{
    // A latched emergency stop outranks everything. Only a deliberate re-arm
    // clears it.
    if (estop_latched)
    {
        return;
    }

    // Refuse to arm on the absence of bad news. BMS_PACK_UNKNOWN is "no fresh
    // sample", which is not evidence of anything, so arming requires a
    // positive BMS_PACK_OK seen on the most recent pass of the main loop.
    if (Bms_TelemetryInterlockActive() && !pack_telemetry_ok)
    {
        return;
    }

    // Refuse to arm into a pack that has just cut out, or that has not been
    // continuously healthy since it did.
    if ((int32_t)(HAL_GetTick() - pack_rearm_after_tick) < 0)
    {
        return;
    }

    if (!drive_enabled)
    {
        MotorDC_Stop();

        // The latch is re-tested with interrupts off, immediately before the
        // enables go high. Tested only at the top of this function, an ESTOP
        // landing in between would have its register-level Drive_EmergencyOff()
        // undone by the write below -- the bridge would come back up for the
        // few hundred microseconds until the main loop reached the latch
        // handler, which is a stop that visibly did not stop the car.
        //
        // A stop arriving after __enable_irq() is safe without this: it finds
        // drive_enabled == 1, its EmergencyOff() stands, and nothing re-arms
        // because CONTROL packets return early while the latch is set.
        __disable_irq();
        if (!estop_latched)
        {
            HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);
            drive_enabled = 1U;
        }
        __enable_irq();
    }
}

// Removes drive on any fault. The order matters:
//   1. PWM to zero. Both low sides turn on and the winding current decays
//      inside the bridge, drawing nothing from the pack.
//   2. Short settle. The winding time constant is a few hundred microseconds,
//      so 3 ms is many time constants.
//   3. Only then drop R_EN/L_EN.
// Dropping the enables first would push the remaining winding current back
// through the bridge diodes into the pack, which is its own way to trip the
// protector. This runs only on faults, so the 3 ms is not on any hot path.
static void Drive_FailSafe(void)
{
    drive_watchdog_armed = 0U;
    MotorDC_Stop();
    HAL_Delay(3);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_RESET);
    drive_enabled = 0U;
}

// Register-level drive kill. Safe to call from a fault handler: it touches no
// HAL state, takes no locks and cannot block.
void Drive_EmergencyOff(void)
{
    TIM1->CCR1 = 0U;
    TIM1->CCR2 = 0U;
    TIM1->EGR  = TIM_EGR_UG;   // force the shadow transfer now, not next period

    GPIOA->BSRR = (uint32_t)(GPIO_PIN_6 | GPIO_PIN_7) << 16U;  // R_EN/L_EN low
}

// -----------------------------------------------------------------------------
// Independent watchdog.
//
// Driven at register level rather than through HAL_IWDG. The IWDG HAL module is
// not in this project's driver source list, and adding it would mean editing
// the CubeMX-generated CMake file for four register writes that have no error
// cases: KR/PR/RLR is the whole peripheral. Enabling the watchdog also starts
// the LSI implicitly, so there is nothing to configure in RCC either.
//
// See IWDG_RELOAD_COUNTS above for why the period is what it is.
// -----------------------------------------------------------------------------
static void IWDG_Start(void)
{
    // Freeze the counter whenever the core is halted by the debugger.
    // Without this, every breakpoint in a Debug build resets the board and
    // the bug being chased is destroyed by the tool looking for it. The bit
    // has no effect when no debugger is attached, so it is safe to leave in
    // the release path too.
    __HAL_DBGMCU_FREEZE_IWDG();

    IWDG->KR = IWDG_KR_ENABLE;   // start the counter (and the LSI with it)
    IWDG->KR = IWDG_KR_UNLOCK;   // allow PR/RLR to be written

    IWDG->PR  = IWDG_PRESCALER_128;
    IWDG->RLR = IWDG_RELOAD_COUNTS;

    // PR and RLR are written across the LSI clock domain; the values are not
    // in force until the update flags clear. Reloading before that would load
    // the reset-default 0xFFF instead of our value. The wait is bounded: the
    // flags clear within a few LSI cycles once the oscillator is up, so if
    // 100 ms is not enough the LSI is not running, the watchdog is already
    // counting on its 512 ms reset-default period, and there is no safe way
    // to continue -- Error_Handler() kills the drive and parks.
    {
        uint32_t started = HAL_GetTick();

        while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U)
        {
            if ((HAL_GetTick() - started) > 100U)
            {
                Error_Handler();
            }
        }
    }

    IWDG->KR = IWDG_KR_RELOAD;   // load the counter and re-lock PR/RLR
}

// Kicks the watchdog. A single register write, no HAL state, no blocking --
// safe to call from anywhere, including the fault handlers.
static void IWDG_Refresh(void)
{
    IWDG->KR = IWDG_KR_RELOAD;
}

// Same kick, exported so the fault handlers in stm32g4xx_it.c can hold the
// board in their diagnosed, drive-dead state instead of being reset out of it.
void Watchdog_Kick(void)
{
    IWDG_Refresh();
}

static int16_t MotorPercent_ToPwm(int8_t percent)
{
    if (percent == 0)
    {
        return 0;
    }

    if (percent > DC_PERCENT_MAX)
    {
        percent = DC_PERCENT_MAX;
    }
    else if (percent < -DC_PERCENT_MAX)
    {
        percent = -DC_PERCENT_MAX;
    }

    int8_t sign = 1;
    int32_t abs_percent = percent;  // int32_t чтобы избежать переполнения в формуле

    if (abs_percent < 0)
    {
        sign = -1;
        abs_percent = -abs_percent;
    }

    // На ARM int 32-битный, поэтому промежуточное умножение скорее всего
    // и так считалось бы без переполнения. Но явный int32_t надёжнее и
    // не зависит от платформы — оставляем для ясности.
    // Верхняя граница масштабируется ограничителем скорости, порог трогания -
    // нет. Иначе при малом ограничителе мотор просто перестал бы стартовать.
    const int32_t span_max = DC_PWM_MIN_START +
                             ((int32_t)(DC_PWM_MAX - DC_PWM_MIN_START) *
                              (int32_t)motor_limit_pct) / 100;

    int16_t pwm = (int16_t)(DC_PWM_MIN_START +
                  ((abs_percent - 1) * (span_max - DC_PWM_MIN_START)) / 99);

    return sign * pwm;
}


// Sliding-window resynchronisation, run only after a complete five-byte window
// has FAILED its checksum.
//
// The old parser threw away all five bytes and started over. If one byte is
// lost on the wire, the window that fails is [A5][C][M][X][A5] -- and the
// trailing 0xA5 it discards is the real start of the NEXT frame, so that frame
// is destroyed too. Instead, drop one byte and look for the next 0xA5 among
// the four still in hand, then slide it to the front.
//
// Be honest about when this actually earns its keep. Two frames are lost per
// single-byte error only when the next start byte arrives before the
// partial-frame timeout has been serviced -- that is, frames spaced closer
// than UART_PARTIAL_FRAME_TIMEOUT_MS, or any gap in which the main loop was
// stalled. At the RPi's resting 5 Hz keepalive the 10 ms timeout already
// clears a truncated frame during the gap, so a DELETED byte costs one frame
// with or without this code. What the resync does cover is a CORRUPTED byte,
// whose window completes inside the burst, and any future sender that packs
// frames closer together. The simulation quoted below modelled a gapless
// stream: 2.00 frames lost without the resync, 1.00 with it.
//
// It is bounded, not statistical: only bytes preceding the earliest surviving
// 0xA5 are discarded, so the true frame start can never be skipped past, and
// each call consumes at least one byte, so it always makes progress.
//
// Note what this does NOT do: it never resynchronises inside a frame that is
// still being assembled. The deliberate "do not resync on 0xA5 mid-frame" rule
// is untouched -- packet[2] is a signed motor percent and is legitimately 0xA5
// at -91. Alignment is only ever reconsidered after a window has already
// failed its check, which is information the old code discarded.
//
// Called from the RX ISR with the buffer it owns.
static void UART_Resync(void)
{
    for (uint8_t i = 1U; i < UART_PACKET_SIZE; i++)
    {
        if (uart_packet[i] == UART_PACKET_START)
        {
            memmove(uart_packet, &uart_packet[i], (size_t)(UART_PACKET_SIZE - i));
            uart_packet_index = (uint8_t)(UART_PACKET_SIZE - i);
            link_resync++;
            return;
        }
    }

    uart_packet_index = 0U;
}

static void UART_SendPacket(uint8_t cmd, int8_t motor, uint8_t steer_position)
{
    uint8_t packet[UART_PACKET_SIZE];

    packet[0] = UART_PACKET_START;
    packet[1] = cmd;
    packet[2] = (uint8_t)motor;
    packet[3] = steer_position;
    packet[4] = UART_CalcCRC(packet[0], packet[1], packet[2], packet[3]);

    HAL_UART_Transmit(&huart1, packet, UART_PACKET_SIZE, 10);
}

static void UART_ProcessPacket(uint8_t *packet)
{
    link_frames_used++;

    if (packet[0] != UART_PACKET_START)
    {
        return;
    }

    uint8_t cmd = packet[1];

    // DC motor is signed:
    // -100..-1 = reverse, 0 = stop, +1..+100 = forward
    int8_t motor_percent = (int8_t)packet[2];

    // Steering servo is unsigned:
    // 0 = left, 50 = center, 100 = right
    uint8_t steer_position = packet[3];

    uint8_t crc_rx = packet[4];
    uint8_t crc_calc = UART_CalcCRC(packet[0], packet[1], packet[2], packet[3]);

    if (crc_rx != crc_calc)
    {
        link_crc_err++;
        return;
    }

    switch (cmd)
    {
        case UART_CMD_CONTROL:
        {
            // A latched emergency stop is not something the command stream can
            // drive out of. Ignore CONTROL entirely -- do not touch the motor,
            // do not touch the steering, and above all do not refresh the
            // drive watchdog, so nothing here looks like a live link.
            if (estop_latched)
            {
                return;
            }

            if ((steer_position > 100U) ||
                (motor_percent > 100) ||
                (motor_percent < -100))
            {
                // Wrong protocol value. Stop safely and center steering.
                link_range_err++;
                Drive_FailSafe();
                Steering_Disable();
                return;
            }

            // MOTOR_FORWARD_SIGN is the one place the physical wiring is
            // converted into the protocol's sense of "forward".
            int16_t motor_pwm =
                MotorPercent_ToPwm((int8_t)(MOTOR_FORWARD_SIGN * motor_percent));

            // Only valid CONTROL packets keep the drive watchdog alive.
            // Repeated identical CONTROL packets are normal and harmless.
            last_control_packet_tick = HAL_GetTick();
            drive_watchdog_armed = 1U;
            drive_timeout_ms = UART_CONTROL_TIMEOUT_MS;
            link_control_ok++;

            Drive_Arm();
            Steering_Enable();
            MotorDC_SetSpeed(motor_pwm);
            Steering_SetPosition(steer_position);

            break;
        }

        case UART_CMD_STOP:
        {
            // Keep the watchdog ARMED, and refresh it.
            //
            // This used to disarm it, which was the one place in the file that
            // stopped supervising the link without also opening the bridge:
            // Drive_FailSafe() clears both flags together, STOP cleared only
            // the watchdog. Because the watchdog's guard is the armed flag and
            // not the age of the last packet, the result was an enabled
            // full-authority bridge with nothing watching the link at all --
            // and with both compares at zero the winding sits shorted across
            // it, so a car that is pushed or rolls circulates its back-EMF
            // through the low-side FETs unsupervised.
            //
            // Keeping it armed means the ramp-down still runs, and if no
            // further packet arrives the CONTROL watchdog opens the bridge
            // half a second later, exactly as it does for any other silence.
            //
            // Nothing else covered this: the truncated-frame handler used to,
            // by accident, and it no longer brakes on a lost byte.
            last_control_packet_tick = HAL_GetTick();
            drive_watchdog_armed = 1U;
            drive_timeout_ms = UART_CONTROL_TIMEOUT_MS;

            MotorDC_SetSpeed(0);
            Steering_Enable();
            Steering_SetPosition(50U); // smooth center through Steering_Update()
            break;
        }

        case UART_CMD_ESTOP:
        {
            // Normally unreachable: the ISR latches ESTOP and the main loop
            // acts on the latch, so an ESTOP never reaches the one-packet
            // mailbox this function reads from. Kept as a backstop -- if the
            // latch path is ever changed, an ESTOP that arrives by this route
            // must still stop the car rather than fall through to default.
            estop_latched = 1U;
            break;
        }

        case UART_CMD_PING:
        {
            // Report current commanded servo position after slew-rate limiting.
            // This is not external physical feedback from the servo.
            UART_SendPacket(UART_CMD_PING, 0, Steering_GetCurrentPosition());
            break;
        }

        default:
        {
            break;
        }
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        last_rx_byte_tick = HAL_GetTick();
        link_bytes++;

        if (uart_packet_index == 0U)
        {
            if (rx_byte == UART_PACKET_START)
            {
                uart_packet[0] = rx_byte;
                uart_packet_index = 1U;
            }
        }
        else
        {
            // Fixed-length binary packet.
            // Do NOT resync on 0xA5 inside an already started packet:
            // packet[2] is signed motor_percent and can legitimately be 0xA5
            // when motor_percent == -91.
            // Corrupted packets are rejected by CRC.
            uart_packet[uart_packet_index++] = rx_byte;

            if (uart_packet_index >= UART_PACKET_SIZE)
            {
                // ESTOP is taken out of the mailbox and latched right here.
                //
                // The mailbox below holds exactly one packet and the newest
                // one wins, which is correct for CONTROL -- an old throttle
                // value is worthless. It is wrong for ESTOP: the RPi streams
                // CONTROL continuously, so a CONTROL packet lands behind the
                // ESTOP within a millisecond and overwrites it, and the car
                // never stops. Latching costs four XORs in the ISR and cannot
                // be lost. Acting on the same stop twice is harmless.
                //
                // The CRC is checked here for the same reason the main loop
                // checks it: a corrupted frame must not be able to invent a
                // stop. uart_packet[0] is UART_PACKET_START by construction.
                // The checksum is now tested HERE rather than only in the main
                // loop, because a failed window is what drives the sliding
                // resynchronisation below -- and that has to happen in the
                // buffer this ISR owns, before the bytes are gone.
                bool frame_ok = (uart_packet[4] == UART_CalcCRC(uart_packet[0],
                                                                uart_packet[1],
                                                                uart_packet[2],
                                                                uart_packet[3]));

                if (!frame_ok)
                {
                    // Bad window: keep whatever might be the start of the next
                    // frame instead of discarding all five bytes.
                    link_crc_err++;
                    UART_Resync();
                }
                else
                {
                    if (uart_packet[1] == UART_CMD_ESTOP)
                    {
                        estop_latched = 1U;
                        link_estop++;

                        // Kill the bridge now, at register level, instead of
                        // waiting for the main loop: a monitor console command
                        // can hold that loop for a few hundred milliseconds,
                        // and an emergency stop must not queue behind it. The
                        // main loop still runs the full, ordered fail-safe
                        // afterwards.
                        Drive_EmergencyOff();
                    }

                    link_frames++;

                    // Latest complete packet wins.
                    // This is better for high-rate repeated control commands.
                    memcpy(uart_packet_rx, uart_packet, UART_PACKET_SIZE);
                    uart_packet_ready = 1U;

                    uart_packet_index = 0U;
                }
            }
        }

        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        // Noise / overrun / framing error can stop HAL RX.
        // Reset packet assembly and restart byte reception.
        uart_packet_index = 0U;
        link_uart_err++;

        HAL_UART_AbortReceive(huart);
        HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
    }
}

// -----------------------------------------------------------------------------
// USER button (blue B1, PC13) = bench emergency stop.
//
// The EXTI line was already configured by BSP_PB_Init() and had no handler, so
// pressing the only button on the board did nothing. It now latches exactly the
// same emergency stop as the UART ESTOP command: same flag, same fail-safe in
// the main loop, same deliberate re-arm with 'e' on the console.
//
// This is the stop that works when the RPi is the thing that has failed, so it
// deliberately does not depend on the UART link, on the main loop having
// reached its next pass, or on the pack monitor being alive.
// -----------------------------------------------------------------------------
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == USER_BUTTON_PIN)
    {
        static uint32_t last_press_tick = 0U;
        uint32_t now = HAL_GetTick();

        // Contact bounce is a burst of rising edges. Only the first edge of a
        // burst is a press. Note that a debounce failure here could only
        // re-latch a stop that is already latched, so this window is about
        // keeping the console readable, not about correctness.
        if ((last_press_tick != 0U) &&
            ((now - last_press_tick) < ESTOP_BUTTON_DEBOUNCE_MS))
        {
            return;
        }
        // Second action: a press while a stop is already latched CLEARS it, so
        // the one button on this board is the whole stop control and the
        // console is not needed to get moving again.
        //
        // Guarded by a much longer window than the debounce above. Clearing a
        // stop must not be something a bounce, a knock or a nervous double-tap
        // can do -- it has to be a separate, deliberate press. Latching, by
        // contrast, stays instant: making a stop harder to reach would be the
        // wrong trade in every case.
        if (estop_latched)
        {
            if ((now - last_press_tick) < ESTOP_BUTTON_REARM_MS)
            {
                return;     /* too soon after the stop -- ignore, stay stopped */
            }

            last_press_tick = now;
            estop_latched = 0U;

            // Deliberately does NOT re-enable the bridge. Clearing the latch
            // only removes the block; the drive still has to be commanded
            // again by the RPi or the bench keys, and Drive_Arm() still has to
            // be satisfied. Releasing a stop must never itself start a motor.
            return;
        }

        last_press_tick = now;

        estop_latched = 1U;

        // Immediate register-level kill, for the same reason as the UART
        // ESTOP path: the main loop may be several hundred milliseconds away.
        Drive_EmergencyOff();
    }
}

// The Nucleo BSP routes EXTI through its own HAL_EXTI handle, so this weak
// hook -- not HAL_GPIO_EXTI_Callback -- is what actually fires for B1. Both
// entry points end up in the same place so the behaviour does not depend on
// which one a future rewiring uses.
void BSP_PB_Callback(Button_TypeDef Button)
{
    if (Button == BUTTON_USER)
    {
        HAL_GPIO_EXTI_Callback(USER_BUTTON_PIN);
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4)
    {
        Steering_Update();
        MotorDC_Update();   // same 2 ms tick drives the DC speed ramp
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  // Kill drive BEFORE parking the CPU. TIM1 is a peripheral: without this it
  // keeps generating PWM at the last commanded duty, with the bridge still
  // enabled, for as long as the board has power.
  Drive_EmergencyOff();
  BSP_LED_Init(LED_GREEN);
  __disable_irq();
  while (1)
  {
    // Kick the watchdog from inside the park loop.
    //
    // This looks backwards, but the drive is already dead and staying here is
    // the DEFINED safe state: motor off, bridge off, LED blinking, a human
    // can see what happened. Letting IWDG reset out of it would instead give
    // a silent reboot loop in which the bridge is re-armed by the next
    // CONTROL packet, faults again, and repeats -- the pack protector
    // hammered several times a second, which is the exact cycle that
    // destroyed the previous board. The watchdog is there to catch a loop
    // that has silently stopped looping, not to escape a diagnosed fault.
    Watchdog_Kick();

    BSP_LED_Toggle(LED_GREEN);
    for (volatile uint32_t i = 0; i < 300000U; i++)
    {
    }
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
