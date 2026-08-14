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

/* Reads one register into a local, bailing out of the whole pass on the
 * first bus error so a half-updated snapshot is never published. */
#define RD(addr_, dst_)                                                   \
    do {                                                                  \
        st = max17320_read_reg(mon->hi2c, (addr_), &(dst_));               \
        if (st != MAX17320_OK) {                                          \
            mon->fail_count++;                                            \
            snap->last_error = st;                                        \
            return st;                                                    \
        }                                                                 \
    } while (0)

max17320_status_t max17320_monitor_poll(max17320_monitor_t *mon, max17320_snapshot_t *snap)
{
    max17320_status_t st;
    uint16_t status, prot_status, prot_alrt, hprot_cfg2, status2, fstat;
    uint16_t commstat, config, config2, nbatt_status, dev_name;
    uint16_t cell1, cell2, avg_cell1, avg_cell2, vcell, avg_vcell, batt, pckp;
    uint16_t current, avg_current, power, ichgterm;
    uint16_t rep_soc, vf_soc, age, full_soc_thr, rep_cap, full_cap, design_cap, cycles;
    uint16_t temp, avg_ta, die_temp, tte, ttf;
    uint16_t chg_current, chg_voltage;

    if ((mon == NULL) || (snap == NULL) || (mon->hi2c == NULL)) {
        return MAX17320_ERR_ARG;
    }

    /* Faults and FET state first: if the bus dies mid-pass, these are the
     * values worth having captured. */
    RD(MAX17320_REG_STATUS,      status);
    RD(MAX17320_REG_PROTSTATUS,  prot_status);
    RD(MAX17320_REG_PROTALRT,    prot_alrt);
    RD(MAX17320_REG_HPROTCFG2,   hprot_cfg2);
    RD(MAX17320_REG_STATUS2,     status2);
    RD(MAX17320_REG_FSTAT,       fstat);
    RD(MAX17320_REG_COMMSTAT,    commstat);
    RD(MAX17320_REG_CONFIG,      config);
    RD(MAX17320_REG_CONFIG2,     config2);
    RD(MAX17320_REG_NBATTSTATUS, nbatt_status);
    RD(MAX17320_REG_DEVNAME,     dev_name);

    RD(MAX17320_REG_CELL1,       cell1);
    RD(MAX17320_REG_CELL2,       cell2);
    RD(MAX17320_REG_AVGCELL1,    avg_cell1);
    RD(MAX17320_REG_AVGCELL2,    avg_cell2);
    RD(MAX17320_REG_VCELL,       vcell);
    RD(MAX17320_REG_AVGVCELL,    avg_vcell);
    RD(MAX17320_REG_BATT,        batt);
    RD(MAX17320_REG_PCKP,        pckp);

    RD(MAX17320_REG_CURRENT,     current);
    RD(MAX17320_REG_AVGCURRENT,  avg_current);
    RD(MAX17320_REG_POWER,       power);
    RD(MAX17320_REG_ICHGTERM,    ichgterm);

    RD(MAX17320_REG_REPSOC,      rep_soc);
    RD(MAX17320_REG_VFSOC,       vf_soc);
    RD(MAX17320_REG_AGE,         age);
    RD(MAX17320_REG_FULLSOCTHR,  full_soc_thr);
    RD(MAX17320_REG_REPCAP,      rep_cap);
    RD(MAX17320_REG_FULLCAPREP,  full_cap);
    RD(MAX17320_REG_DESIGNCAP,   design_cap);
    RD(MAX17320_REG_CYCLES,      cycles);

    RD(MAX17320_REG_TEMP,        temp);
    RD(MAX17320_REG_AVGTA,       avg_ta);
    RD(MAX17320_REG_DIETEMP,     die_temp);
    RD(MAX17320_REG_TTE,         tte);
    RD(MAX17320_REG_TTF,         ttf);

    RD(MAX17320_REG_CHARGINGCURRENT, chg_current);
    RD(MAX17320_REG_CHARGINGVOLTAGE, chg_voltage);

    /* Whole pass succeeded -- publish it. */
    snap->status       = status;
    snap->prot_status  = prot_status;
    snap->prot_alrt    = prot_alrt;
    snap->hprot_cfg2   = hprot_cfg2;
    snap->status2      = status2;
    snap->fstat        = fstat;
    snap->commstat     = commstat;
    snap->config       = config;
    snap->config2      = config2;
    snap->nbatt_status = nbatt_status;
    snap->dev_name     = dev_name;

    snap->cell1_01mv      = dec_volt_01mv(cell1);
    snap->cell2_01mv      = dec_volt_01mv(cell2);
    snap->avg_cell1_01mv  = dec_volt_01mv(avg_cell1);
    snap->avg_cell2_01mv  = dec_volt_01mv(avg_cell2);
    snap->vcell_01mv      = dec_volt_01mv(vcell);
    snap->avg_vcell_01mv  = dec_volt_01mv(avg_vcell);
    snap->batt_01mv       = dec_pack_01mv(batt);
    snap->pckp_01mv       = dec_pack_01mv(pckp);
    snap->imbalance_01mv  = abs32(snap->cell1_01mv - snap->cell2_01mv);

    snap->current_01ma     = dec_curr_01ma(current);
    snap->avg_current_01ma = dec_curr_01ma(avg_current);
    snap->power_mw         = dec_power_mw(power);
    snap->ichgterm_01ma    = dec_curr_01ma(ichgterm);

    snap->rep_soc_01pct      = dec_pct_01(rep_soc);
    snap->vf_soc_01pct       = dec_pct_01(vf_soc);
    snap->age_01pct          = dec_pct_01(age);
    snap->full_soc_thr_01pct = dec_pct_01(full_soc_thr);
    snap->rep_cap_mah        = dec_cap_mah(rep_cap);
    snap->full_cap_mah       = dec_cap_mah(full_cap);
    snap->design_cap_mah     = dec_cap_mah(design_cap);
    snap->cycles_001         = (int32_t)cycles * 25; /* LSB = 25% of a cycle */

    snap->temp_01c     = dec_temp_01c(temp);
    snap->avg_temp_01c = dec_temp_01c(avg_ta);
    snap->die_temp_01c = dec_temp_01c(die_temp);

    snap->charging_current_01ma = dec_curr_01ma(chg_current);
    snap->charging_voltage_01mv = dec_volt_01mv(chg_voltage);

    /* --- derived state --- */
    snap->chg_fet_on = (hprot_cfg2 & MAX17320_HPROTCFG2_CHGS) != 0u;
    snap->dis_fet_on = (hprot_cfg2 & MAX17320_HPROTCFG2_DISS) != 0u;

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
    snap->tte_s     = dec_time_s(tte);
    snap->ttf_s     = dec_time_s(ttf);
    snap->tte_valid = (snap->flow == MAX17320_FLOW_DISCHARGING) && (tte != 0xFFFFu);
    snap->ttf_valid = (snap->flow == MAX17320_FLOW_CHARGING)    && (ttf != 0xFFFFu);

    snap->full           = (prot_status & MAX17320_PROT_FULL) != 0u;
    snap->ship           = (prot_status & MAX17320_PROTSTATUS_SHIP) != 0u;
    snap->perm_fail      = (prot_status & MAX17320_PROT_PERMFAIL) != 0u;
    snap->faulted        = (prot_status & MAX17320_PROT_FAULT_MASK) != 0u;
    snap->hibernating    = (status2 & MAX17320_STATUS2_HIB) != 0u;
    snap->data_not_ready = (fstat & MAX17320_FSTAT_DNR) != 0u;
    snap->por_seen       = (status & MAX17320_STATUS_POR) != 0u;

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

    return MAX17320_OK;
}

#undef RD

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
