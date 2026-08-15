#ifndef BMS_APP_H
#define BMS_APP_H

/*
 * Cooperative superloop for the MAX17320 monitor: three periodic jobs
 * driven off HAL_GetTick(), no RTOS.
 *
 *   poll    250 ms  -- one I2C pass over the gauge (it refreshes its own
 *                      outputs every 351 ms, so this is comfortably fast
 *                      enough without hammering the bus)
 *   render  500 ms  -- repaint the ANSI dashboard, or emit one JSON line
 *   cli     every iteration -- non-blocking key handling
 *
 * The periodic work never blocks: printing goes through an interrupt-drained
 * ring, and with BMS_REALTIME_HOST a polling pass is spread over calls a few
 * registers at a time. The operator commands are the exception -- 'n', 'p',
 * 'x', 'c' and 'b' do their I2C synchronously and can hold the loop for a
 * long time on a dead bus, which is what bms_app_set_block_guard() below is
 * for.
 *
 * Wiring this into a CubeMX project:
 *
 *   main.c, USER CODE BEGIN Includes:  #include "bms_app.h"
 *   main.c, USER CODE BEGIN WHILE:     bms_app_init(&hi2c1, &hcom_uart[COM1]);
 *                                      ... then bms_app_run_once() in the loop
 *
 * The console handle comes from the Nucleo BSP rather than from a CubeMX
 * LPUART1 block: BSP_COM_Init(COM1, ...) in the generated main.c already
 * configures LPUART1 on PA2/PA3 at 115200 8N1, which is the pair wired to
 * the ST-Link VCP on this board. Init must therefore run after that call,
 * hence USER CODE BEGIN WHILE rather than USER CODE BEGIN 2.
 */

#include "max17320_monitor.h"
#include "bms_io.h"

void bms_app_init(I2C_HandleTypeDef *hi2c, UART_HandleTypeDef *huart);
void bms_app_run_once(void);

/*
 * Pack state as a RECOVERY interlock for whatever else this firmware
 * drives. Read the latency note before designing anything around it.
 *
 * WHAT PROTECTS THE HARDWARE IS THE MAX17320 ITSELF. It opens the
 * discharge FET in microseconds, in silicon, with no help from this MCU.
 * Nothing polled over a 100 kHz I2C bus can be part of that path, and this
 * is not trying to be.
 *
 * What this is for is the aftermath. Once the protector has cut the path,
 * everything downstream loses its supply while the load is still enabled
 * and still demanding current -- the transient that destroyed the MCU pin
 * driving the motor bridge on the previous board. A host that watches this
 * disables the bridge shortly after the pack disconnects, which removes
 * the load fighting the protector's recovery attempt and leaves the driver
 * in a defined state instead of a collapsing one.
 *
 * LATENCY, HONESTLY: HProtCfg2 (the live FET readback) and the fault
 * registers are read in the first few reads of a polling pass, and are
 * published to this function as soon as they are in -- but a pass only
 * STARTS every 250 ms. So a change is visible here typically ~125 ms and
 * at worst ~250 ms plus one loop iteration after the protector acted; on
 * the weak-bus fallback the pass rate drops to 1 s and so does this. That
 * is fine for recovery and useless as protection. Do not add anything to
 * the drive path that assumes it is fast.
 *
 * UNKNOWN deliberately does NOT mean "unsafe": a monitor that has lost the
 * bus must not immobilise the vehicle. Only BLOCKED is a positive
 * statement that the pack has cut the path or latched a fault.
 */
typedef enum {
    BMS_PACK_UNKNOWN = 0,  /* no fresh sample -- do not act on this */
    BMS_PACK_OK,
    BMS_PACK_BLOCKED,      /* discharge path open, or a protection tripped */
} bms_pack_state_t;

bms_pack_state_t bms_app_pack_state(void);

/*
 * Console keys the monitor does not recognise are offered to this handler,
 * if one is registered. It lets the host firmware add its own commands to
 * the one interactive console in the system without this module knowing
 * anything about them. Return true if the key was consumed.
 */
void bms_app_set_aux_key_handler(bool (*handler)(char c));

/*
 * "Is it safe to stall your loop right now?"
 *
 * The periodic poll is spread over calls and never blocks for long, but a
 * few console commands ('n', 'p', 'x', 'c', 'b') are synchronous bursts of
 * I2C, and every transfer waits MAX17320_I2C_TIMEOUT_MS before giving up.
 * On a dead bus -- precisely when someone starts pressing those keys --
 * that adds up to whole seconds inside one call. In a vehicle that is the
 * loop which stops the motor.
 *
 * So the host registers a predicate here: return false while blocking
 * would be dangerous (on the car: while the drive is enabled), true
 * otherwise. Those commands are then refused with a printed reason instead
 * of running. Everything else -- the dashboard, the JSON stream, the poll,
 * the raw dump, mode switching -- is unaffected, so the console stays
 * usable while driving.
 *
 * Registering nothing means every moment is safe, which is exactly the
 * behaviour of a dedicated bench monitor. The predicate is called from the
 * key handler only, never from an interrupt.
 */
void bms_app_set_block_guard(bool (*is_safe_to_block)(void));

#endif /* BMS_APP_H */
