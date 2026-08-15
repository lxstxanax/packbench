#ifndef MAX17320_MONITOR_H
#define MAX17320_MONITOR_H

/*
 * Read-only monitoring layer over the MAX17320 transport in max17320.h:
 * one polling pass fills a max17320_snapshot_t with raw registers plus
 * decoded engineering units and derived pack state.
 *
 * Register addresses, LSB sizes and bit meanings are taken from the
 * MAX17320 datasheet Rev 12 (Tables 15, 45, 49, 50, 51, 47, 68, 92) --
 * the working notes with the per-table citations are in
 * notes/max17320-registers.md. Nothing here is guessed; anything the
 * datasheet leaves undocumented is marked and not displayed as fact.
 *
 * FIXED POINT, NOT FLOAT, ON PURPOSE: newlib-nano's printf drops %f
 * unless the build adds -u _printf_float, which costs several KB and is
 * easy to forget in a generated CMake project. Every decoded field below
 * is a scaled integer, and the dashboard formats the decimal point by
 * hand. Units are in each field name.
 */

#include "max17320.h"
#include <stdbool.h>

/* ---- Physical current-sense resistor on the board being monitored ----
 * Must match the shunt actually populated; all current/capacity/power
 * decoding scales by it (datasheet Table 15: capacity LSB = 5.0uVh/Rsense,
 * current LSB = 1.5625uV/Rsense). The 2S4P board in the altium-bms
 * schematic populates R18 = MFC0603-R005FT5, 5.0mOhm. */
#ifndef MAX17320_RSENSE_MOHM
#define MAX17320_RSENSE_MOHM   5
#endif

/* ---------------- Register addresses (slave 0x36 unless noted) ------- */
#define MAX17320_REG_STATUS            0x000u
#define MAX17320_REG_REPCAP            0x005u
#define MAX17320_REG_REPSOC            0x006u
#define MAX17320_REG_AGE               0x007u
#define MAX17320_REG_CONFIG            0x00Bu
#define MAX17320_REG_FULLCAPREP        0x010u
#define MAX17320_REG_TTE               0x011u
#define MAX17320_REG_FULLSOCTHR        0x013u
#define MAX17320_REG_AVGTA             0x016u
#define MAX17320_REG_CYCLES            0x017u
#define MAX17320_REG_DESIGNCAP         0x018u
#define MAX17320_REG_AVGVCELL          0x019u
#define MAX17320_REG_VCELL             0x01Au
#define MAX17320_REG_TEMP              0x01Bu
#define MAX17320_REG_CURRENT           0x01Cu
#define MAX17320_REG_AVGCURRENT        0x01Du
#define MAX17320_REG_ICHGTERM          0x01Eu
#define MAX17320_REG_TTF               0x020u
#define MAX17320_REG_DEVNAME           0x021u
#define MAX17320_REG_CHARGINGCURRENT   0x028u
#define MAX17320_REG_CHARGINGVOLTAGE   0x02Au
#define MAX17320_REG_DIETEMP           0x034u
#define MAX17320_REG_FSTAT             0x03Du
#define MAX17320_REG_COMMSTAT          0x061u
#define MAX17320_REG_CONFIG2           0x0ABu
#define MAX17320_REG_PROTALRT          0x0AFu
#define MAX17320_REG_STATUS2           0x0B0u
#define MAX17320_REG_POWER             0x0B1u
#define MAX17320_REG_AVGCELL2          0x0D3u
#define MAX17320_REG_AVGCELL1          0x0D4u
#define MAX17320_REG_CELL2             0x0D7u
#define MAX17320_REG_CELL1             0x0D8u
#define MAX17320_REG_PROTSTATUS        0x0D9u
#define MAX17320_REG_BATT              0x0DAu
#define MAX17320_REG_PCKP              0x0DBu
#define MAX17320_REG_HPROTCFG2         0x0F1u
#define MAX17320_REG_VFSOC             0x0FFu
/* NV/shadow half, slave 0x0B -- read-only here, never written */
#define MAX17320_REG_NBATTSTATUS       0x1A8u
#define MAX17320_REG_NDESIGNCAP        0x1B3u
#define MAX17320_REG_NPROTMISCTH       0x1D6u

/* ---------------- Status (0x000), datasheet Table 45 ----------------- */
#define MAX17320_STATUS_PA      (1u << 15)  /* protection alert; see ProtAlrt */
#define MAX17320_STATUS_SMX     (1u << 14)
#define MAX17320_STATUS_TMX     (1u << 13)
#define MAX17320_STATUS_VMX     (1u << 12)
#define MAX17320_STATUS_SMN     (1u << 10)
#define MAX17320_STATUS_TMN     (1u <<  9)
#define MAX17320_STATUS_VMN     (1u <<  8)
#define MAX17320_STATUS_DSOCI   (1u <<  7)
#define MAX17320_STATUS_IMX     (1u <<  6)
#define MAX17320_STATUS_IMN     (1u <<  2)
#define MAX17320_STATUS_POR     (1u <<  1)
/* D11, D5, D4, D3, D0 are "don't care" and may read as anything -- mask
 * them off before comparing or logging. */
#define MAX17320_STATUS_DEFINED_MASK  0xF7C6u

/* ---- ProtStatus (0x0D9) / ProtAlrt (0x0AF), Tables 49 and 50 --------
 * The two registers share bits D15..D2 exactly. They differ only at D0:
 * ProtStatus.Ship (in ship state) vs ProtAlrt.LDet (leakage detected). */
#define MAX17320_PROT_CHGWDT    (1u << 15)
#define MAX17320_PROT_TOOHOTC   (1u << 14)
#define MAX17320_PROT_FULL      (1u << 13)  /* not an error: pack is full */
#define MAX17320_PROT_TOOCOLDC  (1u << 12)
#define MAX17320_PROT_OVP       (1u << 11)
#define MAX17320_PROT_OCCP      (1u << 10)
#define MAX17320_PROT_QOVFLW    (1u <<  9)
#define MAX17320_PROT_PREQF     (1u <<  8)
#define MAX17320_PROT_IMBALANCE (1u <<  7)
#define MAX17320_PROT_PERMFAIL  (1u <<  6)
#define MAX17320_PROT_DIEHOT    (1u <<  5)
#define MAX17320_PROT_TOOHOTD   (1u <<  4)
#define MAX17320_PROT_UVP       (1u <<  3)
#define MAX17320_PROT_ODCP      (1u <<  2)
#define MAX17320_PROT_RESDFAULT (1u <<  1)  /* in the bit table, described nowhere */
#define MAX17320_PROTSTATUS_SHIP (1u << 0)
#define MAX17320_PROTALRT_LDET   (1u << 0)

/* "Something is actually wrong": every fault bit except Ship (a state, not
 * a fault), ResDFault (undocumented -- shown separately, never used to
 * drive a verdict) and Full (normal end of charge). */
#define MAX17320_PROT_FAULT_MASK  0xDEFCu

/* ---- HProtCfg2 (0x0F1), Table 51: the only live FET state readback --- */
#define MAX17320_HPROTCFG2_CHGS  (1u << 0)  /* 1 = charge FET on */
#define MAX17320_HPROTCFG2_DISS  (1u << 1)  /* 1 = discharge FET on */

/* ---- nBattStatus (0x1A8), Table 47: why a permanent fail latched ----- */
#define MAX17320_NBATT_PERMFAIL  (1u << 15)
#define MAX17320_NBATT_OVPF      (1u << 14)
#define MAX17320_NBATT_OTPF      (1u << 13)
#define MAX17320_NBATT_CFETFS    (1u << 12)  /* charge FET shorted */
#define MAX17320_NBATT_DFETFS    (1u << 11)  /* discharge FET shorted */
#define MAX17320_NBATT_FETFO     (1u << 10)  /* either FET stuck open */
#define MAX17320_NBATT_LDET      (1u <<  9)
#define MAX17320_NBATT_CHKSUMF   (1u <<  8)  /* NVM checksum OR undervoltage permfail */

/* ---- Config (0x00B), Table 68 -- only the bits we warn about --------- */
#define MAX17320_CONFIG_COMMSH   (1u << 6)  /* SDA+SCL both low -> shutdown */

/* ---- Status2 (0x0B0) / FStat (0x03D) -------------------------------- */
#define MAX17320_STATUS2_HIB     (1u << 1)
#define MAX17320_FSTAT_DNR       (1u << 0)  /* gauge outputs not valid yet */

typedef enum {
    MAX17320_FLOW_IDLE = 0,
    MAX17320_FLOW_CHARGING,
    MAX17320_FLOW_DISCHARGING,
} max17320_flow_t;

typedef struct {
    /* --- raw registers, kept for the raw dump and for bit tests --- */
    uint16_t status;
    uint16_t prot_status;
    uint16_t prot_alrt;
    uint16_t hprot_cfg2;
    uint16_t status2;
    uint16_t fstat;
    uint16_t commstat;
    uint16_t config;
    uint16_t config2;
    uint16_t nbatt_status;
    uint16_t dev_name;

    /* --- decoded, fixed point (see unit suffix in each name) --- */
    int32_t cell1_01mv;      /* 0.1 mV */
    int32_t cell2_01mv;
    int32_t avg_cell1_01mv;
    int32_t avg_cell2_01mv;
    int32_t vcell_01mv;      /* lowest enabled cell -- the gauge input */
    int32_t avg_vcell_01mv;
    int32_t batt_01mv;       /* whole stack, inside the FETs */
    int32_t pckp_01mv;       /* PACK+ to GND, outside the FETs */
    int32_t imbalance_01mv;  /* |cell1 - cell2| */

    int32_t current_01ma;    /* 0.1 mA, positive = charging */
    int32_t avg_current_01ma;
    int32_t power_mw;
    int32_t ichgterm_01ma;

    int32_t rep_soc_01pct;   /* 0.1 % */
    int32_t vf_soc_01pct;
    int32_t age_01pct;
    int32_t full_soc_thr_01pct;
    int32_t rep_cap_mah;
    int32_t full_cap_mah;
    int32_t design_cap_mah;
    int32_t cycles_001;      /* 0.01 cycles */

    int32_t temp_01c;        /* 0.1 C, signed */
    int32_t avg_temp_01c;
    int32_t die_temp_01c;

    uint32_t tte_s;          /* seconds; 0xFFFF raw saturates at ~102 h */
    uint32_t ttf_s;
    bool     tte_valid;      /* only meaningful while discharging */
    bool     ttf_valid;

    int32_t charging_current_01ma;  /* what the IC prescribes to a charger */
    int32_t charging_voltage_01mv;  /* per cell, active JEITA zone */

    /* --- derived state --- */
    max17320_flow_t flow;
    bool chg_fet_on;
    bool dis_fet_on;
    bool full;               /* protector's own end-of-charge latch */
    bool ship;
    bool hibernating;
    bool data_not_ready;     /* FStat.DNR -- gauge outputs still settling */
    bool perm_fail;
    bool faulted;            /* any real protection fault, see PROT_FAULT_MASK */
    bool por_seen;           /* Status.POR -- gauge restarted since last clear */

    /* --- trust flags: is this reading worth believing at all? ---
     * A gauge with no pack profile, or one running below its minimum
     * supply, still returns perfectly well-formed numbers. They are just
     * meaningless, and a dashboard that renders them as ordinary values
     * is lying to whoever reads it. */
    bool supply_ok;        /* Batt >= the datasheet's 4.2 V minimum VIN */
    bool cells_plausible;  /* no cell channel pinned at exactly 0 V, and
                            * Batt is not below the sum of the cells --
                            * either means an open tap or a miswired stack */
    bool gauge_trustworthy;/* provisioned && supply_ok && cells_plausible;
                            * when false, SOC/capacity/TTE/TTF/Age are junk */

    /* --- housekeeping --- */
    uint32_t          uptime_ms;    /* HAL_GetTick() at capture */
    uint32_t          seq;          /* successful-poll counter */
    max17320_status_t last_error;   /* result of the poll that filled this */

    /* The FET/fault fields above (status, prot_status, prot_alrt,
     * hprot_cfg2 and everything derived from them) are refreshed as soon
     * as those registers have been read, part way into a pass, rather than
     * waiting for the remaining ~34 reads to finish. These two say when
     * that last happened, so a safety interlock can judge the freshness of
     * what it actually uses instead of the freshness of the whole pass.
     * safety_seq == 0 means they have never been read. */
    uint32_t          safety_ms;
    uint32_t          safety_seq;
} max17320_snapshot_t;

/* The registers one pass reads, in the order it reads them. Faults and FET
 * state come first so a pass interrupted by a bus error still captured the
 * values worth having -- and so they can be published early, see
 * R_SAFETY_LAST below. Do not reorder the first four without moving that
 * marker with them; there is a _Static_assert on it in the .c. */
typedef enum {
    R_STATUS = 0, R_PROTSTATUS, R_PROTALRT, R_HPROTCFG2, R_STATUS2, R_FSTAT,
    R_COMMSTAT, R_CONFIG, R_CONFIG2, R_NBATTSTATUS, R_DEVNAME,
    R_CELL1, R_CELL2, R_AVGCELL1, R_AVGCELL2, R_VCELL, R_AVGVCELL, R_BATT, R_PCKP,
    R_CURRENT, R_AVGCURRENT, R_POWER, R_ICHGTERM,
    R_REPSOC, R_VFSOC, R_AGE, R_FULLSOCTHR, R_REPCAP, R_FULLCAPREP, R_DESIGNCAP,
    R_CYCLES, R_TEMP, R_AVGTA, R_DIETEMP, R_TTE, R_TTF,
    R_CHARGINGCURRENT, R_CHARGINGVOLTAGE,
    R_COUNT
} max17320_poll_reg_t;

/* Last of the safety-critical prefix: once this one has been read, the
 * fault words and the live FET state are all in hand and get published to
 * the snapshot immediately, without waiting for the rest of the pass. */
#define R_SAFETY_LAST  R_HPROTCFG2

typedef struct {
    I2C_HandleTypeDef *hi2c;
    int32_t curr_det_01ma;   /* charge/discharge deadband from nProtMiscTh */
    bool    boot_read_ok;
    uint32_t fail_count;     /* consecutive failed polls */

    /* Resumable poll state: raw[] accumulates across calls, poll_idx says
     * how far the current pass got. */
    uint16_t raw[R_COUNT];
    uint8_t  poll_idx;

    /* Read once at init from NVM. nDesignCap == 0 is the giveaway that a
     * part has never been provisioned: on a 2S board with a 5 mOhm shunt
     * the other obvious registers (nPackCfg = 0x0004, nRSense = 0x01F4)
     * are identical to the factory defaults, so a virgin die looks
     * configured everywhere else you would think to check. */
    uint16_t n_design_cap;
    bool     provisioned;
} max17320_monitor_t;

/* Datasheet minimum operating supply (VIN), in 0.1 mV units. Below this
 * the measurements carry no accuracy spec at all. */
#define MAX17320_MIN_SUPPLY_01MV  42000

/* Reads the boot-time constants that never change (nProtMiscTh.CurrDet)
 * and confirms the part answers on both I2C addresses. Safe to retry. */
max17320_status_t max17320_monitor_init(max17320_monitor_t *mon, I2C_HandleTypeDef *hi2c);

/* One full polling pass -> snap. On I2C failure the previously decoded
 * values are left untouched and snap->last_error is set, so the dashboard
 * can keep showing the last good reading with a "STALE" marker instead of
 * flashing zeros. */
max17320_status_t max17320_monitor_poll(max17320_monitor_t *mon, max17320_snapshot_t *snap);

/*
 * Resumable version: reads at most budget registers, then returns.
 *
 * This exists for builds where the monitor shares a main loop with
 * real-time work. A whole pass is ~38 register reads: about 18 ms at
 * 100 kHz, which would wreck a control loop; four per call is under a
 * millisecond.
 *
 * The measured values (voltages, current, gauge outputs) are decoded only
 * when a pass completes, so they never show half of one sample and half of
 * the next. The safety fields are the deliberate exception: the fault
 * words and the live FET state are published the moment R_SAFETY_LAST has
 * been read -- they are self-consistent among themselves, they are what an
 * interlock acts on, and making them wait for ~34 more reads of scenery
 * bought nothing. snap->safety_ms / safety_seq track that publication;
 * snap->uptime_ms / seq still track completed passes.
 *
 * Returns true when a pass completed (the whole snapshot updated), false
 * while one is still in progress or after a bus error (which restarts the
 * pass). A false return therefore does NOT mean nothing was updated.
 */
bool max17320_monitor_poll_step(max17320_monitor_t *mon, max17320_snapshot_t *snap,
                                uint8_t budget);

/* Names for the ProtStatus/ProtAlrt bits, MSB (D15) first, 16 entries.
 * which_d0 picks the correct D0 label ("Ship" vs "LDet"). */
const char *max17320_prot_bit_name(uint8_t bit, bool is_prot_alrt);
/* Name for a Status (0x000) bit, or NULL for the don't-care bits. */
const char *max17320_status_bit_name(uint8_t bit);
/* Name for an nBattStatus permanent-fail bit, or NULL. */
const char *max17320_permfail_bit_name(uint8_t bit);

/*
 * The ONLY write this firmware ever performs, and only when a human asks
 * for it from the CLI. Clears the sticky protection-alert history in the
 * documented order (datasheet Status.PA: "prior to clearing this bit, the
 * ProtAlrts register must first be written to 0x0000"), then clears
 * Status.PA with a read-modify-write that leaves every other bit as read.
 *
 * Touches no NVM, needs no unlock: pages 00h and 0Ah are outside WP1-WP5.
 * It erases fault history, so it is deliberately not called automatically.
 */
max17320_status_t max17320_clear_alerts(max17320_monitor_t *mon);

#endif /* MAX17320_MONITOR_H */
