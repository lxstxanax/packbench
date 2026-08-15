#ifndef BMS_PINS_H
#define BMS_PINS_H

/*
 * The I2C pins the gauge is on, and the names printed for them.
 *
 * SINGLE SOURCE OF TRUTH. Every message in this firmware that names a bus
 * pin -- the 'b' line-state check, the probe-failure banner, any future
 * wiring hint -- must print BMS_SCL_NAME / BMS_SDA_NAME rather than a
 * literal. Two builds share these sources and they are NOT on the same
 * pins:
 *
 *   bench monitor (max17320_gui)  I2C1 on PA15 / PB7   -- the defaults below
 *   car firmware  (car_fw)        I2C3 on PC8  / PC9   -- overridden in its
 *                                                         CMakeLists
 *
 * A console that confidently names the wrong pin is worse than one that
 * names none: on the car build PB7 is the Raspberry Pi's USART1_RX, so
 * anyone acting on a stale "SDA = PB7" message would cut the link to the
 * joystick while chasing a bus fault.
 *
 * The port/pin and the name are one unit -- override all six together, or
 * none of them.
 */

#ifndef BMS_SCL_PORT
#define BMS_SCL_PORT  GPIOA
#define BMS_SCL_PIN   GPIO_PIN_15
#endif
#ifndef BMS_SDA_PORT
#define BMS_SDA_PORT  GPIOB
#define BMS_SDA_PIN   GPIO_PIN_7
#endif

#ifndef BMS_SCL_NAME
#define BMS_SCL_NAME  "PA15 (CN7-17)"
#endif
#ifndef BMS_SDA_NAME
#define BMS_SDA_NAME  "PB7  (CN7-21)"
#endif

/* Where the bus pull-ups are told to go. Same rule: named in messages, so
 * it lives here rather than being spelled out at each print site. Both
 * boards are NUCLEO-G474RE, where CN7 pin 16 is +3V3. */
#ifndef BMS_3V3_NAME
#define BMS_3V3_NAME  "3V3 (CN7-16)"
#endif

#endif /* BMS_PINS_H */
