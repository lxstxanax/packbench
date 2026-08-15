#include "max17320_monitor.h"

/* ------------------------------------------------------------------ *
 * Fixed-point decoding helpers (datasheet Table 15).
 *
 * Each LSB is an exact binary fraction, so every conversion below is an
 * exact integer ratio -- no rounding drift, no floats. The intermediate
 * products are bounded by 65535 * 125 (~8.2e6), comfortably inside int32.
 * ------------------------------------------------------------------ */

/* Voltage type: 0.078125 mV = 25/32 of 0.1 mV. */
static int32_t dec_volt_01mv(uint16_t raw)
{
    return ((int32_t)raw * 25) / 32;
}

/* Batt/PCKP are "Special" type: 0.3125 mV = 25/8 of 0.1 mV, i.e. 4x
 * coarser than the per-cell channels. Using the per-cell LSB here is the
 * classic MAX17320 bug -- it reports the pack at a quarter of its voltage. */
static int32_t dec_pack_01mv(uint16_t raw)
{
    return ((int32_t)raw * 25) / 8;
}

/* Current type: 1.5625 uV/Rsense, signed. In 0.1 mA units that is
 * raw * 125 / (8 * Rsense_mOhm) -- 3.125 per LSB at 5 mOhm. */
static int32_t dec_curr_01ma(uint16_t raw)
{
    return ((int32_t)(int16_t)raw * 125) / (8 * MAX17320_RSENSE_MOHM);
}

/* Capacity type: 5.0 uVh/Rsense -> exactly 1 mAh per LSB at 5 mOhm. */
static int32_t dec_cap_mah(uint16_t raw)
{
    return ((int32_t)raw * 5) / MAX17320_RSENSE_MOHM;
}

/* Percentage type: 1/256 % = 5/128 of 0.1 %. */
static int32_t dec_pct_01(uint16_t raw)
{
    return ((int32_t)raw * 5) / 128;
}

/* Temperature type: 1/256 C, signed. Truncates toward zero on negatives,
 * i.e. -0.09 C reads as 0.0 -- irrelevant at 0.1 C display resolution. */
static int32_t dec_temp_01c(uint16_t raw)
{
    return ((int32_t)(int16_t)raw * 5) / 128;
}

/* Time type: 5.625 s = 45/8 s. */
static uint32_t dec_time_s(uint16_t raw)
{
    return ((uint32_t)raw * 45u) / 8u;
}

/* Power: 1.6 mW per LSB at 5 mOhm, scaling as 1/Rsense. */
static int32_t dec_power_mw(uint16_t raw)
{
    return ((int32_t)(int16_t)raw * 8) / MAX17320_RSENSE_MOHM;
}

static int32_t abs32(int32_t v)
{
    return (v < 0) ? -v : v;
}

/* ------------------------------------------------------------------ *
 * Bit name tables
 * ------------------------------------------------------------------ */

/* ProtStatus and ProtAlrt share D15..D1; only D0 differs. Index 0 = D15. */
static const char *const prot_names[16] = {
    "ChgWDT", "TooHotC", "Full", "TooColdC", "OVP", "OCCP", "Qovflw", "PreqF",
    "Imbalance", "PermFail", "DieHot", "TooHotD", "UVP", "ODCP", "ResDFault", NULL,
};

const char *max17320_prot_bit_name(uint8_t bit, bool is_prot_alrt)
{
    if (bit > 15u) {
        return NULL;
    }
    if (bit == 0u) {
        /* The one bit where the two registers disagree (Tables 49 vs 50). */
        return is_prot_alrt ? "LDet" : "Ship";
    }
    return prot_names[15u - bit];
}

const char *max17320_status_bit_name(uint8_t bit)
{
    switch (bit) {
    case 15: return "PA";     /* protection alert */
    case 14: return "Smx";    /* SOC over threshold */
    case 13: return "Tmx";
    case 12: return "Vmx";
    case 10: return "Smn";
    case  9: return "Tmn";
    case  8: return "Vmn";
    case  7: return "dSOCi";  /* crossed a 1% boundary */
    case  6: return "Imx";
    case  2: return "Imn";
    case  1: return "POR";
    default: return NULL;     /* D11, D5, D4, D3, D0 are don't-care */
    }
}

const char *max17320_permfail_bit_name(uint8_t bit)
{
    switch (bit) {
    case 15: return "PermFail";
    case 14: return "OVPF (severe overvoltage)";
    case 13: return "OTPF (severe overtemperature)";
    case 12: return "CFETFs (charge FET shorted)";
    case 11: return "DFETFs (discharge FET shorted)";
    case 10: return "FETFo (FET stuck open)";
    case  9: return "LDet (leakage)";
    case  8: return "ChksumF / UVPF";
    default: return NULL;     /* D7:D0 is the LeakCurr byte, not a flag */
    }
}

/* ------------------------------------------------------------------ *
 * Init
 * ------------------------------------------------------------------ */

max17320_status_t max17320_monitor_init(max17320_monitor_t *mon, I2C_HandleTypeDef *hi2c)
{
    uint16_t misc_th = 0;
    max17320_status_t st;

    if ((mon == NULL) || (hi2c == NULL)) {
        return MAX17320_ERR_ARG;
    }

    mon->hi2c = hi2c;
    mon->fail_count = 0;
    mon->boot_read_ok = false;
    /* Datasheet default nProtMiscTh = 7A28h -> CurrDet = 2 -> 15 mA. Used
     * until the real value is read, so a failed boot read degrades to the
     * factory deadband instead of to zero (which would make every bit of
     * ADC noise look like charging). */
    mon->curr_det_01ma = 150;

    st = max17320_probe(hi2c);
    if (st != MAX17320_OK) {
        return st;
    }

    /* nProtMiscTh D7:D4 = CurrDet, threshold = (CurrDet + 1) * 5 mA at
     * 5 mOhm, scaling as 1/Rsense like every other current field. */
    st = max17320_read_reg(hi2c, MAX17320_REG_NPROTMISCTH, &misc_th);
    if (st == MAX17320_OK) {
        int32_t curr_det = (int32_t)((misc_th >> 4) & 0x0Fu);
        mon->curr_det_01ma = ((curr_det + 1) * 250) / MAX17320_RSENSE_MOHM;
        mon->boot_read_ok = true;
    }

    /* Provisioned or virgin? nDesignCap is the one register whose factory
     * default (0x0000) cannot be mistaken for a real configuration. */
    if (max17320_read_reg(hi2c, MAX17320_REG_NDESIGNCAP, &mon->n_design_cap) == MAX17320_OK) {
        mon->provisioned = (mon->n_design_cap != 0u);
    }

    return st;
}

/* ------------------------------------------------------------------ *
 * Poll
 * ------------------------------------------------------------------ */

/* Addresses in R_* order. Kept next to the enum so the two cannot drift. */
static const uint16_t POLL_ADDR[R_COUNT] = {
    [R_STATUS]      = MAX17320_REG_STATUS,
    [R_PROTSTATUS]  = MAX17320_REG_PROTSTATUS,
    [R_PROTALRT]    = MAX17320_REG_PROTALRT,
    [R_HPROTCFG2]   = MAX17320_REG_HPROTCFG2,
    [R_STATUS2]     = MAX17320_REG_STATUS2,
    [R_FSTAT]       = MAX17320_REG_FSTAT,
    [R_COMMSTAT]    = MAX17320_REG_COMMSTAT,
    [R_CONFIG]      = MAX17320_REG_CONFIG,
    [R_CONFIG2]     = MAX17320_REG_CONFIG2,
    [R_NBATTSTATUS] = MAX17320_REG_NBATTSTATUS,
    [R_DEVNAME]     = MAX17320_REG_DEVNAME,
    [R_CELL1]       = MAX17320_REG_CELL1,
    [R_CELL2]       = MAX17320_REG_CELL2,
    [R_AVGCELL1]    = MAX17320_REG_AVGCELL1,
    [R_AVGCELL2]    = MAX17320_REG_AVGCELL2,
    [R_VCELL]       = MAX17320_REG_VCELL,
    [R_AVGVCELL]    = MAX17320_REG_AVGVCELL,
    [R_BATT]        = MAX17320_REG_BATT,
    [R_PCKP]        = MAX17320_REG_PCKP,
    [R_CURRENT]     = MAX17320_REG_CURRENT,
    [R_AVGCURRENT]  = MAX17320_REG_AVGCURRENT,
    [R_POWER]       = MAX17320_REG_POWER,
    [R_ICHGTERM]    = MAX17320_REG_ICHGTERM,
    [R_REPSOC]      = MAX17320_REG_REPSOC,
    [R_VFSOC]       = MAX17320_REG_VFSOC,
    [R_AGE]         = MAX17320_REG_AGE,
    [R_FULLSOCTHR]  = MAX17320_REG_FULLSOCTHR,
    [R_REPCAP]      = MAX17320_REG_REPCAP,
    [R_FULLCAPREP]  = MAX17320_REG_FULLCAPREP,
    [R_DESIGNCAP]   = MAX17320_REG_DESIGNCAP,
    [R_CYCLES]      = MAX17320_REG_CYCLES,
    [R_TEMP]        = MAX17320_REG_TEMP,
    [R_AVGTA]       = MAX17320_REG_AVGTA,
    [R_DIETEMP]     = MAX17320_REG_DIETEMP,
    [R_TTE]         = MAX17320_REG_TTE,
    [R_TTF]         = MAX17320_REG_TTF,
    [R_CHARGINGCURRENT] = MAX17320_REG_CHARGINGCURRENT,
    [R_CHARGINGVOLTAGE] = MAX17320_REG_CHARGINGVOLTAGE,
};

/*
 * The safety-critical prefix of a pass, published on its own.
 *
 * These four registers are read first (see the R_* enum) and everything an
 * interlock acts on is derived from them alone, so there is no reason to
 * hold them back until the ~34 remaining reads of voltages, capacities and
 * temperatures have finished. Publishing here makes the FET and fault
 * state visible one pass-worth of reads sooner; the pass rate (250 ms in
 * the host build) still dominates the total latency.
 *
 * Called again from decode_snapshot() at the end of a pass, so there is
 * exactly one place that turns these words into flags.
 */
_Static_assert((R_STATUS == 0) && (R_PROTSTATUS == 1) &&
               (R_PROTALRT == 2) && (R_HPROTCFG2 == 3) &&
               (R_SAFETY_LAST == R_HPROTCFG2),
               "the safety registers must be the first ones a pass reads");

static void publish_safety(const max17320_monitor_t *mon, max17320_snapshot_t *snap)
{
    const uint16_t *r = mon->raw;

    snap->status      = r[R_STATUS];
    snap->prot_status = r[R_PROTSTATUS];
    snap->prot_alrt   = r[R_PROTALRT];
    snap->hprot_cfg2  = r[R_HPROTCFG2];

    /* HProtCfg2 D1:D0 is the only live readback of the FETs there is. */
    snap->chg_fet_on = (r[R_HPROTCFG2] & MAX17320_HPROTCFG2_CHGS) != 0u;
    snap->dis_fet_on = (r[R_HPROTCFG2] & MAX17320_HPROTCFG2_DISS) != 0u;

    snap->full      = (r[R_PROTSTATUS] & MAX17320_PROT_FULL) != 0u;
    snap->ship      = (r[R_PROTSTATUS] & MAX17320_PROTSTATUS_SHIP) != 0u;
    snap->perm_fail = (r[R_PROTSTATUS] & MAX17320_PROT_PERMFAIL) != 0u;
    snap->faulted   = (r[R_PROTSTATUS] & MAX17320_PROT_FAULT_MASK) != 0u;
    snap->por_seen  = (r[R_STATUS] & MAX17320_STATUS_POR) != 0u;

    snap->safety_ms = HAL_GetTick();
    snap->safety_seq++;
}

/* Turns a completed set of raw reads into engineering units. Split out so
 * the resumable and the blocking poll share exactly one decode. */
static void decode_snapshot(max17320_monitor_t *mon, max17320_snapshot_t *snap)
{
    const uint16_t *r = mon->raw;

    publish_safety(mon, snap);   /* re-publish: same words, same flags */

    snap->status2      = r[R_STATUS2];
    snap->fstat        = r[R_FSTAT];
    snap->commstat     = r[R_COMMSTAT];
    snap->config       = r[R_CONFIG];
    snap->config2      = r[R_CONFIG2];
    snap->nbatt_status = r[R_NBATTSTATUS];
    snap->dev_name     = r[R_DEVNAME];

    snap->cell1_01mv      = dec_volt_01mv(r[R_CELL1]);
    snap->cell2_01mv      = dec_volt_01mv(r[R_CELL2]);
    snap->avg_cell1_01mv  = dec_volt_01mv(r[R_AVGCELL1]);
    snap->avg_cell2_01mv  = dec_volt_01mv(r[R_AVGCELL2]);
    snap->vcell_01mv      = dec_volt_01mv(r[R_VCELL]);
    snap->avg_vcell_01mv  = dec_volt_01mv(r[R_AVGVCELL]);
    snap->batt_01mv       = dec_pack_01mv(r[R_BATT]);
    snap->pckp_01mv       = dec_pack_01mv(r[R_PCKP]);
    snap->imbalance_01mv  = abs32(snap->cell1_01mv - snap->cell2_01mv);

    snap->current_01ma     = dec_curr_01ma(r[R_CURRENT]);
    snap->avg_current_01ma = dec_curr_01ma(r[R_AVGCURRENT]);
    snap->power_mw         = dec_power_mw(r[R_POWER]);
    snap->ichgterm_01ma    = dec_curr_01ma(r[R_ICHGTERM]);

    snap->rep_soc_01pct      = dec_pct_01(r[R_REPSOC]);
    snap->vf_soc_01pct       = dec_pct_01(r[R_VFSOC]);
    snap->age_01pct          = dec_pct_01(r[R_AGE]);
    snap->full_soc_thr_01pct = dec_pct_01(r[R_FULLSOCTHR]);
    snap->rep_cap_mah        = dec_cap_mah(r[R_REPCAP]);
    snap->full_cap_mah       = dec_cap_mah(r[R_FULLCAPREP]);
    snap->design_cap_mah     = dec_cap_mah(r[R_DESIGNCAP]);
    snap->cycles_001         = (int32_t)r[R_CYCLES] * 25; /* LSB = 25% of a cycle */

    snap->temp_01c     = dec_temp_01c(r[R_TEMP]);
    snap->avg_temp_01c = dec_temp_01c(r[R_AVGTA]);
    snap->die_temp_01c = dec_temp_01c(r[R_DIETEMP]);

    snap->charging_current_01ma = dec_curr_01ma(r[R_CHARGINGCURRENT]);
    snap->charging_voltage_01mv = dec_volt_01mv(r[R_CHARGINGVOLTAGE]);

    /* --- derived state (the FET and fault flags came from
     *     publish_safety() above) --- */

    /* Sign convention confirmed from nProtMiscTh.CurrDet: current above
     * +CurrDet is charging, below -CurrDet is discharging, in between the
     * gauge itself considers the pack at rest. */
    if (snap->current_01ma > mon->curr_det_01ma) {
        snap->flow = MAX17320_FLOW_CHARGING;
    } else if (snap->current_01ma < -mon->curr_det_01ma) {
        snap->flow = MAX17320_FLOW_DISCHARGING;
    } else {
        snap->flow = MAX17320_FLOW_IDLE;
    }

    /* TTE/TTF only mean anything in the matching direction, and 0xFFFF is
     * the saturation value, not 102 hours of runtime. */
    snap->tte_s     = dec_time_s(r[R_TTE]);
    snap->ttf_s     = dec_time_s(r[R_TTF]);
    snap->tte_valid = (snap->flow == MAX17320_FLOW_DISCHARGING) && (r[R_TTE] != 0xFFFFu);
    snap->ttf_valid = (snap->flow == MAX17320_FLOW_CHARGING)    && (r[R_TTF] != 0xFFFFu);

    snap->hibernating    = (r[R_STATUS2] & MAX17320_STATUS2_HIB) != 0u;
    snap->data_not_ready = (r[R_FSTAT] & MAX17320_FSTAT_DNR) != 0u;

    /* --- can any of this be believed? ---
     * Cell channels in a 2S pack are Cell1 = CELL1-CSP and Cell2 =
     * BATTS-CELL1. An open top tap makes that difference negative, and
     * since the register is unsigned it lands on exactly 0x0000 -- a
     * number that looks like a measurement and is not one. Batt sitting
     * below Cell1 is the same fault seen from the other side. */
    snap->supply_ok = (snap->batt_01mv >= MAX17320_MIN_SUPPLY_01MV);
    snap->cells_plausible =
        !(((snap->cell1_01mv > 20000) && (snap->cell2_01mv == 0)) ||
          ((snap->cell2_01mv > 20000) && (snap->cell1_01mv == 0)) ||
          (snap->batt_01mv < snap->cell1_01mv));
    snap->gauge_trustworthy = mon->provisioned && snap->supply_ok && snap->cells_plausible;

    snap->uptime_ms  = HAL_GetTick();
    snap->seq++;
    snap->last_error = MAX17320_OK;
    mon->fail_count  = 0;
}

bool max17320_monitor_poll_step(max17320_monitor_t *mon, max17320_snapshot_t *snap,
                                uint8_t budget)
{
    if ((mon == NULL) || (snap == NULL) || (mon->hi2c == NULL) || (budget == 0u)) {
        return false;
    }

    for (uint8_t n = 0; n < budget; n++) {
        uint8_t idx = mon->poll_idx;
        max17320_status_t st = max17320_read_reg(mon->hi2c, POLL_ADDR[idx],
                                                 &mon->raw[idx]);
        if (st != MAX17320_OK) {
            /* Abandon the pass rather than mixing samples from before and
             * after a bus glitch. */
            mon->poll_idx = 0;
            mon->fail_count++;
            snap->last_error = st;
            return false;
        }

        mon->poll_idx++;

        /* Fault words and live FET state are in hand: hand them over now
         * rather than at the end of the pass. An interlock reading
         * bms_app_pack_state() sees a protector trip a pass-worth of reads
         * earlier, and on a bus that dies mid-pass these were still read
         * successfully and are worth keeping. */
        if (idx == (uint8_t)R_SAFETY_LAST) {
            publish_safety(mon, snap);
        }

        if (mon->poll_idx >= (uint8_t)R_COUNT) {
            mon->poll_idx = 0;
            decode_snapshot(mon, snap);
            return true;
        }
    }
    return false;
}

max17320_status_t max17320_monitor_poll(max17320_monitor_t *mon, max17320_snapshot_t *snap)
{
    if ((mon == NULL) || (snap == NULL) || (mon->hi2c == NULL)) {
        return MAX17320_ERR_ARG;
    }

    mon->poll_idx = 0;
    while (!max17320_monitor_poll_step(mon, snap, (uint8_t)R_COUNT)) {
        if (snap->last_error != MAX17320_OK) {
            return snap->last_error;
        }
    }
    return MAX17320_OK;
}

/* ------------------------------------------------------------------ *
 * The single permitted write
 * ------------------------------------------------------------------ */

max17320_status_t max17320_clear_alerts(max17320_monitor_t *mon)
{
    uint16_t status = 0;
    max17320_status_t st;

    if ((mon == NULL) || (mon->hi2c == NULL)) {
        return MAX17320_ERR_ARG;
    }

    /* Order is mandated by the datasheet: ProtAlrts must go to 0x0000
     * before Status.PA is cleared, otherwise PA immediately re-asserts
     * from the still-set alert history. */
    st = max17320_write_reg(mon->hi2c, MAX17320_REG_PROTALRT, 0x0000u);
    if (st != MAX17320_OK) {
        return st;
    }

    st = max17320_read_reg(mon->hi2c, MAX17320_REG_STATUS, &status);
    if (st != MAX17320_OK) {
        return st;
    }

    /* Read-modify-write clearing PA only. Never write 0x0000 blindly and
     * never write a 1 into a bit that read 0 -- that can re-arm alerts.
     * Imn/Imx self-clear and are left alone by construction. */
    return max17320_write_reg(mon->hi2c, MAX17320_REG_STATUS,
                              (uint16_t)(status & (uint16_t)~MAX17320_STATUS_PA));
}
