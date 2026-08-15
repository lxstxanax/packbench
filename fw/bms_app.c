#include "bms_app.h"
#include "bms_dashboard.h"
#include "max17320_provision.h"
#include "main.h"

/* main.h pulls in the Nucleo BSP when the project was generated with the
 * board selected, which is where LD2 and the VCP UART come from. */
#if defined(__has_include)
#  if __has_include("stm32g4xx_nucleo.h")
#    include "stm32g4xx_nucleo.h"
#    define BMS_HAS_NUCLEO_BSP 1
#  endif
#endif

/* Bus pins and the names printed for them, for the 'b' line-state check --
 * normal traffic goes through the HAL handle. bms_pins.h is the only place
 * they are defined, so no message can name a pin this build does not use. */
#include "bms_pins.h"

/* In a build that shares the loop with real-time work, one pass is spread
 * over several calls at this many registers each: ~4 reads is well under a
 * millisecond, against a drive watchdog that trips at 500 ms. */
#ifndef BMS_POLL_BUDGET
#define BMS_POLL_BUDGET      4u
#endif

#define POLL_PERIOD_MS     250u
#define RENDER_PERIOD_MS   500u
#define REPROBE_PERIOD_MS 2000u
#define HEARTBEAT_OK_MS    500u
#define HEARTBEAT_BAD_MS   100u

typedef enum {
    MODE_DASHBOARD = 0,
    MODE_JSON,
} app_mode_t;

static max17320_monitor_t  s_mon;
static max17320_snapshot_t s_snap;

static app_mode_t s_mode        = MODE_DASHBOARD;
static bool       s_online      = false;  /* gauge answered the last probe */
static bool       s_await_clear = false;  /* 'c' pressed, waiting for Y/n */
static bool       s_weak_bus    = false;  /* running on internal pull-ups */

/* One-shot output (a register dump, a help screen) used to be followed by
 * HAL_Delay() so a human could read it before the dashboard painted over
 * it. That is fine in a dedicated monitor and fatal next to motor control,
 * where the drive watchdog trips after 500 ms without a serviced packet.
 * So the pause became a deadline: the dashboard does not repaint until it
 * expires, and the loop keeps running throughout. */
static uint32_t   s_hold_until_ms;
static bool       s_redraw_pending;
static uint32_t   s_normal_timing;        /* CubeMX TIMINGR, saved at init */
static bool     (*s_aux_key)(char c);     /* host firmware's own commands */
static bool     (*s_may_block)(void);     /* host's "safe to stall now?" */

static void hold_output(uint32_t ms);

static uint32_t s_next_poll_ms;
static uint32_t s_next_render_ms;
static uint32_t s_next_probe_ms;
static uint32_t s_next_beat_ms;

/* HAL_GetTick() wraps after ~49 days. Comparing (now - deadline) as a
 * signed difference keeps the schedule correct across the wrap, which a
 * plain (now >= deadline) would not. */
static bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

/* Slow blink = talking to the gauge, fast blink = it is not answering.
 * The board BSP owns LD2 on this project (BSP_LED_Init(LED_GREEN) in
 * main.c); the LD2_Pin path is the fallback for a plain CubeMX GPIO
 * project without the Nucleo BSP. */
static void heartbeat(uint32_t now)
{
#if defined(BMS_HAS_NUCLEO_BSP) || defined(LD2_Pin)
    uint32_t period = (s_online && (s_snap.last_error == MAX17320_OK))
                      ? HEARTBEAT_OK_MS : HEARTBEAT_BAD_MS;

    if (due(now, s_next_beat_ms)) {
#if defined(BMS_HAS_NUCLEO_BSP)
        BSP_LED_Toggle(LED_GREEN);
#else
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
#endif
        s_next_beat_ms = now + period;
    }
#else
    (void)now;
#endif
}

/* Keep whatever was just printed readable for ms, without blocking. */
static void hold_output(uint32_t ms)
{
    s_hold_until_ms = HAL_GetTick() + ms;
    s_redraw_pending = true;
}

/*
 * Gate for the console commands that DO block.
 *
 * The automatic poll is spread over calls and never holds the loop for
 * more than a few reads, but the operator commands are still synchronous
 * bursts: every max17320_read_reg() is a transmit plus a receive, each
 * waiting MAX17320_I2C_TIMEOUT_MS before it gives up. On a bus that has
 * just died -- the exact moment someone reaches for these keys -- 'n' is
 * ~20 transfers, i.e. about a second inside one call, and 'p' is worse.
 * That is longer than the car's drive watchdog, so a keystroke could stop
 * the loop that stops the motor.
 *
 * The monitor cannot know when that matters, and must not include a car
 * header to find out, so the host registers a predicate instead. No
 * predicate registered = every moment is safe, which keeps the bench
 * monitor exactly as it was.
 *
 * worst_ms is how long the command can hold the loop if every transfer it
 * makes times out -- printed so the operator sees what they are spared,
 * and stated by each call site so it cannot drift from the code.
 */
static bool may_block(const char *what, uint32_t worst_ms)
{
    if ((s_may_block == NULL) || s_may_block()) {
        return true;
    }

    bms_printf("\r\n-- refused: %s can hold the main loop for up to %lu ms,\r\n",
               what, (unsigned long)worst_ms);
    bms_print("   and the host says this is not a safe moment to stall it\r\n"
              "   (on the car: the drive is live). Stop, then press again.\r\n");
    hold_output(2000);
    return false;
}

/* One register read is an address write plus a data read, and each of the
 * two waits out MAX17320_I2C_TIMEOUT_MS on a dead bus. */
#define BLOCK_MS_PER_REG  (2u * MAX17320_I2C_TIMEOUT_MS)

static void enter_mode(app_mode_t mode)
{
    s_mode = mode;
    if (mode == MODE_DASHBOARD) {
        bms_dashboard_enter();
    } else {
        bms_print("\x1b[?25h\r\n-- JSON stream, one object per sample. "
                  "press d for the dashboard --\r\n");
    }
}

#ifndef BMS_REALTIME_HOST
static void weak_probe(void);
#endif

/* max17320_probe() tries two addresses twice each, max17320_monitor_init()
 * then reads two registers, and a successful probe is followed by one
 * blocking pass over all R_COUNT registers. */
#define BLOCK_MS_PROBE \
    ((4u + (2u * 2u) + ((uint32_t)R_COUNT * 2u)) * MAX17320_I2C_TIMEOUT_MS)

static void try_probe(void)
{
    max17320_status_t st = max17320_monitor_init(&s_mon, s_mon.hi2c);

    s_online = (st == MAX17320_OK);
    if (s_online) {
        /* Prime one snapshot so the first frame is not all zeros. */
        (void)max17320_monitor_poll(&s_mon, &s_snap);
    }
    bms_dashboard_banner(&s_snap, st);

    /* Nothing on the normal bus? Try it on the MCU's own pull-ups before
     * giving up. On a bench where the external resistors are not fitted
     * yet that is the difference between a working dashboard and a blank
     * one, and it is the same check a human would run by hand with 'w'.
     * It announces itself, and 'w' switches back. */
#ifndef BMS_REALTIME_HOST
    if (!s_online && !s_weak_bus) {
        bms_print("falling back to the internal pull-ups to see if the part\r\n"
                  "is there at all...\r\n");
        weak_probe();
    }
#endif

    if (s_online && (s_mode == MODE_DASHBOARD)) {
        bms_dashboard_enter();
    }
}

/*
 * Bus line-state check. "Device does not answer" has two very different
 * causes that look identical from the HAL: nothing is pulling the bus up,
 * or the bus is fine and the gauge is simply unpowered (it runs off the
 * pack). Reading the idle levels tells them apart in one keystroke.
 *
 * Releases the pins from the I2C peripheral, reads them as plain inputs,
 * then re-inits -- HAL_I2C_Init() calls the generated MspInit, which puts
 * the alternate-function config back exactly as CubeMX made it.
 */
static GPIO_PinState read_pin_with(GPIO_TypeDef *port, uint16_t pin, uint32_t pull)
{
    GPIO_InitTypeDef g = {0};

    g.Mode = GPIO_MODE_INPUT;
    g.Pull = pull;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin = pin;
    HAL_GPIO_Init(port, &g);
    HAL_Delay(2);                  /* let the ~40k internal settle the line */
    return HAL_GPIO_ReadPin(port, pin);
}

static void bus_check(void)
{
    GPIO_PinState scl_np, sda_np, scl_pu, sda_pu;

    bms_print("\r\nbus check: releasing SCL/SDA and reading idle levels...\r\n");

    (void)HAL_I2C_DeInit(s_mon.hi2c);

    /* Pass 1 -- floating: shows what the outside world does on its own. */
    scl_np = read_pin_with(BMS_SCL_PORT, BMS_SCL_PIN, GPIO_NOPULL);
    sda_np = read_pin_with(BMS_SDA_PORT, BMS_SDA_PIN, GPIO_NOPULL);

    /* Pass 2 -- with the MCU's own ~40k pull-up. A line that still reads
     * LOW against 40k is genuinely tied to something low-impedance; a line
     * that comes up was merely floating. This is what separates "wire is
     * on a GND pin / shorted" from "the pull-up rail is dead". */
    scl_pu = read_pin_with(BMS_SCL_PORT, BMS_SCL_PIN, GPIO_PULLUP);
    sda_pu = read_pin_with(BMS_SDA_PORT, BMS_SDA_PIN, GPIO_PULLUP);

    bms_printf("             floating   +40k internal pull-up\r\n"
               "  SCL %-8s %-10s %s\r\n"
               "  SDA %-8s %-10s %s\r\n",
               BMS_SCL_NAME,
               (scl_np == GPIO_PIN_SET) ? "HIGH" : "LOW",
               (scl_pu == GPIO_PIN_SET) ? "HIGH" : "LOW",
               BMS_SDA_NAME,
               (sda_np == GPIO_PIN_SET) ? "HIGH" : "LOW",
               (sda_pu == GPIO_PIN_SET) ? "HIGH" : "LOW");

    if ((scl_np == GPIO_PIN_SET) && (sda_np == GPIO_PIN_SET)) {
        bms_print("  -> bus idles high on its own: pull-ups are alive.\r\n"
                  "     Wiring is fine and nothing is answering -- the gauge\r\n"
                  "     runs off the pack, so check the cells are connected.\r\n");
    } else if ((scl_pu == GPIO_PIN_RESET) || (sda_pu == GPIO_PIN_RESET)) {
        bms_print("  -> a line stays LOW even against the internal 40k, so it\r\n"
                  "     is hard-tied low. Either that wire is on the wrong\r\n"
                  "     morpho pin (a GND pin is the usual one) or the pair\r\n"
                  "     is shorted. This build expects " BMS_SCL_NAME " = SCL and "
                  BMS_SDA_NAME " = SDA.\r\n");
    } else {
        bms_print("  -> lines float low but come up against the internal 40k:\r\n"
                  "     nothing is actively pulling the bus up right now.\r\n"
                  "     Either the pull-ups are missing, or they are tied to\r\n"
                  "     the gauge's own AOLDO rail, which is dead until the\r\n"
                  "     pack is connected. CONNECT THE PACK, then press b\r\n"
                  "     again -- if both go HIGH, that was it.\r\n");
    }

    if (HAL_I2C_Init(s_mon.hi2c) != HAL_OK) {
        bms_print("  !! could not re-init I2C, reset the board\r\n");
    }
}

/*
 * "Is the chip alive at all?" probe, for a bench with no pull-up resistors
 * fitted yet.
 *
 * Runs the bus off the STM32's own ~40k internal pull-ups instead of
 * external ones. That is far too weak for 100 kHz -- the rise time swamps
 * the bit period -- so the I2C clock is dropped to ~20 kHz first, where
 * 40k into a short jumper's capacitance settles in time. Not a substitute
 * for real pull-ups, but enough to get an ACK out of a working part.
 *
 * PRESC=15, SCLL=SCLH=255 is the slowest TIMINGR the peripheral can
 * express at 170 MHz PCLK1: (255+1 + 255+1) * 16/170MHz = 48.2 us -> 20.7 kHz.
 */
#define BMS_SLOW_TIMING  0xF0F2FFFFu

#ifndef BMS_REALTIME_HOST
static void apply_internal_pullups(void)
{
    GPIO_InitTypeDef g = {0};

    /* HAL_I2C_Init() ran the generated MspInit, which sets these pins to
     * AF open-drain with GPIO_NOPULL. Re-apply the same alternate function
     * with the internal pull-up switched on. */
    g.Mode = GPIO_MODE_AF_OD;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF4_I2C1;
    g.Pin = BMS_SCL_PIN;
    HAL_GPIO_Init(BMS_SCL_PORT, &g);
    g.Pin = BMS_SDA_PIN;
    HAL_GPIO_Init(BMS_SDA_PORT, &g);
}

static void weak_probe(void)
{
    uint16_t value = 0;
    bool main_ok, nv_ok;

    if (s_weak_bus) {           /* second press: back to the normal config */
        (void)HAL_I2C_DeInit(s_mon.hi2c);
        s_mon.hi2c->Init.Timing = s_normal_timing;
        (void)HAL_I2C_Init(s_mon.hi2c);
        s_weak_bus = false;
        bms_dashboard_set_note(NULL);
        bms_print("\r\nback to the generated 100 kHz config with no internal\r\n"
                  "pull-ups -- external resistors required from here.\r\n");
        return;
    }

    bms_print("\r\nlife check: ~20 kHz on the MCU's internal 40k pull-ups\r\n"
              "(no external resistors needed for this test)\r\n");

    (void)HAL_I2C_DeInit(s_mon.hi2c);
    s_mon.hi2c->Init.Timing = BMS_SLOW_TIMING;
    if (HAL_I2C_Init(s_mon.hi2c) != HAL_OK) {
        bms_print("  !! I2C re-init failed\r\n");
        return;
    }
    apply_internal_pullups();
    HAL_Delay(5);

    main_ok = (HAL_I2C_IsDeviceReady(s_mon.hi2c, MAX17320_HAL_ADDR_MAIN, 5, 200) == HAL_OK);
    nv_ok   = (HAL_I2C_IsDeviceReady(s_mon.hi2c, MAX17320_HAL_ADDR_NV,   5, 200) == HAL_OK);

    bms_printf("  0x36 (main, memory 000h-0FFh) : %s\r\n"
               "  0x0B (NV,   memory 100h-1FFh) : %s\r\n",
               main_ok ? "ACK  <-- alive" : "no answer",
               nv_ok   ? "ACK  <-- alive" : "no answer");

    if (main_ok) {
        if (max17320_read_reg(s_mon.hi2c, MAX17320_REG_DEVNAME, &value) == MAX17320_OK) {
            bms_printf("  DevName 0x021 = 0x%04X  (expect 0x4209/0x420A/0x420B)\r\n",
                       value);
        }
        if (max17320_read_reg(s_mon.hi2c, MAX17320_REG_STATUS, &value) == MAX17320_OK) {
            bms_printf("  Status  0x000 = 0x%04X\r\n", value);
        }

        /* It answers, so stay here rather than dropping back to a config
         * that cannot talk: the bench can measure packs right now. This is
         * a stopgap -- 40k pull-ups mean slow edges and no noise margin --
         * so the dashboard keeps saying so until real resistors are on. */
        s_weak_bus = true;
        bms_dashboard_set_note("WEAK BUS: internal 40k pull-ups, ~20 kHz -- "
                               "fit 4.7k to " BMS_3V3_NAME " for real use");
        bms_print("  -> THE CHIP IS ALIVE.\r\n"
                  "     Staying on the internal pull-ups so you can measure\r\n"
                  "     now; press w again to go back to the normal 100 kHz\r\n"
                  "     config. Fit 4.7k from SCL and SDA to " BMS_3V3_NAME "\r\n"
                  "     when you can -- this mode has no noise margin.\r\n");

        /* Pick up nProtMiscTh.CurrDet and prime a snapshot so the dashboard
         * comes up populated instead of waiting a poll period. */
        if (max17320_monitor_init(&s_mon, s_mon.hi2c) == MAX17320_OK) {
            s_online = true;
            (void)max17320_monitor_poll(&s_mon, &s_snap);
        }
        return;                 /* deliberately skip the restore below */
    } else {
        bms_print("  -> no ACK. That is still not proof the part is dead: with\r\n"
                  "     only 40k pulling the bus up this test is marginal, and\r\n"
                  "     it cannot work at all if the pack is disconnected (the\r\n"
                  "     gauge is powered from BATTP). Order of checks:\r\n"
                  "       1. pack connected to the BMS board?\r\n"
                  "       2. AOLDO (P1.4) vs AGND (P1.5) with a meter -- if the\r\n"
                  "          LDO is up (1.8 V or 3.4 V), the die is alive and\r\n"
                  "          this is a bus problem, not a dead chip\r\n"
                  "       3. fit real 4.7k pull-ups and press b, then p\r\n");
    }

    /* Restore the generated configuration exactly. */
    (void)HAL_I2C_DeInit(s_mon.hi2c);
    s_mon.hi2c->Init.Timing = s_normal_timing;
    if (HAL_I2C_Init(s_mon.hi2c) != HAL_OK) {
        bms_print("  !! could not restore I2C, reset the board\r\n");
    }
}

/*
 * Reads the NV/shadow configuration registers that decide what the gauge
 * believes the pack is, and compares them against the values the 2S4P
 * board is supposed to be provisioned with (from the F7 provisioning
 * repo's max17320_config.h, captured from fertig_11400.INI).
 *
 * Purely a read. It answers one question a replaced or unprovisioned die
 * cannot otherwise be asked: are the capacity, shunt and protection
 * thresholds this pack's, or the factory defaults?
 */
#endif /* !BMS_REALTIME_HOST : weak-bus probe */

/*
 * The subset worth reading back by hand: enough to tell a provisioned die
 * from a virgin one in one screen, without the ~100 reads a full diff
 * costs. ONLY the address and a human note live here -- the expected value
 * and the register name are looked up in max17320_target_config[] at run
 * time, which is the same table the provisioner writes.
 *
 * That is deliberate. This list used to carry its own copies of the
 * expected values, and when MAX17320_MAX_CURRENT_LIMITS raised nIPrtTh1
 * and nODSCTh the copies were left behind: 'n' then reported a correctly
 * provisioned max-current part as WRONG. A value that exists twice will
 * eventually disagree with itself, so now it exists once.
 *
 * The notes stay unit-only for the same reason -- anything derived from
 * MAX17320_RSENSE_MOHM (a raw code, a mAh figure at a given shunt) would
 * be one more copy to drift.
 */
typedef struct {
    uint16_t    addr;
    const char *unit_note;
} nv_check_t;

static const nv_check_t s_nv_check[] = {
    { 0x19C, "charge-termination current" },
    { 0x1A5, "nominal full capacity (13224 mAh)" },
    { 0x1A9, "reported full capacity (11400 mAh)" },
    { 0x1B3, "design capacity (11400 mAh)" },
    { 0x1B5, "cell count / pack options" },
    { 0x1CF, "sense resistor, LSb = 10 uOhm" },
    { 0x1D3, "OCCP / ODCP slow current limits" },
    { 0x1DD, "OC / SC / OD comparator thresholds" },
    { 0x1D0, "undervoltage protection" },
    { 0x1DA, "overvoltage protection" },
};

#define NV_CHECK_COUNT (sizeof(s_nv_check) / sizeof(s_nv_check[0]))

/* The profile entry for an address, or NULL if this build's profile does
 * not carry that register at all. */
static const max17320_reg_t *nv_target_entry(uint16_t addr)
{
    for (size_t i = 0; i < MAX17320_TARGET_CONFIG_COUNT; i++) {
        if (max17320_target_config[i].addr == addr) {
            return &max17320_target_config[i];
        }
    }
    return NULL;
}

static void nv_config_check(void)
{
    size_t mismatches = 0;
    size_t checked = 0;
    size_t i;

    bms_print("\r\nNV config vs the profile this firmware would provision\r\n"
              "  addr   name          read    expect  \r\n");

    for (i = 0; i < NV_CHECK_COUNT; i++) {
        const max17320_reg_t *want = nv_target_entry(s_nv_check[i].addr);
        uint16_t value = 0;

        if (want == NULL) {
            /* Only reachable if someone deletes a register from the target
             * table; say so rather than compare against nothing. */
            bms_printf("  0x%03X  %-12s  not in the target profile\r\n",
                       s_nv_check[i].addr, "?");
            continue;
        }

        if (max17320_read_reg(s_mon.hi2c, want->addr, &value) != MAX17320_OK) {
            bms_printf("  0x%03X  %-12s  read failed\r\n", want->addr, want->name);
            continue;
        }

        checked++;
        if (value == want->value) {
            bms_printf("  0x%03X  %-12s  0x%04X  ok\r\n",
                       want->addr, want->name, value);
        } else {
            mismatches++;
            bms_printf("  0x%03X  %-12s  0x%04X  0x%04X  <-- differs (%s)\r\n",
                       want->addr, want->name, value, want->value,
                       s_nv_check[i].unit_note);
        }
    }

    if (checked == 0u) {
        bms_print("  -> nothing could be read; the gauge is not answering.\r\n");
    } else if (mismatches == 0) {
        bms_print("  -> this die carries the profile this firmware targets.\r\n");
    } else {
        /* One printf per chunk: bms_printf() formats through a 256-byte
         * buffer and drops whatever does not fit, which would have eaten
         * the tail of a single long message. */
        bms_printf("  -> %u of %u registers differ. If this die was replaced,\r\n",
                   (unsigned)mismatches, (unsigned)checked);
        bms_print("     it is running Maxim's factory defaults, not this\r\n"
                  "     pack's profile: capacity, SOC and the current\r\n"
                  "     protection thresholds will all be wrong.\r\n"
                  "     Provision it (" BMS_PROVISION_HINT ") before\r\n"
                  "     trusting any reading.\r\n");
    }
}

/*
 * Arbitrary register read: 'x' then three hex digits, e.g. "x0D8" for
 * Cell1 or "x1B5" for nPackCfg. Addresses >= 0x180 go to the NV slave
 * automatically. Read-only by construction -- there is no write path in
 * this firmware to pair it with.
 *
 * Exists so a hypothesis about the pack can be tested without a rebuild
 * and a reflash, which on a bench with a battery attached is the
 * difference between a minute and ten.
 */
static char   s_hex_buf[3];
static uint8_t s_hex_len;
static bool    s_await_hex;
static uint32_t s_hex_started_ms;

/* A half-typed command must not be able to swallow the next keystroke
 * forever: a host tool that opens the port and sends 'j' to switch to
 * JSON would have that 'j' eaten as the cancel character, and the stream
 * would silently never start. Entry states therefore time out, and ESC
 * drops out of any of them. */
#define BMS_INPUT_TIMEOUT_MS 3000u

static int hex_digit(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'a') && (c <= 'f')) { return c - 'a' + 10; }
    if ((c >= 'A') && (c <= 'F')) { return c - 'A' + 10; }
    return -1;
}

static void handle_hex_input(char c)
{
    int digit = hex_digit(c);

    if (digit < 0) {
        s_await_hex = false;
        s_hex_len = 0;
        bms_print(" -- cancelled\r\n");
        return;
    }

    s_hex_buf[s_hex_len++] = c;
    bms_printf("%c", c);

    if (s_hex_len >= 3u) {
        uint16_t addr = (uint16_t)((hex_digit(s_hex_buf[0]) << 8) |
                                   (hex_digit(s_hex_buf[1]) << 4) |
                                    hex_digit(s_hex_buf[2]));
        uint16_t value = 0;
        max17320_status_t st;

        s_await_hex = false;
        s_hex_len = 0;

        /* Checked again here, not just when 'x' was pressed: the address
         * takes three keystrokes to type, and the vehicle can start moving
         * between the first and the last. */
        if (!may_block("a register read", BLOCK_MS_PER_REG)) {
            return;
        }
        st = max17320_read_reg(s_mon.hi2c, addr, &value);

        if (st == MAX17320_OK) {
            bms_printf("\r\n  0x%03X = 0x%04X  (%u, slave 0x%02X)\r\n",
                       addr, value, (unsigned)value,
                       (addr >= 0x180u) ? MAX17320_ADDR7_NV : MAX17320_ADDR7_MAIN);
        } else {
            bms_printf("\r\n  0x%03X : %s\r\n", addr, max17320_status_str(st));
        }
    }
}

/* ------------------------------------------------------------------ *
 * Provisioning menu
 *
 * Behind its own '!' submenu rather than a top-level key, because one of
 * its entries is the only irreversible action in this firmware and the
 * part allows seven of them for its whole life.
 * ------------------------------------------------------------------ */
#ifndef BMS_REALTIME_HOST
static bool  s_prov_menu;
static bool  s_await_burn;
static char  s_burn_buf[4];
static uint8_t s_burn_len;

static void prov_menu_print(void)
{
    bms_print("\r\n-- provisioning ------------------------------------------\r\n"
              "  the NVM block can be written 7 times for the life of the\r\n"
              "  part; the factory test already used one.\r\n"
              "\r\n"
              "  1  read back the profile registers and diff them (safe)\r\n"
              "  2  write the profile to shadow RAM and verify (safe,\r\n"
              "     volatile -- lost on power cycle, costs nothing)\r\n"
              "  3  how many NVM writes remain (safe)\r\n"
              "  4  COMMIT shadow RAM to NVM -- irreversible, spends one\r\n"
              "  5  restart the gauge model so it reloads the config it is\r\n"
              "     holding now (no NVM write, no cycle spent)\r\n"
              "  q  back to the dashboard\r\n"
              "----------------------------------------------------------\r\n");
}

static void prov_diff(void)
{
    static uint16_t backup[MAX17320_TARGET_CONFIG_COUNT];
    size_t differing = 0;
    max17320_status_t st = max17320_backup_config(s_mon.hi2c, backup,
                                                  MAX17320_TARGET_CONFIG_COUNT);

    if (st != MAX17320_OK) {
        bms_printf("\r\nread failed: %s\r\n", max17320_status_str(st));
        return;
    }

    bms_print("\r\ndifferences between the part and the target profile:\r\n");
    for (size_t i = 0; i < MAX17320_TARGET_CONFIG_COUNT; i++) {
        if (backup[i] != max17320_target_config[i].value) {
            bms_printf("  0x%03X %-14s is 0x%04X, want 0x%04X\r\n",
                       max17320_target_config[i].addr,
                       max17320_target_config[i].name,
                       backup[i], max17320_target_config[i].value);
            differing++;
        }
    }
    bms_printf("  %u of %u registers differ\r\n",
               (unsigned)differing, (unsigned)MAX17320_TARGET_CONFIG_COUNT);
}

static void prov_shadow(void)
{
    static size_t mismatches[16];
    size_t count = 0;
    max17320_status_t st;

    bms_print("\r\nwriting the profile to shadow RAM (volatile, no NVM)...\r\n");
    st = max17320_write_shadow_config(s_mon.hi2c);
    if (st != MAX17320_OK) {
        bms_printf("  write failed: %s\r\n", max17320_status_str(st));
        return;
    }

    st = max17320_verify_shadow_config(s_mon.hi2c, mismatches,
                                       sizeof(mismatches) / sizeof(mismatches[0]),
                                       &count);
    if (st == MAX17320_OK) {
        bms_print("  verify: all registers read back as written.\r\n"
                  "  The gauge is running the correct profile RIGHT NOW --\r\n"
                  "  readings should make sense until the next power cycle.\r\n"
                  "  Check them before spending an NVM write on this.\r\n");
    } else if (st == MAX17320_ERR_MISMATCH) {
        bms_printf("  verify: %u register(s) did not stick:\r\n", (unsigned)count);
        for (size_t i = 0; (i < count) && (i < 16u); i++) {
            size_t idx = mismatches[i];
            bms_printf("    0x%03X %s\r\n", max17320_target_config[idx].addr,
                       max17320_target_config[idx].name);
        }
        bms_print("  DO NOT COMMIT while this is failing.\r\n");
    } else {
        bms_printf("  verify failed: %s\r\n", max17320_status_str(st));
    }
}

static void prov_remaining(void)
{
    uint8_t used = 0, remaining = 0;
    max17320_status_t st = max17320_read_remaining_nvm_updates(s_mon.hi2c,
                                                               &used, &remaining);
    if (st != MAX17320_OK) {
        bms_printf("\r\nread failed: %s\r\n", max17320_status_str(st));
        return;
    }
    bms_printf("\r\nNVM writes used: %u of 7   remaining: %u\r\n",
               (unsigned)used, (unsigned)remaining);
    if (used == 1u) {
        bms_print("  (one used = the factory test only; this part has never\r\n"
                  "   been provisioned)\r\n");
    }
}

static void handle_burn_input(char c)
{
    static const char word[] = "BURN";

    if (c != word[s_burn_len]) {
        s_await_burn = false;
        s_burn_len = 0;
        bms_print(" -- cancelled, nothing was written\r\n");
        return;
    }

    s_burn_buf[s_burn_len++] = c;
    bms_printf("%c", c);

    if (s_burn_len >= 4u) {
        max17320_status_t st;

        s_await_burn = false;
        s_burn_len = 0;
        bms_print("\r\n");
        st = max17320_commit_nvm(s_mon.hi2c, MAX17320_NVM_CONFIRM_TOKEN);
        bms_printf("\r\ncommit: %s\r\n", max17320_status_str(st));
        if (st == MAX17320_OK) {
            bms_print("  profile is in NVM and survives power cycles.\r\n");
            (void)max17320_monitor_init(&s_mon, s_mon.hi2c);
        }
    }
}

static void handle_prov_key(char c)
{
    if (s_await_burn) {
        handle_burn_input(c);
        return;
    }

    switch (c) {
    case '1': prov_diff();      break;
    case '2': prov_shadow();    break;
    case '3': prov_remaining(); break;
    case '5':
        /*
         * Capacity, SOC and Age come from the fuel-gauge model, and the
         * model only reloads its configuration when its firmware restarts
         * -- which is why a shadow write alone leaves RepCap showing the
         * old learned value. This is steps 9-12 of the datasheet sequence
         * on their own: Config2.POR_CMD, wait for it to clear, re-lock.
         * It touches no nonvolatile memory, so it costs no lifetime write
         * and can be repeated freely. Running it after option 2 shows
         * exactly what the committed profile would behave like.
         */
        bms_print("\r\nrestarting the gauge model (no NVM write)...\r\n");
        {
            max17320_status_t st = max17320_finish_post_commit_reset(s_mon.hi2c);
            bms_printf("restart: %s\r\n", max17320_status_str(st));
            if (st == MAX17320_OK) {
                (void)max17320_monitor_init(&s_mon, s_mon.hi2c);
                bms_print("give the gauge a few seconds, then check the "
                          "capacity on the dashboard.\r\n");
            }
        }
        break;
    case '4':
        s_await_burn = true;
        s_burn_len = 0;
        bms_print("\r\nThis spends one of the part's 7 lifetime NVM writes and\r\n"
                  "cannot be undone. Run 2 and 3 first if you have not.\r\n"
                  "Type BURN to go ahead, anything else to cancel: ");
        break;
    case 'q':
    case 'Q':
        s_prov_menu = false;
        bms_print("\r\nleaving provisioning\r\n");
        hold_output(400);
        return;
    default:
        break;
    }

    if (s_prov_menu && !s_await_burn) {
        prov_menu_print();
    }
}
#endif /* !BMS_REALTIME_HOST : provisioning menu */

/*
 * Trip capture.
 *
 * A protection event on a motor inrush lasts microseconds: the fast
 * comparators act far below the 351 ms the current ADC needs, so the
 * normal 4 Hz poll cannot see it and MaxMinCurr never records it either.
 * What does survive is state -- ProtStatus and the live FET bits change,
 * and ProtAlrt latches.
 *
 * So this mode drops everything else and polls just the two registers
 * that change, as fast as the bus allows (~1 kHz at 100 kHz I2C), storing
 * timestamped transitions in RAM. Printing happens afterwards, so the
 * capture loop is never slowed by the UART.
 */
#ifndef BMS_REALTIME_HOST
#define TRIP_EVENTS_MAX 48u

typedef struct {
    uint32_t t_ms;
    uint16_t prot_status;
    uint16_t hprot_cfg2;
    uint16_t prot_alrt;
    int32_t  current_01ma;
} trip_event_t;

static trip_event_t s_trip[TRIP_EVENTS_MAX];
static uint16_t     s_trip_count;

static void trip_capture(void)
{
    uint16_t prot = 0, hprot = 0, last_prot = 0xFFFFu, last_hprot = 0xFFFFu;
    uint32_t t0, polls = 0;
    char c;

    bms_print("\r\n-- trip capture ------------------------------------------\r\n"
              "polling ProtStatus and the live FET bits flat out, logging\r\n"
              "every transition with a timestamp. Start the load now.\r\n"
              "Press any key to stop.\r\n\r\n");

    s_trip_count = 0;
    t0 = HAL_GetTick();

    for (;;) {
        if (bms_getc(&c)) {
            break;
        }

        if (max17320_read_reg(s_mon.hi2c, MAX17320_REG_PROTSTATUS, &prot) != MAX17320_OK) {
            continue;
        }
        if (max17320_read_reg(s_mon.hi2c, MAX17320_REG_HPROTCFG2, &hprot) != MAX17320_OK) {
            continue;
        }
        polls++;

        if ((prot != last_prot) || ((hprot & 0x0003u) != (last_hprot & 0x0003u))) {
            if (s_trip_count < TRIP_EVENTS_MAX) {
                uint16_t alrt = 0, cur = 0;
                trip_event_t *e = &s_trip[s_trip_count++];

                /* Read the slower context only on a transition, so the hot
                 * loop stays as tight as possible. */
                (void)max17320_read_reg(s_mon.hi2c, MAX17320_REG_PROTALRT, &alrt);
                (void)max17320_read_reg(s_mon.hi2c, MAX17320_REG_CURRENT, &cur);

                e->t_ms = HAL_GetTick() - t0;
                e->prot_status = prot;
                e->hprot_cfg2 = hprot;
                e->prot_alrt = alrt;
                e->current_01ma = ((int32_t)(int16_t)cur * 125) / (8 * MAX17320_RSENSE_MOHM);
            }
            last_prot = prot;
            last_hprot = hprot;
        }
    }

    bms_printf("captured %u transition(s) over %lu ms, %lu polls "
               "(%lu polls/s)\r\n\r\n",
               (unsigned)s_trip_count, (unsigned long)(HAL_GetTick() - t0),
               (unsigned long)polls,
               (unsigned long)((polls * 1000u) / ((HAL_GetTick() - t0) | 1u)));

    if (s_trip_count == 0u) {
        bms_print("nothing changed -- either the load never tripped it, or the\r\n"
                  "event was shorter than one poll. ProtAlrt is sticky, so check\r\n"
                  "it with 'r' regardless.\r\n");
        return;
    }

    bms_print("   t_ms  ProtStatus  FETs      ProtAlrt  Current    what\r\n");
    for (uint16_t i = 0; i < s_trip_count; i++) {
        const trip_event_t *e = &s_trip[i];
        char cur_buf[16];

        bms_printf("  %5lu   0x%04X    CHG %s DIS %s   0x%04X  %s A   ",
                   (unsigned long)e->t_ms, e->prot_status,
                   (e->hprot_cfg2 & MAX17320_HPROTCFG2_CHGS) ? "on " : "OFF",
                   (e->hprot_cfg2 & MAX17320_HPROTCFG2_DISS) ? "on " : "OFF",
                   e->prot_alrt,
                   bms_fixed(cur_buf, sizeof(cur_buf), e->current_01ma, 10000, 3));

        if (e->prot_status == 0u) {
            bms_print("clear");
        } else {
            for (uint8_t bit = 16u; bit-- > 0u;) {
                const char *name = max17320_prot_bit_name(bit, false);
                if ((name != NULL) && ((e->prot_status & (uint16_t)(1u << bit)) != 0u)) {
                    bms_printf("%s ", name);
                }
            }
        }
        bms_print("\r\n");
    }
    bms_print("\r\n");
}

#endif /* !BMS_REALTIME_HOST : trip capture */

/* Write ProtAlrt, read Status, write Status: three transfers. */
#define BLOCK_MS_CLEAR   (3u * MAX17320_I2C_TIMEOUT_MS)

static void handle_clear_confirmation(char c)
{
    s_await_clear = false;

    if ((c == 'y') || (c == 'Y')) {
        max17320_status_t st;

        /* Re-checked at the moment of the write: the prompt can sit on
         * screen for as long as the operator takes to answer it. */
        if (!may_block("clearing the alert history", BLOCK_MS_CLEAR)) {
            return;
        }
        st = max17320_clear_alerts(&s_mon);
        bms_printf("\r\nclear alerts: %s\r\n", max17320_status_str(st));
    } else {
        bms_print("\r\ncancelled\r\n");
    }

    hold_output(600);
}

static void handle_key(char c)
{
    /* ESC always returns to the top level, from any prompt or submenu.
     * Without it, a host tool cannot get the firmware into a known state
     * without a power cycle. */
    if (c == 0x1B) {
        bool was_pending = s_await_hex || s_await_clear;

#ifndef BMS_REALTIME_HOST
        was_pending = was_pending || s_await_burn || s_prov_menu;
        s_await_burn = false;
        s_prov_menu = false;
        s_burn_len = 0;
#endif
        if (was_pending) {
            s_await_hex = false;
            s_await_clear = false;
            s_hex_len = 0;
            bms_print("\r\n-- back to top level\r\n");
        }
        return;
    }

    if (s_await_clear) {
        handle_clear_confirmation(c);
        return;
    }
    if (s_await_hex) {
        handle_hex_input(c);
        return;
    }
#ifndef BMS_REALTIME_HOST
    if (s_prov_menu) {
        handle_prov_key(c);
        return;
    }
#endif

    switch (c) {
#ifndef BMS_REALTIME_HOST
    case '!':
        s_prov_menu = true;
        prov_menu_print();
        break;
#endif

    case 'x':
    case 'X':
        /* Refuse before the prompt as well as before the read, so nobody
         * types an address that was never going to be executed. */
        if (!may_block("a register read", BLOCK_MS_PER_REG)) {
            break;
        }
        s_await_hex = true;
        s_hex_len = 0;
        s_hex_started_ms = HAL_GetTick();
        bms_print("\r\nread register 0x");
        break;

    case 'd':
    case 'D':
        enter_mode(MODE_DASHBOARD);
        break;

    case 'j':
    case 'J':
        enter_mode(MODE_JSON);
        break;

    case 'r':
    case 'R':
        bms_dashboard_raw_dump(&s_snap);
        hold_output(1200);
        break;

    case 'c':
    case 'C':
        /* The only write path in the firmware, so it asks first. It wipes
         * the sticky ProtAlrt history -- that history is often the only
         * evidence of what tripped. */
        if (!may_block("clearing the alert history", BLOCK_MS_CLEAR)) {
            break;
        }
        s_await_clear = true;
        bms_print("\r\nclear sticky protection alerts? this erases the fault "
                  "history. [y/N] ");
        break;

    case 'b':
    case 'B':
        /* Not the transfers -- bus_check() makes none. It de-inits I2C,
         * takes the pins away from the peripheral, waits out four settling
         * delays and re-inits, so the pack interlock is blind throughout
         * and any transfer in flight dies with the peripheral. */
        if (!may_block("the bus check (it releases SCL/SDA and re-inits I2C)",
                       20u)) {
            break;
        }
        bus_check();
        hold_output(2500);
        break;

#ifndef BMS_REALTIME_HOST
    case 't':
    case 'T':
        trip_capture();
        hold_output(1500);
        break;
#endif

    case 'n':
    case 'N':
        if (!may_block("the NV config check",
                       (uint32_t)NV_CHECK_COUNT * BLOCK_MS_PER_REG)) {
            break;
        }
        nv_config_check();
        hold_output(4000);
        break;

#ifndef BMS_REALTIME_HOST
    case 'w':
    case 'W':
        weak_probe();
        hold_output(2500);
        break;
#endif

    case 'p':
    case 'P':
        /* The heaviest of the lot: an address probe, the boot reads, and
         * then a whole register pass in one go rather than spread over
         * calls. Only the manual key is gated -- the automatic reconnect
         * in bms_app_run_once() still runs, or a bus glitch while driving
         * would leave the interlock blind for the rest of the trip. */
        if (!may_block("a re-probe (address probe + a full register pass)",
                       BLOCK_MS_PROBE)) {
            break;
        }
        try_probe();
        break;

    case 'h':
    case 'H':
    case '?':
        bms_dashboard_help();
        /* Give the host firmware a chance to append its own keys. Without
         * this the vehicle's steering, speed and bench-drive keys exist but
         * are invisible on the help screen, which is how a key gets lost at
         * exactly the moment someone needs it. */
        if (s_aux_key != NULL) {
            (void)s_aux_key('h');
        }
        hold_output(1500);
        break;

    default:
        if ((s_aux_key != NULL) && s_aux_key(c)) {
            /* consumed by the host firmware */
        }
        break;
    }
}

/* A reading older than this is not evidence about the pack any more. A
 * pass starts every 250 ms and publishes the FET/fault registers within
 * its first few reads, so 1.5 s means several passes have been missed. */
#define BMS_PACK_STALE_MS 1500u

void bms_app_set_aux_key_handler(bool (*handler)(char c))
{
    s_aux_key = handler;
}

void bms_app_set_block_guard(bool (*is_safe_to_block)(void))
{
    s_may_block = is_safe_to_block;
}

bms_pack_state_t bms_app_pack_state(void)
{
    /* safety_seq / safety_ms, not seq / uptime_ms: the FET and fault
     * registers are published as soon as they have been read, part way
     * into a pass, while seq only counts completed passes. Judging
     * freshness by the whole pass would age out a reading that is in fact
     * the newest one taken. */
    if (!s_online || (s_snap.safety_seq == 0u) ||
        ((HAL_GetTick() - s_snap.safety_ms) > BMS_PACK_STALE_MS)) {
        return BMS_PACK_UNKNOWN;
    }

    /* dis_fet_on is the live readback from HProtCfg2, not an inference:
     * the bit is already false by the time it is read, because the
     * protector opened the path in hardware long before. */
    if (!s_snap.dis_fet_on || s_snap.faulted || s_snap.perm_fail) {
        return BMS_PACK_BLOCKED;
    }
    return BMS_PACK_OK;
}

void bms_app_init(I2C_HandleTypeDef *hi2c, UART_HandleTypeDef *huart)
{
    uint32_t now;

    bms_io_init(huart);

    s_mon.hi2c = hi2c;
    s_normal_timing = hi2c->Init.Timing;   /* so the weak-bus mode can undo itself */
    s_snap.last_error = MAX17320_ERR_NOT_PRESENT;

    try_probe();

    now              = HAL_GetTick();
    s_next_poll_ms   = now;
    s_next_render_ms = now + RENDER_PERIOD_MS;
    s_next_probe_ms  = now + REPROBE_PERIOD_MS;
    s_next_beat_ms   = now;
}

void bms_app_run_once(void)
{
    uint32_t now = HAL_GetTick();
    char c;

    /* --- CLI: drain whatever has arrived, never block --- */
    while (bms_getc(&c)) {
        handle_key(c);
        now = HAL_GetTick();
    }

    /* An abandoned half-typed register address expires on its own. */
    if (s_await_hex && ((now - s_hex_started_ms) > BMS_INPUT_TIMEOUT_MS)) {
        s_await_hex = false;
        s_hex_len = 0;
        bms_print(" -- timed out\r\n");
    }

    /* --- poll ---
     * A pass already in progress keeps stepping every call; a new one only
     * starts once the period has elapsed. Each call reads BMS_POLL_BUDGET
     * registers and returns, so the loop is never held for long. */
    if (s_online && ((s_mon.poll_idx != 0u) || due(now, s_next_poll_ms))) {
        if (max17320_monitor_poll_step(&s_mon, &s_snap, BMS_POLL_BUDGET)) {
            /* A full pass over the weak bus at ~20 kHz costs real time, so
             * back the rate off rather than starting the next one on top. */
            s_next_poll_ms = HAL_GetTick() + (s_weak_bus ? 1000u : POLL_PERIOD_MS);
        } else if (s_snap.last_error != MAX17320_OK) {
            /* Three consecutive failures means the pack was unplugged or
             * the bus died -- fall back to probing rather than spamming
             * failed transactions at the poll rate. */
            if (s_mon.fail_count >= 3u) {
                s_online        = false;
                s_next_probe_ms = now + REPROBE_PERIOD_MS;
            }
        }
    }

    /* --- reconnect ---
     *
     * try_probe() is blocking: on a dead bus every transfer burns its full
     * MAX17320_I2C_TIMEOUT_MS, and the host's main loop is stopped for the
     * whole call. That is exactly why the manual probe key is refused while
     * the vehicle is driving -- and this automatic path used to run anyway,
     * which pushed the host's own drive watchdog out by the probe duration
     * while the motor ramp, running from a timer interrupt, kept climbing.
     *
     * So honour the same guard here. Losing the gauge while driving is
     * already handled without a probe: the telemetry interlock sees the
     * reading go stale and drops the drive itself. The reconnect simply
     * waits for a safe moment.
     *
     * The guard is read directly rather than through may_block(), which
     * prints a refusal notice -- correct for a keypress the operator made,
     * wrong for a timer that would repeat it every couple of seconds. */
    if (!s_online && due(now, s_next_probe_ms)) {
        s_next_probe_ms = now + REPROBE_PERIOD_MS;

        if ((s_may_block == NULL) || s_may_block()) {
            try_probe();
        }
    }

    /* --- a one-shot dump is on screen: leave it there, then repaint --- */
    if (s_redraw_pending) {
        if (!due(now, s_hold_until_ms)) {
            heartbeat(now);
            return;
        }
        s_redraw_pending = false;
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        s_next_render_ms = now;
    }

    /* --- render --- */
    if (due(now, s_next_render_ms)) {
        s_next_render_ms = now + RENDER_PERIOD_MS;

        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_render(&s_snap, &s_mon);
        } else if (s_online) {
            bms_dashboard_json(&s_snap, &s_mon);
        } else {
            /* Keep the JSON stream parseable while the gauge is missing. */
            bms_printf("{\"ok\":false,\"error\":\"%s\"}\r\n",
                       max17320_status_str(s_snap.last_error));
        }
    }

    heartbeat(now);
}
