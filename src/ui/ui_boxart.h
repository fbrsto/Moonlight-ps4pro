// App box art: fetched from the host, scaled to the grid's size, kept in RAM.
#pragma once

#include <stdint.h>
#include "../gamestream/client.h"

#define UI_BOXART_W 200
#define UI_BOXART_H 267

/* Forget everything if host differs from the host the cache belongs to. */
void ui_boxart_set_host(const char *host);

/* Art for an app, or the no-art image if not loaded (yet). *is_placeholder is
 * set when the app's name must be drawn over the image. Returns NULL only if
 * even the built-in image could not be decoded. */
const uint32_t *ui_boxart_get(int app_id, int *is_placeholder);

/* Fetch one missing image (blocking). Returns 1 if it tried one, 0 if all done. */
int ui_boxart_load_next(gs_server_t *server, const int *app_ids, int count);
