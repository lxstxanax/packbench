#ifndef MAX17320_PROVISION_H
#define MAX17320_PROVISION_H

/*
 * NVM provisioning for the MAX17320, ported from the F7 tool in
 * github.com/lxstxanax/stm32f767zi-max17320-bms. Every address, command
 * code and timing is that driver's, which took them from the datasheet's
 * "Nonvolatile Block Programming" / "Determining Number of Remaining
 * Updates" sections -- this port changes the HAL header, routes logging
 * through the console, and adds a supply-voltage interlock.
 *
 * THE WRITE BUDGET IS SEVEN, FOR THE LIFE OF THE PART, and Maxim's factory
 * test already spent one. A failed or wrong commit spends one just the
 * same. Hence the sequence the F7 tool established and this keeps:
 *
 *   1. backup      -- read what is there now
 *   2. shadow      -- write the profile to shadow RAM (volatile, free,
 *                     repeatable) and read it back
 *   3. remaining   -- ask the part how many writes are left, show a human
 *   4. commit      -- only then, and only with an explicit confirmation
 *
 * Steps 1-3 cannot consume a write cycle. Only max17320_commit_nvm() can.
 */

#include "max17320.h"
#include "max17320_config.h"

/* Reads every register in max17320_target_config into out[]. Pure
 * readback; run it before anything else so the previous contents are
 * recoverable. */
max17320_status_t max17320_backup_config(I2C_HandleTypeDef *hi2c,
                                         uint16_t *out, size_t out_len);

/* Writes the profile to shadow RAM (0x180-0x1EF). Volatile: lost on power
 * cycle, touches no NVM, safe to repeat. */
max17320_status_t max17320_write_shadow_config(I2C_HandleTypeDef *hi2c);

/* Reads every profile register back and compares. MAX17320_OK when all
 * match, MAX17320_ERR_MISMATCH otherwise; mismatches[] collects the
 * differing indices (pass NULL/0 to skip). */
max17320_status_t max17320_verify_shadow_config(I2C_HandleTypeDef *hi2c,
                                                size_t *mismatches,
                                                size_t mismatch_cap,
                                                size_t *mismatch_count);

/* How many of the 7 lifetime writes are gone. Always show this to a human
 * before committing. */
max17320_status_t max17320_read_remaining_nvm_updates(I2C_HandleTypeDef *hi2c,
                                                      uint8_t *used,
                                                      uint8_t *remaining);

/*
 * Burns one lifetime write, copying shadow RAM into NVM per the
 * datasheet's 12-step sequence. Irreversible, and irreversible even when
 * the result is wrong.
 *
 * Gated three ways:
 *   - compile time: MAX17320_I_KNOW_THIS_BURNS_NVM must be 1
 *   - run time:     confirm_token must equal MAX17320_NVM_CONFIRM_TOKEN
 *   - supply:       refuses below MAX17320_MIN_COMMIT_MV, because the
 *                   datasheet's minimum operating supply is 4.2 V and a
 *                   flash write below it can land corrupted -- while still
 *                   spending the cycle
 */
#define MAX17320_NVM_CONFIRM_TOKEN (0xA5A5C0DEu)

/* Refuse to commit below this pack voltage, in mV. The datasheet's VIN
 * minimum is 4.2 V; for a 2S pack anything under ~6 V also means the stack
 * itself is not wired the way the profile assumes. */
#ifndef MAX17320_MIN_COMMIT_MV
#define MAX17320_MIN_COMMIT_MV 6000
#endif

max17320_status_t max17320_commit_nvm(I2C_HandleTypeDef *hi2c, uint32_t confirm_token);

/*
 * Steps 9-12 only: re-unlock, pulse Config2.POR_CMD, wait, re-lock. Never
 * touches NVM, so it is safe to retry. Use this -- never commit again --
 * if a commit reported the block copy as done but then failed afterwards.
 */
max17320_status_t max17320_finish_post_commit_reset(I2C_HandleTypeDef *hi2c);

#endif /* MAX17320_PROVISION_H */
