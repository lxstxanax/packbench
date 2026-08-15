/*
 * Host-side test for the fixed-point decoding in max17320_monitor.c.
 *
 * Builds on a PC (no STM32 needed) against the HAL stubs in this
 * directory: HAL_I2C_Master_Transmit records which register the driver
 * asked for, HAL_I2C_Master_Receive answers from a table of raw values
 * chosen so the expected engineering units are exact round numbers.
 *
 * This exercises the real max17320_monitor_poll() path, so it also
 * catches a register address typo'd into the wrong struct field.
 *
 *   cc -I. -Itest -o /tmp/test_decode test/test_decode.c \
 *      max17320.c max17320_monitor.c && /tmp/test_decode
 */

#include "max17320_monitor.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK_EQ(what_, got_, want_)                                        \
    do {                                                                     \
        long g = (long)(got_), w = (long)(want_);                            \
        if (g != w) {                                                        \
            printf("  FAIL %-22s got %ld, want %ld\n", (what_), g, w);       \
            failures++;                                                      \
        } else {                                                             \
            printf("  ok   %-22s %ld\n", (what_), g);                        \
        }                                                                    \
    } while (0)

#define CHECK_TRUE(what_, cond_)                                            \
    do {                                                                     \
        if (!(cond_)) {                                                      \
            printf("  FAIL %-22s expected true\n", (what_));                 \
            failures++;                                                      \
        } else {                                                             \
            printf("  ok   %-22s true\n", (what_));                          \
        }                                                                    \
    } while (0)

#define CHECK_FALSE(what_, cond_)                                           \
    do {                                                                     \
        if (cond_) {                                                         \
            printf("  FAIL %-22s expected false\n", (what_));                \
            failures++;                                                      \
        } else {                                                             \
            printf("  ok   %-22s false\n", (what_));                         \
        }                                                                    \
    } while (0)

/* ---- fake device ------------------------------------------------- */

static uint8_t last_slave7;
static uint8_t last_reg;

/* Flipped by the interlock test below to make the part report its
 * discharge FET open, the way the protector leaves it after a trip. */
static bool fets_open;

/* Raw register values, picked so every decoded result below is exact. */
static uint16_t fake_reg(uint8_t slave7, uint8_t reg)
{
    if (slave7 == MAX17320_ADDR7_NV) {
        switch (reg) {
        case 0xA8: return 0x0000;   /* nBattStatus: no permanent failure */
        case 0xD6: return 0x7A28;   /* nProtMiscTh: CurrDet = 2 -> 15 mA */
        default:   return 0x0000;
        }
    }

    switch (reg) {
    case 0x00: return 0x0002;   /* Status: POR set */
    case 0xD9: return 0x2000;   /* ProtStatus: Full only -> not a fault */
    case 0xAF: return 0x0800;   /* ProtAlrt: OVP in the sticky history */
    case 0xF1: return fets_open ? 0x0000 : 0x0003;  /* HProtCfg2 D1:D0 */
    case 0xB0: return 0x0000;
    case 0x3D: return 0x0000;
    case 0x61: return 0x00F9;   /* CommStat: normal locked value */
    case 0x0B: return 0x0040;   /* Config: COMMSH set */
    case 0xAB: return 0x0000;
    case 0x21: return 0x4209;   /* DevName */

    case 0xD8: return 53760;    /* Cell1 = 4.2000 V   (53760 * 0.078125 mV) */
    case 0xD7: return 53504;    /* Cell2 = 4.1800 V                          */
    case 0xD4: return 53760;
    case 0xD3: return 53504;
    case 0x1A: return 53504;    /* VCell = lowest cell                       */
    case 0x19: return 53504;
    case 0xDA: return 26880;    /* Batt  = 8.4000 V   (0.3125 mV LSB)        */
    case 0xDB: return 26848;    /* PCKP  = 8.3900 V                          */

    case 0x1C: return 3200;     /* Current    = +1000.0 mA (charging)        */
    case 0x1D: return 0xF380;   /* AvgCurrent = -1000.0 mA (as int16)        */
    case 0xB1: return 3200;     /* Power      = 5120 mW                      */
    case 0x1E: return 640;      /* IChgTerm   = 200.0 mA                     */

    case 0x06: return 22400;    /* RepSOC = 87.5 %                           */
    case 0xFF: return 22400;
    case 0x07: return 25344;    /* Age    = 99.0 %                           */
    case 0x13: return 24576;    /* FullSocThr = 96.0 %                       */
    case 0x05: return 9975;     /* RepCap     = 9975 mAh @ 5 mOhm            */
    case 0x10: return 11400;    /* FullCapRep = 11400 mAh                    */
    case 0x18: return 11400;    /* DesignCap                                 */
    case 0x17: return 49;       /* Cycles = 12.25 (25 % per LSB)             */

    case 0x1B: return 6221;     /* Temp    = 24.3 C                          */
    case 0x16: return 6221;
    case 0x34: return 7936;     /* DieTemp = 31.0 C                          */
    case 0x11: return 0xFFFF;   /* TTE saturated                             */
    case 0x20: return 896;      /* TTF = 5040 s = 1h24m                      */

    case 0x28: return 4800;     /* ChargingCurrent = 1500.0 mA               */
    case 0x2A: return 53760;    /* ChargingVoltage = 4.2000 V                */
    default:   return 0x0000;
    }
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t addr,
                                          uint8_t *data, uint16_t len, uint32_t to)
{
    (void)h; (void)to;
    last_slave7 = (uint8_t)(addr >> 1);
    last_reg    = data[0];
    (void)len;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *h, uint16_t addr,
                                         uint8_t *data, uint16_t len, uint32_t to)
{
    uint16_t v = fake_reg((uint8_t)(addr >> 1), last_reg);
    (void)h; (void)to; (void)len;
    data[0] = (uint8_t)(v & 0xFF);      /* LSB first on the wire */
    data[1] = (uint8_t)(v >> 8);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
                                        uint32_t tries, uint32_t to)
{
    (void)h; (void)addr; (void)tries; (void)to;
    return HAL_OK;
}

uint32_t HAL_GetTick(void) { return 1000u; }

int main(void)
{
    max17320_monitor_t  mon;
    max17320_snapshot_t snap;
    I2C_HandleTypeDef   hi2c;

    memset(&mon, 0, sizeof(mon));
    memset(&snap, 0, sizeof(snap));
    memset(&hi2c, 0, sizeof(hi2c));

    printf("max17320_monitor_init:\n");
    CHECK_EQ("init status", max17320_monitor_init(&mon, &hi2c), MAX17320_OK);
    CHECK_EQ("CurrDet (0.1 mA)", mon.curr_det_01ma, 150);   /* (2+1) * 5 mA */

    printf("\nmax17320_monitor_poll:\n");
    CHECK_EQ("poll status", max17320_monitor_poll(&mon, &snap), MAX17320_OK);

    printf("\nvoltages (0.1 mV):\n");
    CHECK_EQ("cell1", snap.cell1_01mv, 42000);          /* 4.2000 V */
    CHECK_EQ("cell2", snap.cell2_01mv, 41800);          /* 4.1800 V */
    CHECK_EQ("imbalance", snap.imbalance_01mv, 200);    /* 20.0 mV  */
    CHECK_EQ("batt", snap.batt_01mv, 84000);            /* 8.4000 V */
    CHECK_EQ("pckp", snap.pckp_01mv, 83900);            /* 8.3900 V */

    printf("\ncurrent / power:\n");
    CHECK_EQ("current (0.1 mA)", snap.current_01ma, 10000);
    CHECK_EQ("avg current", snap.avg_current_01ma, -10000);  /* sign extension */
    CHECK_EQ("power (mW)", snap.power_mw, 5120);
    CHECK_EQ("ichgterm", snap.ichgterm_01ma, 2000);

    printf("\ngauge:\n");
    CHECK_EQ("rep soc (0.1 %%)", snap.rep_soc_01pct, 875);
    CHECK_EQ("age (0.1 %%)", snap.age_01pct, 990);
    CHECK_EQ("rep cap (mAh)", snap.rep_cap_mah, 9975);
    CHECK_EQ("full cap (mAh)", snap.full_cap_mah, 11400);
    CHECK_EQ("cycles (0.01)", snap.cycles_001, 1225);

    printf("\ntemperature (0.1 C):\n");
    CHECK_EQ("temp", snap.temp_01c, 243);
    CHECK_EQ("die temp", snap.die_temp_01c, 310);

    printf("\ncharger targets:\n");
    CHECK_EQ("charging current", snap.charging_current_01ma, 15000);
    CHECK_EQ("charging voltage", snap.charging_voltage_01mv, 42000);

    printf("\nderived state:\n");
    CHECK_EQ("flow", snap.flow, MAX17320_FLOW_CHARGING);
    CHECK_TRUE("chg fet on", snap.chg_fet_on);
    CHECK_TRUE("dis fet on", snap.dis_fet_on);
    CHECK_TRUE("full latched", snap.full);
    CHECK_FALSE("faulted", snap.faulted);          /* Full alone is not a fault */
    CHECK_FALSE("perm fail", snap.perm_fail);
    CHECK_TRUE("por seen", snap.por_seen);
    CHECK_TRUE("ttf valid", snap.ttf_valid);
    CHECK_EQ("ttf seconds", snap.ttf_s, 5040);
    CHECK_FALSE("tte valid (saturated)", snap.tte_valid);

    /*
     * The interlock depends on the FET and fault state reaching the
     * snapshot part way into a pass, not at the end of one: a host reading
     * bms_app_pack_state() must not have to wait for ~34 reads of
     * voltages and capacities to learn that the pack cut the path.
     */
    printf("\nearly publish of the safety registers:\n");
    {
        uint32_t seq_before        = snap.seq;
        uint32_t safety_seq_before = snap.safety_seq;
        int32_t  cell1_before      = snap.cell1_01mv;

        fets_open = true;           /* protector opens the discharge FET */

        /* One short slice, exactly what a real-time host calls per loop:
         * enough to cover Status, ProtStatus, ProtAlrt and HProtCfg2. */
        CHECK_FALSE("pass still in progress",
                    max17320_monitor_poll_step(&mon, &snap, 4));
        CHECK_TRUE("safety fields republished", snap.safety_seq > safety_seq_before);
        CHECK_EQ("completed-pass counter unchanged", snap.seq, seq_before);
        CHECK_FALSE("dis fet seen open mid-pass", snap.dis_fet_on);
        CHECK_FALSE("chg fet seen open mid-pass", snap.chg_fet_on);
        /* ...and the measured half of the snapshot is untouched until the
         * pass that is reading it finishes. */
        CHECK_EQ("cell1 not half-updated", snap.cell1_01mv, cell1_before);

        fets_open = false;
        mon.poll_idx = 0;
    }

    printf("\nbit names:\n");
    CHECK_TRUE("D13 == Full",
               strcmp(max17320_prot_bit_name(13, false), "Full") == 0);
    CHECK_TRUE("ProtStatus D0 == Ship",
               strcmp(max17320_prot_bit_name(0, false), "Ship") == 0);
    CHECK_TRUE("ProtAlrt D0 == LDet",
               strcmp(max17320_prot_bit_name(0, true), "LDet") == 0);
    CHECK_TRUE("Status D1 == POR",
               strcmp(max17320_status_bit_name(1), "POR") == 0);
    CHECK_TRUE("Status D11 unnamed", max17320_status_bit_name(11) == NULL);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
