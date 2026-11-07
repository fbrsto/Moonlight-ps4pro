// On-screen menu laid out like the official client: PCs, app grid, settings.
#pragma once

#include "../config.h"
#include "../gamestream/client.h"

#define UI_MENU_LAUNCH    0 /* launch stream with cfg->app_name */
#define UI_MENU_RECONNECT 1 /* host changed / retry connection */

/* Status screen (pairing, connecting…). Leaves VideoOut BGRA open. */
void ui_show_status(const char *title, const char *line1, const char *line2);

/* server may be NULL (no connection): PC page, Settings and retry only.
 * Returns UI_MENU_LAUNCH, UI_MENU_RECONNECT, or <0 (video error). */
int ui_menu_run(app_config_t *cfg, gs_server_t *server, const char *config_dir);
