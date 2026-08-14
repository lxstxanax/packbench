#include "max17320_provision.h"
#include "max17320_monitor.h"
#include "bms_io.h"

/* Datasheet register/command constants (all main-address-space, < 0x180) */
#define REG_COMMAND             0x060u
#define REG_COMMSTAT            0x061u
#define REG_CONFIG2             0x0ABu
#define REG_NV_REMAINING_FLAGS  0x1FDu   /* valid only right after the recall command */

#define CMD_COPY_NV_BLOCK              0xE904u
#define CMD_RECALL_REMAINING_UPDATES   0xE29Bu
#define CMD_FULL_RESET                 0x000Fu
#define CONFIG2_POR_CMD                0x8000u
#define CONFIG2_POR_CMD_BIT            (1u << 15)

#define COMMSTAT_UNLOCK          0x0000u
#define COMMSTAT_LOCK            0x00F9u
#define COMMSTAT_NVERROR_MASK    0x0004u

/* Electrical Characteristics (max values) plus a small margin */
#define T_BLOCK_MS   7360u
#define T_RECALL_MS  20u
/* The datasheet gives no explicit maximum for Config2.POR_CMD to
 * self-clear. The F7 tool observed it exceeding 200 ms on real hardware
 * twice in a row after a successful copy, so the margin here is large --
 * this stage touches no NVM, so waiting costs nothing. */
#define T_POR_POLL_TIMEOUT_MS 3000u

static max17320_status_t commstat_write(I2C_HandleTypeDef *hi2c, uint16_t value, int times)
{
    for (int i = 0; i < times; i++) {
        max17320_status_t st = max17320_write_reg(hi2c, REG_COMMSTAT, value);
        if (st != MAX17320_OK) {
            return st;
        }
    }
    return MAX17320_OK;
}

max17320_status_t max17320_backup_config(I2C_HandleTypeDef *hi2c,
                                         uint16_t *out, size_t out_len)
{
    if ((out == NULL) || (out_len < MAX17320_TARGET_CONFIG_COUNT)) {
        return MAX17320_ERR_ARG;
    }

    for (size_t i = 0; i < MAX17320_TARGET_CONFIG_COUNT; i++) {
        max17320_status_t st = max17320_read_reg(hi2c, max17320_target_config[i].addr, &out[i]);
        if (st != MAX17320_OK) {
            return st;
        }
    }
    return MAX17320_OK;
}

max17320_status_t max17320_write_shadow_config(I2C_HandleTypeDef *hi2c)
{
    max17320_status_t st;

    /* Shadow-RAM writes to 0x180-0x1EF are ignored while write protection
     * is set. Clearing it does not touch NVM -- only Copy NV Block does. */
    st = commstat_write(hi2c, COMMSTAT_UNLOCK, 2);
    if (st != MAX17320_OK) {
        return st;
    }

    for (size_t i = 0; i < MAX17320_TARGET_CONFIG_COUNT; i++) {
        st = max17320_write_reg(hi2c, max17320_target_config[i].addr,
                                max17320_target_config[i].value);
        if (st != MAX17320_OK) {
            (void)commstat_write(hi2c, COMMSTAT_LOCK, 2);  /* best effort re-lock */
            return st;
        }
        HAL_Delay(2);
    }

    return commstat_write(hi2c, COMMSTAT_LOCK, 2);
}

max17320_status_t max17320_verify_shadow_config(I2C_HandleTypeDef *hi2c,
                                                size_t *mismatches,
                                                size_t mismatch_cap,
                                                size_t *mismatch_count)
{
    size_t found = 0;

    for (size_t i = 0; i < MAX17320_TARGET_CONFIG_COUNT; i++) {
        uint16_t readback = 0;
        max17320_status_t st = max17320_read_reg(hi2c, max17320_target_config[i].addr, &readback);
        if (st != MAX17320_OK) {
            return st;
        }
        if (readback != max17320_target_config[i].value) {
            if ((mismatches != NULL) && (found < mismatch_cap)) {
                mismatches[found] = i;
            }
            found++;
        }
    }

    if (mismatch_count != NULL) {
        *mismatch_count = found;
    }
    return (found == 0) ? MAX17320_OK : MAX17320_ERR_MISMATCH;
}

max17320_status_t max17320_read_remaining_nvm_updates(I2C_HandleTypeDef *hi2c,
                                                      uint8_t *used,
                                                      uint8_t *remaining)
{
    max17320_status_t st;
    uint16_t flags = 0;
    uint8_t or_byte, count = 0;

    st = commstat_write(hi2c, COMMSTAT_UNLOCK, 2);
    if (st != MAX17320_OK) {
        return st;
    }

    st = max17320_write_reg(hi2c, REG_COMMAND, CMD_RECALL_REMAINING_UPDATES);
    if (st != MAX17320_OK) {
        return st;
    }

    HAL_Delay(T_RECALL_MS);

    st = max17320_read_reg(hi2c, REG_NV_REMAINING_FLAGS, &flags);
    if (st != MAX17320_OK) {
        return st;
    }

    /* Datasheet: OR the two bytes together, the population count is how
     * many of the 7 updates have been used. */
    or_byte = (uint8_t)((flags & 0xFFu) | (flags >> 8));
    for (uint8_t b = 0; b < 8u; b++) {
        if (or_byte & (uint8_t)(1u << b)) {
            count++;
        }
    }
    if (used != NULL) {
        *used = count;
    }
    if (remaining != NULL) {
        *remaining = (count >= 7u) ? 0u : (uint8_t)(7u - count);
    }

    return commstat_write(hi2c, COMMSTAT_LOCK, 2);
}

#ifndef MAX17320_I_KNOW_THIS_BURNS_NVM
#define MAX17320_I_KNOW_THIS_BURNS_NVM 0
#endif

max17320_status_t max17320_commit_nvm(I2C_HandleTypeDef *hi2c, uint32_t confirm_token)
{
#if MAX17320_I_KNOW_THIS_BURNS_NVM
    max17320_status_t st;
    uint16_t commstat = 0;
    uint16_t batt_raw = 0;
    int32_t  batt_mv;

    if (confirm_token != MAX17320_NVM_CONFIRM_TOKEN) {
        return MAX17320_ERR_NOT_CONFIRMED;
    }

    /* Supply interlock. The part's minimum operating supply is 4.2 V; a
     * flash write below it can land corrupted and still spends one of the
     * seven lifetime cycles. Batt is measured at BATTS, i.e. the actual
     * stack the die runs from. */
    st = max17320_read_reg(hi2c, MAX17320_REG_BATT, &batt_raw);
    if (st != MAX17320_OK) {
        bms_print("  [commit] ABORT: cannot read Batt to check the supply\r\n");
        return st;
    }
    batt_mv = ((int32_t)batt_raw * 25) / 80;      /* 0.3125 mV per LSB -> mV */
    if (batt_mv < MAX17320_MIN_COMMIT_MV) {
        bms_printf("  [commit] ABORT: pack is %ld mV, below the %d mV floor.\r\n"
                   "           The datasheet minimum supply is 4200 mV; writing\r\n"
                   "           NVM under it can corrupt the block AND still\r\n"
                   "           spends one of the 7 lifetime writes.\r\n"
                   "           Fix the pack wiring first.\r\n",
                   (long)batt_mv, MAX17320_MIN_COMMIT_MV);
        return MAX17320_ERR_NOT_CONFIRMED;
    }
    bms_printf("  [commit] supply check: Batt = %ld mV, ok\r\n", (long)batt_mv);

    /* Steps 1+3: unlock write protection, clearing NVError with it. Step 2
     * (loading the shadow values) is the caller's job and must already
     * have been done and verified. */
    bms_print("  [commit] step 1+3: unlock write protection (3x)...\r\n");
    st = commstat_write(hi2c, COMMSTAT_UNLOCK, 3);
    if (st != MAX17320_OK) { bms_printf("  [commit] FAILED at step 1+3: %s\r\n", max17320_status_str(st)); return st; }

    /* Step 4: initiate the block copy. This is the irreversible one. */
    bms_print("  [commit] step 4: Copy NV Block (0xE904)...\r\n");
    st = max17320_write_reg(hi2c, REG_COMMAND, CMD_COPY_NV_BLOCK);
    if (st != MAX17320_OK) { bms_printf("  [commit] FAILED at step 4: %s\r\n", max17320_status_str(st)); return st; }

    /* Step 5: wait tBLOCK */
    bms_printf("  [commit] step 5: waiting tBLOCK (%lu ms)...\r\n", (unsigned long)T_BLOCK_MS);
    HAL_Delay(T_BLOCK_MS);

    /* Step 6: check NVError */
    bms_print("  [commit] step 6: checking CommStat.NVError...\r\n");
    st = max17320_read_reg(hi2c, REG_COMMSTAT, &commstat);
    if (st != MAX17320_OK) { bms_printf("  [commit] FAILED at step 6: %s\r\n", max17320_status_str(st)); return st; }
    if (commstat & COMMSTAT_NVERROR_MASK) {
        bms_printf("  [commit] NVError SET (CommStat=0x%04X): the block copy did\r\n"
                   "           not complete. NOT retrying -- each attempt costs\r\n"
                   "           another lifetime write. Re-read the remaining\r\n"
                   "           count before deciding anything.\r\n", commstat);
        return MAX17320_ERR_NVM_ERROR;
    }
    bms_printf("  [commit] NVError clear (CommStat=0x%04X), data is committed.\r\n", commstat);

    /* Step 7: full reset so the new NV settings take effect */
    bms_print("  [commit] step 7: full reset (0x000F)...\r\n");
    st = max17320_write_reg(hi2c, REG_COMMAND, CMD_FULL_RESET);
    if (st != MAX17320_OK) { bms_printf("  [commit] FAILED at step 7: %s\r\n", max17320_status_str(st)); return st; }

    /* Step 8: wait for the reset (write protection resets with it) */
    HAL_Delay(10);

    bms_print("  [commit] running post-write housekeeping (steps 9-12, no NVM)...\r\n");
    return max17320_finish_post_commit_reset(hi2c);
#else
    (void)hi2c;
    (void)confirm_token;
    bms_print("  [commit] built without MAX17320_I_KNOW_THIS_BURNS_NVM=1\r\n");
    return MAX17320_ERR_NOT_IMPLEMENTED;
#endif
}

max17320_status_t max17320_finish_post_commit_reset(I2C_HandleTypeDef *hi2c)
{
    max17320_status_t st;
    uint16_t config2 = 0;
    uint32_t waited_ms;

    /* Step 9: unlock again */
    bms_print("  [reset] step 9: unlock write protection (2x)...\r\n");
    st = commstat_write(hi2c, COMMSTAT_UNLOCK, 2);
    if (st != MAX17320_OK) { bms_printf("  [reset] FAILED at step 9: %s\r\n", max17320_status_str(st)); return st; }

    /* Step 10: restart the gauge firmware. Not an NVM write. */
    bms_print("  [reset] step 10: Config2.POR_CMD...\r\n");
    st = max17320_write_reg(hi2c, REG_CONFIG2, CONFIG2_POR_CMD);
    if (st != MAX17320_OK) { bms_printf("  [reset] FAILED at step 10: %s\r\n", max17320_status_str(st)); return st; }

    /* Step 11: wait for POR_CMD to self-clear */
    bms_printf("  [reset] step 11: waiting for POR_CMD to clear (up to %lu ms)...\r\n",
               (unsigned long)T_POR_POLL_TIMEOUT_MS);
    for (waited_ms = 0; waited_ms < T_POR_POLL_TIMEOUT_MS; waited_ms += 5u) {
        st = max17320_read_reg(hi2c, REG_CONFIG2, &config2);
        if (st != MAX17320_OK) { bms_printf("  [reset] FAILED at step 11: %s\r\n", max17320_status_str(st)); return st; }
        if ((config2 & CONFIG2_POR_CMD_BIT) == 0u) {
            break;
        }
        HAL_Delay(5);
    }
    if (config2 & CONFIG2_POR_CMD_BIT) {
        bms_printf("  [reset] step 11 TIMEOUT: Config2=0x%04X after %lu ms.\r\n"
                   "          Nothing was written here, so this is safe to retry.\r\n",
                   config2, (unsigned long)T_POR_POLL_TIMEOUT_MS);
        return MAX17320_ERR_I2C;
    }
    bms_printf("  [reset] step 11 ok, Config2=0x%04X\r\n", config2);

    /* Step 12: lock write protection */
    bms_print("  [reset] step 12: lock write protection...\r\n");
    return commstat_write(hi2c, COMMSTAT_LOCK, 2);
}
