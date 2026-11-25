#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RECORDER_MAX_MS  60000
#define RECORDER_PATH_MAX 96

typedef struct {
    bool available;  /* codec initialised and usable */
    bool recording;
    bool storage_error;
    uint32_t elapsed_ms;
    uint32_t max_ms;
    uint32_t queued;
    char error[48];
} recorder_status_t;

typedef void (*recorder_event_cb_t)(void);

/* Queue item owned by the recorder; treat as opaque. */
typedef struct recorder_item recorder_item_t;

esp_err_t recorder_init(void);
void recorder_set_event_callback(recorder_event_cb_t cb);

bool recorder_available(void);
esp_err_t recorder_start(void);
/* Stop and enqueue the current recording. */
esp_err_t recorder_stop(void);
/* Stop and discard the current recording. */
void recorder_cancel(void);

void recorder_get_status(recorder_status_t *out);

/*
 * Queue access for the uploader. `recorder_peek()` returns the oldest item
 * (still owned by the queue) without removing it, or NULL when empty. Only the
 * uploader removes items via `recorder_item_sent()` / `recorder_item_drop()`.
 */
recorder_item_t *recorder_peek(void);
const char *recorder_item_path(const recorder_item_t *item); /* NULL => RAM */
const uint8_t *recorder_item_data(const recorder_item_t *item, size_t *len);
size_t recorder_item_size(const recorder_item_t *item);

/* Upload succeeded: remove and delete the recording. */
void recorder_item_sent(recorder_item_t *item);
/* Permanent failure (bad request): remove and delete so it is not retried. */
void recorder_item_drop(recorder_item_t *item);
