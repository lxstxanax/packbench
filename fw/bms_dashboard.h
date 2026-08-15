#ifndef BMS_DASHBOARD_H
#define BMS_DASHBOARD_H

/*
 * Renderers for a max17320_snapshot_t.
 *
 * bms_dashboard_render() draws a full-screen ANSI dashboard that redraws
 * in place (cursor-home + erase-to-end-of-line per row, no full clear) so
 * it does not flicker in minicom/picocom/PuTTY.
 *
 * bms_dashboard_json() emits one self-contained JSON object per line for
 * a host-side GUI to parse.
 */

#include "max17320_monitor.h"

/*
 * What to tell a human who needs this part provisioned. The provisioning
 * menu is compiled out of a real-time build, so that build must not send
 * anyone hunting for a '!' key it does not have. Defined once here because
 * both the dashboard and the console print it.
 */
#ifdef BMS_REALTIME_HOST
#define BMS_PROVISION_HINT "with the bench monitor build"
#else
#define BMS_PROVISION_HINT "! then 2, verify, then 4"
#endif

/* Clears the screen and paints the static frame. Call once when entering
 * dashboard mode, then call render() repeatedly. */
void bms_dashboard_enter(void);

/* Sets a one-line banner shown under the header until cleared with NULL.
 * Used for bench states the reader must not forget about, such as running
 * the bus on the MCU's internal pull-ups. The string is not copied. */
void bms_dashboard_set_note(const char *note);

void bms_dashboard_render(const max17320_snapshot_t *s, const max17320_monitor_t *mon);
void bms_dashboard_json(const max17320_snapshot_t *s, const max17320_monitor_t *mon);

/* One-shot diagnostics printed as a normal scrolling log. */
void bms_dashboard_raw_dump(const max17320_snapshot_t *s);
void bms_dashboard_help(void);
void bms_dashboard_banner(const max17320_snapshot_t *s, max17320_status_t probe_result);

#endif /* BMS_DASHBOARD_H */
