#include "bms_dashboard.h"
#include "bms_io.h"
#include "bms_pins.h"   /* the only place a bus pin may be named from */

/* ANSI: home the cursor and erase each line as it is rewritten, rather
 * than clearing the whole screen every frame -- a full clear makes the
 * dashboard strobe on a 115200 link. */
#define ANSI_HOME     "\x1b[H"
#define ANSI_CLS      "\x1b[2J\x1b[H"
#define ANSI_EOL      "\x1b[K\r\n"
#define ANSI_HIDE_CUR "\x1b[?25l"
#define ANSI_SHOW_CUR "\x1b[?25h"

#define C_RST  "\x1b[0m"
#define C_DIM  "\x1b[2m"
#define C_BLD  "\x1b[1m"
#define C_RED  "\x1b[31;1m"
#define C_GRN  "\x1b[32;1m"
#define C_YEL  "\x1b[33;1m"
#define C_CYN  "\x1b[36m"

#define RULE "--------------------------------------------------------------------------"

/* Footer hint. 'w' (and the rest of the bench-only keys) do not exist in a
 * real-time build, so that build must not advertise them. */
#ifdef BMS_REALTIME_HOST
#define KEY_HINT "keys: [d]ash [j]son [r]aw [n]vcfg [x]reg [b]us [c]lear [p]robe [h]elp"
#else
#define KEY_HINT "keys: [d]ash [j]son [r]aw [n]vcfg [x]reg [b]us [w]life [c]lear [p]robe [h]elp"
#endif

static const char *flow_text(max17320_flow_t f)
{
    switch (f) {
    case MAX17320_FLOW_CHARGING:    return C_GRN ">> CHARGING   " C_RST;
    case MAX17320_FLOW_DISCHARGING: return C_YEL "<< DISCHARGING" C_RST;
    default:                        return C_DIM "== IDLE       " C_RST;
    }
}

/* 20-cell bar; SOC arrives in 0.1 % units. */
static void soc_bar(char *buf, size_t n, int32_t soc_01pct)
{
    const size_t width = 20u;
    size_t filled, i, k = 0u;

    if (n < (width + 3u)) {
        if (n > 0u) { buf[0] = '\0'; }
        return;
    }
    if (soc_01pct < 0)    { soc_01pct = 0; }
    if (soc_01pct > 1000) { soc_01pct = 1000; }

    filled = (size_t)(((int32_t)width * soc_01pct) / 1000);

    buf[k++] = '[';
    for (i = 0u; i < width; i++) {
        buf[k++] = (i < filled) ? '#' : '-';
    }
    buf[k++] = ']';
    buf[k]   = '\0';
}

/* Prints the set bits of a ProtStatus/ProtAlrt word by name. Returns the
 * number printed so the caller can say "none". */
static int print_prot_bits(uint16_t word, bool is_prot_alrt, uint16_t highlight_mask)
{
    int printed = 0;
    uint8_t bit;

    for (bit = 16u; bit-- > 0u;) {
        const char *name;

        if ((word & (uint16_t)(1u << bit)) == 0u) {
            continue;
        }
        name = max17320_prot_bit_name(bit, is_prot_alrt);
        if (name == NULL) {
            continue;
        }
        bms_printf("%s%s%s ",
                   ((1u << bit) & highlight_mask) ? C_RED : C_YEL,
                   name, C_RST);
        printed++;
    }
    return printed;
}

static const char *s_note = NULL;

void bms_dashboard_set_note(const char *note)
{
    s_note = note;
}

void bms_dashboard_enter(void)
{
    bms_print(ANSI_CLS ANSI_HIDE_CUR);
}

void bms_dashboard_render(const max17320_snapshot_t *s, const max17320_monitor_t *mon)
{
    char b1[24], b2[24], b3[24], b4[24];
    char bar[32];
    bool stale;
    uint32_t age_ms;

    if ((s == NULL) || (mon == NULL)) {
        return;
    }

    age_ms = HAL_GetTick() - s->uptime_ms;
    stale  = (s->last_error != MAX17320_OK) || (age_ms > 2000u);

    bms_print(ANSI_HOME);

    bms_printf("%s== MAX17320 BMS ==%s  2S4P  Rsense %d.0 mOhm  dev 0x%04X%s",
               C_BLD, C_RST, MAX17320_RSENSE_MOHM, s->dev_name, ANSI_EOL);

    if (stale) {
        bms_printf("%s  *** STALE: %s (last good read %lu ms ago) ***%s%s",
                   C_RED, max17320_status_str(s->last_error),
                   (unsigned long)age_ms, C_RST, ANSI_EOL);
    } else if (s_note != NULL) {
        bms_printf("%s  %s%s%s", C_YEL, s_note, C_RST, ANSI_EOL);
    } else {
        bms_print(ANSI_EOL);
    }

    /* Why a reading cannot be trusted, said out loud. The gauge returns
     * well-formed numbers in all of these states; printing them plainly
     * would be the dashboard's own lie, not the chip's. */
    if (!mon->provisioned) {
        bms_printf("%s  !! GAUGE NOT PROVISIONED (nDesignCap = 0): capacity, SOC, "
                   "TTE/TTF and Age are meaningless.%s%s", C_RED, C_RST, ANSI_EOL);
        bms_printf("%s     Load the pack profile (" BMS_PROVISION_HINT
                   ") before believing any of it.%s%s", C_RED, C_RST, ANSI_EOL);
    }
    if (!s->cells_plausible) {
        bms_printf("%s  !! CELL WIRING: a cell channel reads exactly 0 V. In 2S, "
                   "Cell2 = BATTS - CELL1,%s%s", C_RED, C_RST, ANSI_EOL);
        bms_printf("%s     so an open top tap goes negative and clamps to zero. "
                   "Check J1 / the top group.%s%s", C_RED, C_RST, ANSI_EOL);
    }
    if (!s->supply_ok) {
        bms_printf("%s  !! SUPPLY %s V is under the 4.2 V datasheet minimum: "
                   "measurements are out of spec%s%s",
                   C_RED, bms_fixed(b1, sizeof(b1), s->batt_01mv, 10000, 3),
                   C_RST, ANSI_EOL);
    }

    /* ---- voltages ---- */
    bms_printf(" PACK    %s V " C_DIM "(BATT, inside FETs)" C_RST
               "   PACK+   %s V " C_DIM "(PCKP)" C_RST "%s",
               bms_fixed(b1, sizeof(b1), s->batt_01mv, 10000, 3),
               bms_fixed(b2, sizeof(b2), s->pckp_01mv, 10000, 3), ANSI_EOL);

    if (s->cells_plausible) {
        bms_printf(" CELL1   %s V    CELL2   %s V    IMBALANCE %s mV%s",
                   bms_fixed(b1, sizeof(b1), s->cell1_01mv, 10000, 4),
                   bms_fixed(b2, sizeof(b2), s->cell2_01mv, 10000, 4),
                   bms_fixed(b3, sizeof(b3), s->imbalance_01mv, 10, 1), ANSI_EOL);
    } else {
        /* Do not offer an imbalance figure computed from a clamped zero --
         * it would read as a 4 V mismatch between two live cells. */
        bms_printf(" CELL1   %s V    CELL2   %s%s V (clamped)%s    IMBALANCE %sn/a%s%s",
                   bms_fixed(b1, sizeof(b1), s->cell1_01mv, 10000, 4),
                   C_RED, bms_fixed(b2, sizeof(b2), s->cell2_01mv, 10000, 4), C_RST,
                   C_DIM, C_RST, ANSI_EOL);
    }

    /* ---- current / power ---- */
    bms_printf(" CURRENT %s A  %s  AVG %s A   POWER %s W%s",
               bms_fixed(b1, sizeof(b1), s->current_01ma, 10000, 3),
               flow_text(s->flow),
               bms_fixed(b2, sizeof(b2), s->avg_current_01ma, 10000, 3),
               bms_fixed(b3, sizeof(b3), s->power_mw, 1000, 2), ANSI_EOL);

    /* ---- gauge ---- *
     * Everything on these two lines is a model output, not a measurement:
     * it is computed from the NV profile, so with no profile loaded it is
     * arithmetic on nothing. Temperature is a real ADC reading and stays. */
    if (s->gauge_trustworthy) {
        soc_bar(bar, sizeof(bar), s->rep_soc_01pct);
        bms_printf(" SOC     %s %%  %s  %ld / %ld mAh%s",
                   bms_fixed(b1, sizeof(b1), s->rep_soc_01pct, 10, 1), bar,
                   (long)s->rep_cap_mah, (long)s->full_cap_mah, ANSI_EOL);
        bms_printf(" TEMP    %s C     DIE %s C     AGE %s %%    CYCLES %s%s",
                   bms_fixed(b1, sizeof(b1), s->temp_01c, 10, 1),
                   bms_fixed(b2, sizeof(b2), s->die_temp_01c, 10, 1),
                   bms_fixed(b3, sizeof(b3), s->age_01pct, 10, 1),
                   bms_fixed(b4, sizeof(b4), s->cycles_001, 100, 2), ANSI_EOL);
    } else {
        bms_printf(" SOC     %s--%s  %s(gauge output suppressed, see above; raw "
                   "would read %s %%)%s%s",
                   C_DIM, C_RST, C_DIM,
                   bms_fixed(b1, sizeof(b1), s->rep_soc_01pct, 10, 1), C_RST, ANSI_EOL);
        bms_printf(" TEMP    %s C     DIE %s C     AGE %s--%s    CYCLES %s--%s%s",
                   bms_fixed(b1, sizeof(b1), s->temp_01c, 10, 1),
                   bms_fixed(b2, sizeof(b2), s->die_temp_01c, 10, 1),
                   C_DIM, C_RST, C_DIM, C_RST, ANSI_EOL);
    }

    if (!s->gauge_trustworthy) {
        bms_printf(" %sTIME TO FULL/EMPTY   -- (needs a valid capacity profile)%s%s",
                   C_DIM, C_RST, ANSI_EOL);
    } else if (s->ttf_valid) {
        bms_printf(" TIME TO FULL   %luh%02lum%s",
                   (unsigned long)(s->ttf_s / 3600u),
                   (unsigned long)((s->ttf_s % 3600u) / 60u), ANSI_EOL);
    } else if (s->tte_valid) {
        bms_printf(" TIME TO EMPTY  %luh%02lum%s",
                   (unsigned long)(s->tte_s / 3600u),
                   (unsigned long)((s->tte_s % 3600u) / 60u), ANSI_EOL);
    } else {
        bms_printf(" " C_DIM "TIME TO FULL/EMPTY   n/a at rest" C_RST "%s", ANSI_EOL);
    }

    /* The IC computes these for an external charger; they move with the
     * JEITA temperature zone and the step-charge stage. */
    bms_printf(" CHARGER TARGET  %s A @ %s V/cell%s",
               bms_fixed(b1, sizeof(b1), s->charging_current_01ma, 10000, 3),
               bms_fixed(b2, sizeof(b2), s->charging_voltage_01mv, 10000, 3), ANSI_EOL);

    bms_printf(C_DIM RULE C_RST "%s", ANSI_EOL);

    /* ---- FET state: the only live readback is HProtCfg2 D1:D0 ---- */
    bms_printf(" FETS    CHG [%s]   DIS [%s]%s",
               s->chg_fet_on ? C_GRN " ON " C_RST : C_RED "OFF!" C_RST,
               s->dis_fet_on ? C_GRN " ON " C_RST : C_RED "OFF!" C_RST, ANSI_EOL);

    /* ---- one-line verdict ---- */
    bms_print(" STATE   ");
    if (s->perm_fail) {
        bms_print(C_RED "PERMANENT FAILURE - pack latched off, see FAULTS" C_RST);
    } else if (s->faulted) {
        bms_print(C_RED "PROTECTION TRIPPED" C_RST);
    } else if (s->ship) {
        bms_print(C_YEL "SHIP MODE - FETs open, waiting for wake-up" C_RST);
    } else if (s->data_not_ready) {
        bms_print(C_YEL "gauge settling (FStat.DNR)" C_RST);
    } else if (s->full) {
        bms_print(C_GRN "FULL - charge complete" C_RST);
    } else {
        bms_print(C_GRN "OK" C_RST);
    }
    if (s->hibernating) {
        bms_print(C_DIM "  [hibernate]" C_RST);
    }
    bms_print(ANSI_EOL);

    /* ---- live faults ---- */
    bms_print(" FAULTS  ");
    if (print_prot_bits(s->prot_status, false, MAX17320_PROT_FAULT_MASK) == 0) {
        bms_print(C_GRN "none" C_RST);
    }
    bms_print(ANSI_EOL);

    /* ---- sticky history ---- */
    bms_print(" ALERTS  ");
    if (print_prot_bits(s->prot_alrt, true, MAX17320_PROT_FAULT_MASK) == 0) {
        bms_print(C_GRN "none" C_RST);
    } else {
        bms_print(C_DIM "(sticky - press c to clear)" C_RST);
    }
    bms_print(ANSI_EOL);

    /* ---- permanent-fail detail, only when it actually latched ---- */
    if (s->perm_fail || ((s->nbatt_status & MAX17320_NBATT_PERMFAIL) != 0u)) {
        uint8_t bit;
        bms_print(C_RED " PERMFAIL " C_RST);
        for (bit = 16u; bit-- > 8u;) {
            const char *name = max17320_permfail_bit_name(bit);
            if ((name != NULL) && ((s->nbatt_status & (uint16_t)(1u << bit)) != 0u)) {
                bms_printf(C_RED "%s  " C_RST, name);
            }
        }
        bms_print(ANSI_EOL);
    }

    /* ---- gauge-side flags worth surfacing ---- */
    if (s->por_seen || ((s->status & MAX17320_STATUS_PA) != 0u)) {
        bms_print(" GAUGE   ");
        if (s->por_seen) {
            bms_print(C_YEL "POR (gauge restarted) " C_RST);
        }
        if ((s->status & MAX17320_STATUS_PA) != 0u) {
            bms_print(C_YEL "PA (protection alert pending) " C_RST);
        }
        bms_print(ANSI_EOL);
    }

    /* COMMSH turns "both bus lines stuck low" into a pack shutdown. Worth
     * knowing about before debugging a wiring fault with the pack live. */
    if ((s->config & MAX17320_CONFIG_COMMSH) != 0u) {
        bms_printf(" " C_YEL "NOTE" C_RST "    Config.COMMSH=1: holding SDA+SCL low will "
                   "shut the pack down%s", ANSI_EOL);
    }

    bms_printf(C_DIM RULE C_RST "%s", ANSI_EOL);
    bms_printf(C_DIM " seq %lu  poll_err %lu  age %lu ms   " KEY_HINT C_RST "%s",
               (unsigned long)s->seq, (unsigned long)mon->fail_count,
               (unsigned long)age_ms, ANSI_EOL);
}

void bms_dashboard_json(const max17320_snapshot_t *s, const max17320_monitor_t *mon)
{
    char b1[24], b2[24], b3[24], b4[24];
    bool mon_provisioned;

    if ((s == NULL) || (mon == NULL)) {
        return;
    }
    mon_provisioned = mon->provisioned;

    bms_printf("{\"t\":%lu,\"seq\":%lu,\"ok\":%s",
               (unsigned long)s->uptime_ms, (unsigned long)s->seq,
               (s->last_error == MAX17320_OK) ? "true" : "false");

    bms_printf(",\"v_pack\":%s,\"v_pckp\":%s,\"cells\":[%s,%s]",
               bms_fixed(b1, sizeof(b1), s->batt_01mv, 10000, 4),
               bms_fixed(b2, sizeof(b2), s->pckp_01mv, 10000, 4),
               bms_fixed(b3, sizeof(b3), s->cell1_01mv, 10000, 4),
               bms_fixed(b4, sizeof(b4), s->cell2_01mv, 10000, 4));

    bms_printf(",\"i\":%s,\"i_avg\":%s,\"p\":%s",
               bms_fixed(b1, sizeof(b1), s->current_01ma, 10000, 4),
               bms_fixed(b2, sizeof(b2), s->avg_current_01ma, 10000, 4),
               bms_fixed(b3, sizeof(b3), s->power_mw, 1000, 3));

    /* Model outputs go out as null when the profile they are computed from
     * is missing or the pack is miswired, so a host GUI cannot plot junk
     * as though it were data. The trust flags travel with them. */
    if (s->gauge_trustworthy) {
        bms_printf(",\"soc\":%s,\"cap_mah\":%ld,\"full_mah\":%ld,\"age\":%s,\"cycles\":%s",
                   bms_fixed(b1, sizeof(b1), s->rep_soc_01pct, 10, 1),
                   (long)s->rep_cap_mah, (long)s->full_cap_mah,
                   bms_fixed(b2, sizeof(b2), s->age_01pct, 10, 1),
                   bms_fixed(b3, sizeof(b3), s->cycles_001, 100, 2));
    } else {
        bms_print(",\"soc\":null,\"cap_mah\":null,\"full_mah\":null,"
                  "\"age\":null,\"cycles\":null");
    }

    bms_printf(",\"trustworthy\":%s,\"provisioned\":%s,\"supply_ok\":%s,"
               "\"cells_plausible\":%s",
               s->gauge_trustworthy ? "true" : "false",
               mon_provisioned ? "true" : "false",
               s->supply_ok ? "true" : "false",
               s->cells_plausible ? "true" : "false");

    bms_printf(",\"temp\":%s,\"die_temp\":%s",
               bms_fixed(b1, sizeof(b1), s->temp_01c, 10, 1),
               bms_fixed(b2, sizeof(b2), s->die_temp_01c, 10, 1));

    bms_printf(",\"flow\":\"%s\",\"chg_fet\":%s,\"dis_fet\":%s",
               (s->flow == MAX17320_FLOW_CHARGING)    ? "charging" :
               (s->flow == MAX17320_FLOW_DISCHARGING) ? "discharging" : "idle",
               s->chg_fet_on ? "true" : "false",
               s->dis_fet_on ? "true" : "false");

    bms_printf(",\"full\":%s,\"ship\":%s,\"faulted\":%s,\"permfail\":%s",
               s->full      ? "true" : "false",
               s->ship      ? "true" : "false",
               s->faulted   ? "true" : "false",
               s->perm_fail ? "true" : "false");

    bms_printf(",\"tte_s\":%lu,\"ttf_s\":%lu",
               (unsigned long)(s->tte_valid ? s->tte_s : 0u),
               (unsigned long)(s->ttf_valid ? s->ttf_s : 0u));

    /* Raw fault words too: a host GUI can decode bits the firmware does
     * not name, and they make bug reports unambiguous. */
    bms_printf(",\"status\":%u,\"prot_status\":%u,\"prot_alrt\":%u,\"nbatt_status\":%u}\r\n",
               (unsigned)s->status, (unsigned)s->prot_status,
               (unsigned)s->prot_alrt, (unsigned)s->nbatt_status);
}

void bms_dashboard_raw_dump(const max17320_snapshot_t *s)
{
    if (s == NULL) {
        return;
    }
    bms_print("\r\n-- raw registers ------------------------------\r\n");
    bms_printf("  0x000 Status       0x%04X\r\n", s->status);
    bms_printf("  0x0D9 ProtStatus   0x%04X\r\n", s->prot_status);
    bms_printf("  0x0AF ProtAlrt     0x%04X\r\n", s->prot_alrt);
    bms_printf("  0x0F1 HProtCfg2    0x%04X   (D0 CHGs=%u, D1 DISs=%u)\r\n",
               s->hprot_cfg2,
               (unsigned)(s->hprot_cfg2 & 1u), (unsigned)((s->hprot_cfg2 >> 1) & 1u));
    bms_printf("  0x0B0 Status2      0x%04X\r\n", s->status2);
    bms_printf("  0x03D FStat        0x%04X\r\n", s->fstat);
    bms_printf("  0x061 CommStat     0x%04X   (0x00F9 = normally locked)\r\n", s->commstat);
    bms_printf("  0x00B Config       0x%04X\r\n", s->config);
    bms_printf("  0x0AB Config2      0x%04X\r\n", s->config2);
    bms_printf("  0x1A8 nBattStatus  0x%04X\r\n", s->nbatt_status);
    bms_printf("  0x021 DevName      0x%04X   (device %u, rev %u)\r\n",
               s->dev_name, (unsigned)(s->dev_name & 0x0Fu), (unsigned)(s->dev_name >> 4));
    bms_print("-----------------------------------------------\r\n");
}

/* Only the keys this build actually has. A help screen that lists a key
 * the switch statement does not handle is a bug report waiting to happen,
 * and the closing paragraph is a safety claim that has to stay true for
 * the build it is printed by. */
void bms_dashboard_help(void)
{
    bms_print("\r\n-- keys ---------------------------------------\r\n"
              "  d   ANSI dashboard (default)\r\n"
              "  j   stream one JSON object per sample\r\n"
              "  r   dump raw status registers once\r\n"
              "  n   read the NV profile back and diff it\r\n"
              "  x   read one register: x then three hex digits\r\n"
              "  b   bus check: read SCL/SDA idle levels to tell a missing\r\n"
              "      pull-up apart from an unpowered gauge\r\n"
#ifndef BMS_REALTIME_HOST
              "  w   life check: run the bus at ~20 kHz off the MCU's own\r\n"
              "      internal pull-ups and see if the gauge ACKs at all\r\n"
              "  t   trip capture: poll the fault and FET bits flat out\r\n"
              "  !   provisioning menu (the only path that writes NVM)\r\n"
#endif
              "  c   clear sticky protection alerts (asks Y/n)\r\n"
              "  p   re-probe the gauge and re-read boot config\r\n"
              "  h   this help\r\n"
              "-----------------------------------------------\r\n"
#ifdef BMS_REALTIME_HOST
              "This build is read-only apart from 'c'. It never writes\r\n"
              "NVM, never touches Command/CommStat, and cannot consume\r\n"
              "one of the part's 7 lifetime NVM writes.\r\n"
              "n, x, c, b and p do their I2C synchronously, so they are\r\n"
              "refused while the host says stalling the loop is unsafe\r\n"
              "(on the car: while the drive is live).\r\n\r\n"
#else
              "Everything is read-only except 'c' and the '!' menu. Inside\r\n"
              "'!', only option 4 spends one of the part's 7 lifetime NVM\r\n"
              "writes, and it asks before it does.\r\n\r\n"
#endif
              );
}

void bms_dashboard_banner(const max17320_snapshot_t *s, max17320_status_t probe_result)
{
    bms_print(ANSI_CLS);
    /* No peripheral number here: the two builds are not on the same I2C
     * instance, and this banner has no way to know which handle it was
     * given. The pin names it can state, because they come from the same
     * defines the bus check drives. */
    bms_print(C_BLD "MAX17320 BMS monitor" C_RST "  --  STM32G474RE, SCL "
              BMS_SCL_NAME ", SDA " BMS_SDA_NAME "\r\n");

    if (probe_result == MAX17320_OK) {
        bms_printf("probe: OK  (0x36 and 0x0B both answer)  DevName 0x%04X\r\n",
                   (s != NULL) ? s->dev_name : 0u);
    } else {
        bms_printf(C_RED "probe: %s" C_RST "\r\n", max17320_status_str(probe_result));
        /* The pin names come from bms_pins.h, which is also what the bus
         * check drives -- the two builds are not on the same pins, and a
         * banner that named the wrong ones would send someone rewiring a
         * working link. */
        bms_print("  check, in this order:\r\n"
                  "   1. pack connected? the gauge is powered from BATTP, it is\r\n"
                  "      dead without cells and looks exactly like a bus fault\r\n"
                  "   2. pull-ups fitted? this board has none of its own --\r\n"
                  "      4.7k from SDA and SCL to 3V3\r\n"
                  "   3. this build expects SCL = " BMS_SCL_NAME
                  ", SDA = " BMS_SDA_NAME ",\r\n"
                  "      plus any GND pin. Pack side: P1.3 = SCL, P1.2 = SDA,\r\n"
                  "      P1.5 = GND\r\n");
    }
    bms_print("press h for keys\r\n\r\n");
}
