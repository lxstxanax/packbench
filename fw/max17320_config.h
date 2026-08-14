#ifndef MAX17320_CONFIG_H
#define MAX17320_CONFIG_H

/*
 * Target NV/shadow configuration for MAX17320 (2S4P pack), sourced from:
 *   Documents/Maxim Integrated/MAX17320/fertig_11400.INI
 *   ("Export of Non-Volatile Memory for 340000E098F12626")
 *
 * This is the tuned, working configuration exported from the USB/GUI EVAL
 * board for this specific pack (capacity, charge-termination current,
 * protection thresholds, etc.) -- NOT the generic settings_bms.INI
 * template, whose values do not match this system.
 *
 * nROMIDx (0x1BC-0x1BF) are intentionally EXCLUDED from this table: on the
 * source export they hold the unique factory die ID of the first (USB)
 * chip ("340000E098F12626"). That ID must not be pushed onto the second,
 * physically different die. Verify in the MAX17320 datasheet whether
 * nROMIDx is even writable before ever touching it.
 *
 * Pack facts confirmed with the user (2026-07-09), not guessed:
 *   - Cells: Molicel INR18650-P30B, 2.85Ah as printed on the physical cell
 *     label (datasheet advertises up to 3.0Ah typical, but the user is
 *     deliberately using the conservative printed/guaranteed value).
 *     2S4P -> pack capacity = 4 x 2.85Ah = 11.4Ah, matching nDesignCap
 *     below exactly (11400 = 0x2C88) -- this is why fertig_11400.INI's
 *     capacity is correct for this pack and was NOT changed.
 *   - Physical current-sense resistor actually populated on this board:
 *     MFC0603-R005FT5, 5.0mOhm (confirmed against the schematic BOM).
 *     With 5mOhm, the MAX17320's own current ADC / OCCP-ODCP protection
 *     range tops out at +-51.2mV/RSENSE = +-10.24A (datasheet Table 15)
 *     -- this is a hardware ceiling, not a firmware setting. Target load
 *     for this board is ~10A, so 5mOhm is being kept as-is here.
 */

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------
 * Physical current-sense resistor, in milliohms, actually populated on
 * the board this table is flashed to. ALL current- and capacity-type NV
 * registers below are computed relative to this value, because their
 * register codes represent a physical A/mAh quantity scaled by 1/RSENSE
 * (datasheet Table 15: Capacity LSB = 5.0uVh/RSENSE, Current LSB =
 * 1.5625uV/RSENSE). Changing the physical resistor without updating this
 * define (and nRSense/nIPrtTh1/etc. that derive from it) would silently
 * shift what capacity/current the gauge and protector actually believe
 * they are seeing.
 *
 * CHANGE THIS when a board gets reworked to a different shunt (e.g. to 1
 * for a ~1mOhm shunt supporting ~40A, for the higher-power/240W boards)
 * and rebuild -- nRSense, nIChgTerm, nFullCapNom/Rep, nDesignCap and
 * nIPrtTh1 below all recompute automatically from the *_REF_5MOHM raw
 * values (which are frozen at the as-characterized 5mOhm numbers from
 * fertig_11400.INI). nODSCTh (short-circuit/overdischarge/overcharge mV
 * thresholds, Table 22/23) is ALSO recomputed, but because it is a
 * bit-packed, range-limited, safety-critical field, treat the computed
 * value as a first draft only: manually re-derive it from the datasheet
 * tables and re-verify the resulting A thresholds before ever flashing a
 * board with a different shunt value.
 * ------------------------------------------------------------------- */
#ifndef MAX17320_RSENSE_MOHM
#define MAX17320_RSENSE_MOHM      5
#endif
#define MAX17320_RSENSE_REF_MOHM  5   /* shunt value the *_REF_5MOHM constants below were captured at -- history, do not change */

/* Scales a 16-bit capacity/current register code from the RSENSE_REF_MOHM
 * reference to MAX17320_RSENSE_MOHM, preserving the physical A/mAh value
 * the code represents (code_new = code_ref * RSENSE_MOHM / RSENSE_REF_MOHM,
 * since LSB size is proportional to RSENSE). Pure compile-time constant
 * arithmetic -- safe to use as a static initializer. */
#define MAX17320_SCALE_U16(ref_code) \
    ((uint16_t)(((int32_t)(ref_code) * MAX17320_RSENSE_MOHM) / MAX17320_RSENSE_REF_MOHM))

/* Same scaling, applied to one signed 8-bit sub-field (nIPrtTh1's OCCP/ODCP
 * are two independent signed bytes packed into one word, not one 16-bit
 * value -- must be scaled separately or the sign/byte-boundary breaks). */
#define MAX17320_SCALE_I8(ref_byte) \
    ((uint8_t)(uint8_t)((int32_t)(int8_t)(uint8_t)(ref_byte) * MAX17320_RSENSE_MOHM / MAX17320_RSENSE_REF_MOHM))

/* nIPrtTh1 (1D3h): upper byte = OCCP (overcharge current), lower byte =
 * ODCP (overdischarge current), both signed 2's complement, independently
 * scaled then repacked. */
#define MAX17320_IPRTTH1(ref_word) \
    ((uint16_t)(((uint16_t)MAX17320_SCALE_I8((ref_word) >> 8) << 8) | MAX17320_SCALE_I8((ref_word) & 0xFFu)))

/* nODSCTh (1DDh): OCTH[14:10]/SCTH[9:5]/ODTH[4:0], each an affine mV code
 * (datasheet Table 22/23), not a proportional one -- decode to mV (in
 * quarter-mV units to stay integer), scale the mV by the same RSENSE
 * ratio (mV_new = mV_ref * RSENSE_MOHM/RSENSE_REF_MOHM keeps the target
 * *current* constant), then re-encode and clamp to each field's 0-31
 * range. SEE HEADER COMMENT: verify manually before use on a new shunt. */
#define MAX17320_CLAMP5(x) ((x) < 0 ? 0 : ((x) > 31 ? 31 : (x)))
#define MAX17320_ODSCTH(ref_word) \
    ((uint16_t)( \
        (MAX17320_CLAMP5((155  - (155  - (int32_t)(((ref_word) >> 10) & 0x1Fu) * 5)  * MAX17320_RSENSE_MOHM / MAX17320_RSENSE_REF_MOHM) / 5))  << 10 | \
        (MAX17320_CLAMP5(((-620 + (int32_t)(((ref_word) >> 5)  & 0x1Fu) * 20) * MAX17320_RSENSE_MOHM / MAX17320_RSENSE_REF_MOHM + 620) / 20)) << 5  | \
        (MAX17320_CLAMP5(((-310 + (int32_t)((ref_word)         & 0x1Fu) * 10) * MAX17320_RSENSE_MOHM / MAX17320_RSENSE_REF_MOHM + 310) / 10)) \
    ))

/* ---------------------------------------------------------------------
 * Maximum-current option (requested 2026-08-14).
 *
 * With the 5mOhm shunt actually populated, the achievable limits are
 * NOT symmetric:
 *     discharge  -10.24A  (the current ADC's own full scale)
 *     charge      +7.75A  (nODSCTh.OCTH, and it cannot be raised further
 *                          or disabled -- see the note below)
 *
 * The captured profile ran +6.00A charge / -10.24A discharge, so discharge
 * was already at the ceiling and only charge had headroom left.
 *
 * Set MAX17320_MAX_CURRENT_LIMITS to 0 to fall back to the captured
 * profile's values verbatim.
 * ------------------------------------------------------------------- */
#ifndef MAX17320_MAX_CURRENT_LIMITS
#define MAX17320_MAX_CURRENT_LIMITS 1
#endif

/*
 * Four registers gate current, not one. Raising nIPrtTh1 alone leaves the
 * fast comparators and the JEITA prescription where they were, and the
 * lowest ceiling wins.
 *
 *   nIPrtTh1  slow OCCP/ODCP, 400uV LSb signed  -> +10.08A / -10.24A
 *   nODSCTh   fast OCTH/SCTH/ODTH comparators   -> +7.75A  <-- the ceiling
 *   nJEITAC   prescribed charge current per zone-> 7.00A flat
 *   nStepChg  derating above 4.12V/cell         -> disabled
 *
 * OCTH is an INVERTED 5-bit code: 00h is the maximum and means
 * +38.75mV/Rsense = +7.75A at 5mOhm. There is no documented way to
 * disable it, so 7.75A is a hard charge ceiling for this shunt --
 * a smaller shunt is the only way past it (2mOhm -> 19.4A).
 * Discharge has no such comparator ceiling in range: -10.24A is the
 * ADC's own limit.
 *
 * OCCP is 7Eh rather than 7Fh on purpose: the Current register saturates
 * at 7FFFh, so its upper byte can equal 7Fh but never exceed it, and the
 * fault fires only on "exceeds" -- 7Fh would leave slow overcharge
 * protection permanently unarmed. 7Eh = +10.08A, armed.
 */
#if MAX17320_MAX_CURRENT_LIMITS
#define MAX17320_NIPRTTH1_VALUE  ((uint16_t)0x7E80u)  /* +10.08A / -10.24A */
#define MAX17320_NODSCTH_VALUE   ((uint16_t)0x0166u)  /* OCTH +7.75A (max), SCTH -20A, ODTH -12.5A */
#define MAX17320_NJEITAC_VALUE   ((uint16_t)0xAFFFu)  /* 7.00A in every temperature zone */
#define MAX17320_NSTEPCHG_VALUE  ((uint16_t)0xFF00u)  /* no step-charge derating */
#else
#define MAX17320_NIPRTTH1_VALUE  MAX17320_IPRTTH1(0x4B80)  /* +6.00A / -10.24A */
#define MAX17320_NODSCTH_VALUE   MAX17320_ODSCTH(0x0C00)
#define MAX17320_NJEITAC_VALUE   ((uint16_t)0x644Bu)       /* 4.00A room, 1.00A cold */
#define MAX17320_NSTEPCHG_VALUE  ((uint16_t)0xC884u)
#endif

typedef struct {
    uint16_t    addr;
    uint16_t    value;
    const char *name;
} max17320_reg_t;

static const max17320_reg_t max17320_target_config[] = {
    { 0x180, 0x0000, "nXTable0" },
    { 0x181, 0x0000, "nXTable1" },
    { 0x182, 0x0000, "nXTable2" },
    { 0x183, 0x0000, "nXTable3" },
    { 0x184, 0x0000, "nXTable4" },
    { 0x185, 0x0000, "nXTable5" },
    { 0x186, 0x0000, "nXTable6" },
    { 0x187, 0x0000, "nXTable7" },
    { 0x188, 0x0000, "nXTable8" },
    { 0x189, 0x0000, "nXTable9" },
    { 0x18A, 0x0000, "nXTable10" },
    { 0x18B, 0x0000, "nXTable11" },
    { 0x18C, 0x0000, "nVAlrtTh" },
    { 0x18D, 0x0000, "nTAlrtTh" },
    { 0x18E, 0x0000, "nIAlrtTh" },
    { 0x18F, 0x0000, "nSAlrtTh" },
    { 0x190, 0x0000, "nOCVTable0" },
    { 0x191, 0x0000, "nOCVTable1" },
    { 0x192, 0x0000, "nOCVTable2" },
    { 0x193, 0x0000, "nOCVTable3" },
    { 0x194, 0x0000, "nOCVTable4" },
    { 0x195, 0x0000, "nOCVTable5" },
    { 0x196, 0x0000, "nOCVTable6" },
    { 0x197, 0x0000, "nOCVTable7" },
    { 0x198, 0x0000, "nOCVTable8" },
    { 0x199, 0x0000, "nOCVTable9" },
    { 0x19A, 0x0000, "nOCVTable10" },
    { 0x19B, 0x0000, "nOCVTable11" },
    { 0x19C, MAX17320_SCALE_U16(0x0720), "nIChgTerm" },
    { 0x19D, 0x0000, "nFilterCfg" },
    { 0x19E, 0x0000, "nVEmpty" },
    { 0x19F, 0x0000, "nLearnCfg" },
    { 0x1A0, 0x1050, "nQRTable00" },
    { 0x1A1, 0x8002, "nQRTable10" },
    { 0x1A2, 0x078C, "nQRTable20" },
    { 0x1A3, 0x0880, "nQRTable30" },
    { 0x1A4, 0x0000, "nCycles" },
    { 0x1A5, MAX17320_SCALE_U16(0x33A8), "nFullCapNom" },
    { 0x1A6, 0x0445, "nRComp0" },
    { 0x1A7, 0x223E, "nTempCo" },
    { 0x1A8, 0x0000, "nBattStatus" },
    { 0x1A9, MAX17320_SCALE_U16(0x2C88), "nFullCapRep" },
    { 0x1AA, 0x0000, "ndQTot" },
    { 0x1AB, 0x0000, "nMaxMinCurr" },
    { 0x1AC, 0x0000, "nMaxMinVolt" },
    { 0x1AD, 0x0000, "nMaxMinTemp" },
    { 0x1AE, 0x0000, "nFaultLog" },
    { 0x1AF, 0x0000, "nTimerH" },
    { 0x1B0, 0x2290, "nCONFIG" },
    { 0x1B1, 0x0204, "nRippleCfg" },
    { 0x1B2, 0x0000, "nMiscCFG" },
    { 0x1B3, MAX17320_SCALE_U16(0x2C88), "nDesignCap" },
    { 0x1B4, 0x0008, "nSBSCFG" },
    { 0x1B5, 0x0004, "nPACKCFG" },
    { 0x1B6, 0x083B, "nRelaxCFG" },
    { 0x1B7, 0x2241, "nConvgCFG" },
    { 0x1B8, 0x0A80, "nNVCFG0" },
    { 0x1B9, 0x0182, "nNVCFG1" },
    { 0x1BA, 0xBE2D, "nNVCFG2" },
    { 0x1BB, 0x0909, "nHibCFG" },
    /* 0x1BC-0x1BF nROMIDx intentionally omitted -- unique die ID of the
     * source (USB) chip, do not copy to the second physical device. */
    { 0x1C0, 0x0000, "nChgCtrl1" },
    { 0x1C1, 0x0000, "nPReserved1" },
    { 0x1C2, 0x2061, "nChgCfg0" },
    { 0x1C3, 0x00E1, "nChgCtrl0" },
    { 0x1C4, 0x0000, "nRGain" },
    { 0x1C5, 0x0000, "nPackResistance" },
    { 0x1C6, 0x0000, "nFullSOCThr" },
    { 0x1C7, 0x0000, "nTTFCFG" },
    { 0x1C8, 0x4000, "nCGAIN" },
    { 0x1C9, 0x0000, "nCGTempCo" },
    { 0x1CA, 0x71BE, "nThermCfg" },
    { 0x1CB, 0x0000, "nChgCfg1" },
    { 0x1CC, 0x0000, "nManfctrName" },
    { 0x1CD, 0x0000, "nManfctrName1" },
    { 0x1CE, 0x0000, "nManfctrName2" },
    { 0x1CF, (uint16_t)(MAX17320_RSENSE_MOHM * 100u), "nRSense" }, /* LSb = 10uOhm (datasheet) */
    { 0x1D0, 0x785B, "nUVPrtTh" },
    { 0x1D1, 0x3700, "nTPrtTh1" },
    { 0x1D2, 0x5F28, "nTPrtTh3" },
    { 0x1D3, MAX17320_NIPRTTH1_VALUE, "nIPrtTh1" },
    { 0x1D4, 0x0000, "nBALTh" },
    { 0x1D5, 0x2D0A, "nTPrtTh2" },
    { 0x1D6, 0x7A58, "nProtMiscTh" },
    { 0x1D7, 0x0900, "nProtCfg" },
    { 0x1D8, MAX17320_NJEITAC_VALUE, "nJEITAC" },
    { 0x1D9, 0x0059, "nJEITAV" },
    { 0x1DA, 0xB754, "nOVPrtTh" },
    { 0x1DB, MAX17320_NSTEPCHG_VALUE, "nStepChg" },
    { 0x1DC, 0xAB3D, "nDelayCfg" },
    { 0x1DD, MAX17320_NODSCTH_VALUE, "nODSCTh" }, /* OCTH is the hard charge ceiling -- see the note above */
    { 0x1DE, 0x4355, "nODSCCfg" },
    { 0x1DF, 0x8016, "nProtCfg2" },
    { 0x1E0, 0x0000, "nDPLimit" },
    { 0x1E1, 0x0000, "nScOcvLim" },
    { 0x1E2, 0x0000, "nAgeFcCfg" },
    { 0x1E3, 0xA5B9, "nDesignVoltage" },
    { 0x1E4, 0x0000, "nVGain" },
    { 0x1E5, 0x0000, "nRFastVShdn" },
    { 0x1E6, 0x0000, "nManfctrDate" },
    { 0x1E7, 0x0000, "nFirstUsed" },
    { 0x1E8, 0x0000, "nSerialNumber0" },
    { 0x1E9, 0x0000, "nSerialNumber1" },
    { 0x1EA, 0x0000, "nSerialNumber2" },
    { 0x1EB, 0x0000, "nDeviceName0" },
    { 0x1EC, 0x0000, "nDeviceName1" },
    { 0x1ED, 0x0000, "nDeviceName2" },
    { 0x1EE, 0x0000, "nDeviceName3" },
    { 0x1EF, 0x0000, "nDeviceName4" },
};

#define MAX17320_TARGET_CONFIG_COUNT \
    (sizeof(max17320_target_config) / sizeof(max17320_target_config[0]))

#endif /* MAX17320_CONFIG_H */
