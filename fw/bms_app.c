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

/* Bus pins, for the 'b' line-state check only -- normal traffic goes
 * through the HAL handle. Defaults match this project: I2C1 on PA15 (SCL)
 * and PB7 (SDA), which is what CubeMX assigned. */
#ifndef BMS_SCL_PORT
#define BMS_SCL_PORT  GPIOA
#define BMS_SCL_PIN   GPIO_PIN_15
#endif
#ifndef BMS_SDA_PORT
#define BMS_SDA_PORT  GPIOB
#define BMS_SDA_PIN   GPIO_PIN_7
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
static uint32_t   s_normal_timing;        /* CubeMX TIMINGR, saved at init */

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

static void weak_probe(void);

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
    if (!s_online && !s_weak_bus) {
        bms_print("falling back to the internal pull-ups to see if the part\r\n"
                  "is there at all...\r\n");
        weak_probe();
    }

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
               "  SCL PA15    %-10s %s\r\n"
               "  SDA PB7     %-10s %s\r\n",
               (scl_np == GPIO_PIN_SET) ? "HIGH" : "LOW",
               (scl_pu == GPIO_PIN_SET) ? "HIGH" : "LOW",
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
                  "     is shorted. Check CN7: 17 = SCL, 19 = GND, 21 = SDA,\r\n"
                  "     and that the row is the odd-numbered column.\r\n");
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
                               "fit 4.7k to 3V3 (CN7-16) for real use");
        bms_print("  -> THE CHIP IS ALIVE.\r\n"
                  "     Staying on the internal pull-ups so you can measure\r\n"
                  "     now; press w again to go back to the normal 100 kHz\r\n"
                  "     config. Fit 4.7k from SCL and SDA to 3V3 (CN7 pin 16)\r\n"
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
typedef struct {
    uint16_t    addr;
    uint16_t    expected;
    const char *name;
    const char *unit_note;
} nv_expect_t;

static const nv_expect_t s_nv_expect[] = {
    { 0x19C, 0x0720, "nIChgTerm",   "charge-termination current" },
    { 0x1A5, 0x33A8, "nFullCapNom", "13224 mAh at 5 mOhm" },
    { 0x1A9, 0x2C88, "nFullCapRep", "11400 mAh at 5 mOhm" },
    { 0x1B3, 0x2C88, "nDesignCap",  "11400 mAh at 5 mOhm" },
    { 0x1B5, 0x0004, "nPackCfg",    "cell count / pack options" },
    { 0x1CF, 0x01F4, "nRSense",     "500 = 5.00 mOhm" },
    { 0x1D3, 0x4B80, "nIPrtTh1",    "OCCP / ODCP current limits" },
    { 0x1DD, 0x0C00, "nODSCTh",     "OC / SC / OD thresholds" },
    { 0x1D0, 0x785B, "nUVPrtTh",    "undervoltage protection" },
    { 0x1DA, 0xB754, "nOVPrtTh",    "overvoltage protection" },
};

static void nv_config_check(void)
{
    size_t mismatches = 0;
    size_t i;

    bms_print("\r\nNV config vs the provisioned 2S4P (11400 mAh, 5 mOhm) profile\r\n"
              "  addr   name          read    expect  \r\n");

    for (i = 0; i < (sizeof(s_nv_expect) / sizeof(s_nv_expect[0])); i++) {
        uint16_t value = 0;

        if (max17320_read_reg(s_mon.hi2c, s_nv_expect[i].addr, &value) != MAX17320_OK) {
            bms_printf("  0x%03X  %-12s  read failed\r\n",
                       s_nv_expect[i].addr, s_nv_expect[i].name);
            continue;
        }

        if (value == s_nv_expect[i].expected) {
            bms_printf("  0x%03X  %-12s  0x%04X  ok\r\n",
                       s_nv_expect[i].addr, s_nv_expect[i].name, value);
        } else {
            mismatches++;
            bms_printf("  0x%03X  %-12s  0x%04X  0x%04X  <-- differs (%s)\r\n",
                       s_nv_expect[i].addr, s_nv_expect[i].name, value,
                       s_nv_expect[i].expected, s_nv_expect[i].unit_note);
        }
    }

    if (mismatches == 0) {
        bms_print("  -> this die carries the provisioned pack profile.\r\n");
    } else {
        bms_printf("  -> %u of %u registers differ. If this die was replaced,\r\n"
                   "     it is running Maxim's factory defaults, not this\r\n"
                   "     pack's profile: capacity, SOC and the current\r\n"
                   "     protection thresholds will all be wrong. Provision it\r\n"
                   "     with the F7 tool (max17320_write_shadow_config, then\r\n"
                   "     verify, then commit) before trusting any reading.\r\n",
                   (unsigned)mismatches,
                   (unsigned)(sizeof(s_nv_expect) / sizeof(s_nv_expect[0])));
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
        max17320_status_t st = max17320_read_reg(s_mon.hi2c, addr, &value);

        s_await_hex = false;
        s_hex_len = 0;

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
        HAL_Delay(400);
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        return;
    default:
        break;
    }

    if (s_prov_menu && !s_await_burn) {
        prov_menu_print();
    }
}

static void handle_clear_confirmation(char c)
{
    s_await_clear = false;

    if ((c == 'y') || (c == 'Y')) {
        max17320_status_t st = max17320_clear_alerts(&s_mon);
        bms_printf("\r\nclear alerts: %s\r\n", max17320_status_str(st));
    } else {
        bms_print("\r\ncancelled\r\n");
    }

    HAL_Delay(600);
    if (s_mode == MODE_DASHBOARD) {
        bms_dashboard_enter();
    }
}

static void handle_key(char c)
{
    /* ESC always returns to the top level, from any prompt or submenu.
     * Without it, a host tool cannot get the firmware into a known state
     * without a power cycle. */
    if (c == 0x1B) {
        if (s_await_hex || s_await_burn || s_await_clear || s_prov_menu) {
            s_await_hex = false;
            s_await_burn = false;
            s_await_clear = false;
            s_prov_menu = false;
            s_hex_len = 0;
            s_burn_len = 0;
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
    if (s_prov_menu) {
        handle_prov_key(c);
        return;
    }

    switch (c) {
    case '!':
        s_prov_menu = true;
        prov_menu_print();
        break;

    case 'x':
    case 'X':
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
        HAL_Delay(1200);
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        break;

    case 'c':
    case 'C':
        /* The only write path in the firmware, so it asks first. It wipes
         * the sticky ProtAlrt history -- that history is often the only
         * evidence of what tripped. */
        s_await_clear = true;
        bms_print("\r\nclear sticky protection alerts? this erases the fault "
                  "history. [y/N] ");
        break;

    case 'b':
    case 'B':
        bus_check();
        HAL_Delay(2500);
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        break;

    case 'n':
    case 'N':
        nv_config_check();
        HAL_Delay(4000);
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        break;

    case 'w':
    case 'W':
        weak_probe();
        HAL_Delay(2500);
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        break;

    case 'p':
    case 'P':
        try_probe();
        break;

    case 'h':
    case 'H':
    case '?':
        bms_dashboard_help();
        HAL_Delay(1500);
        if (s_mode == MODE_DASHBOARD) {
            bms_dashboard_enter();
        }
        break;

    default:
        break;
    }
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

    /* --- poll --- */
    if (s_online && due(now, s_next_poll_ms)) {
        /* A full pass is ~35 word reads; at the weak-bus mode's ~20 kHz
         * that is around 150 ms of bus time, so back the rate off rather
         * than starting the next pass on top of the previous one. */
        s_next_poll_ms = now + (s_weak_bus ? 1000u : POLL_PERIOD_MS);

        if (max17320_monitor_poll(&s_mon, &s_snap) != MAX17320_OK) {
            /* Three consecutive failures means the pack was unplugged or
             * the bus died -- fall back to probing rather than spamming
             * failed transactions at the poll rate. */
            if (s_mon.fail_count >= 3u) {
                s_online        = false;
                s_next_probe_ms = now + REPROBE_PERIOD_MS;
            }
        }
    }

    /* --- reconnect --- */
    if (!s_online && due(now, s_next_probe_ms)) {
        s_next_probe_ms = now + REPROBE_PERIOD_MS;
        try_probe();
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
