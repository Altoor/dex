#include "net_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "recorder.h"
#include "wifi_manager.h"

static const char *TAG = "net";

#define HTTP_TIMEOUT_MS   15000
#define BODY_BUF_SIZE     8192
#define MAX_OUTSTANDING   8
#define POLL_INTERVAL_MS  (2 * 60 * 1000)
#define FORCE_POLL_MIN_MS 3000
#define POLL_TIMER_US     (2LL * 60 * 1000 * 1000)
#define MULTIPART_BOUNDARY "dexboundary7MA4YWxkTrZu0gW"

typedef enum {
    MSG_REFRESH = 0,
    MSG_SEND_ONLY,
    MSG_PATCH,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    bool force_poll;
    bool done;
    char todo_id[NET_TODO_ID_MAX];
} net_msg_t;

typedef struct {
    bool active;
    char id[APP_CONFIG_ID_MAX];
    int64_t last_poll_ms;
} outstanding_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;

static net_event_cb_t s_event_cb;
static volatile bool s_inflight;
static volatile uint32_t s_outstanding;

static net_todo_t s_todo;
static char s_message[NET_STATUS_MAX];
static char s_error[NET_ERROR_MAX];
static int64_t s_last_poll_ms;
static outstanding_t s_slots[MAX_OUTSTANDING];
static char *s_body;

static void notify(void)
{
    if (s_event_cb) {
        s_event_cb();
    }
}

static void set_message(const char *text)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    snprintf(s_message, sizeof(s_message), "%s", text ? text : "");
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void set_error(const char *text)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void clear_error(void)
{
    set_error("");
}

/* -------------------------------------------------------------- helpers -- */

static void build_auth(const app_config_t *cfg, bool dynamic, char *out,
                       size_t len)
{
    const char *key = dynamic ? cfg->dynamic_key : cfg->standard_key;
    snprintf(out, len, "Bearer %s", key);
}

/*
 * Read the whole response into s_body (bounded). Status snapshots include the
 * flow steps, so a single fixed-size read could truncate valid JSON.
 */
static void read_body(esp_http_client_handle_t client)
{
    int total = 0;

    while (total < BODY_BUF_SIZE - 1) {
        int got = esp_http_client_read(client, s_body + total,
                                       BODY_BUF_SIZE - 1 - total);
        if (got <= 0) {
            break;
        }
        total += got;
    }
    s_body[total] = '\0';

    /* Drain any remainder so the connection closes cleanly. */
    char scratch[128];
    while (esp_http_client_read(client, scratch, sizeof(scratch)) > 0) {
    }
}

static esp_err_t http_get(const char *url, const char *auth, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, 0);
    *status = 0;

    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
        read_body(client);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void json_to_text(cJSON *item, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }
    out[0] = '\0';
    if (item == NULL) {
        return;
    }
    if (cJSON_IsString(item)) {
        snprintf(out, len, "%s", item->valuestring ? item->valuestring : "");
        return;
    }
    if (cJSON_IsNull(item)) {
        return;
    }
    char *text = cJSON_PrintUnformatted(item);
    if (text) {
        snprintf(out, len, "%s", text);
        cJSON_free(text);
    }
}

/* ---------------------------------------------------------- outstanding -- */

static void add_outstanding(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return;
    }

    for (int i = 0; i < MAX_OUTSTANDING; i++) {
        if (s_slots[i].active && strcmp(s_slots[i].id, id) == 0) {
            return;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_OUTSTANDING; i++) {
        if (!s_slots[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Drop the oldest tracked request; the upload itself already succeeded. */
        memmove(&s_slots[0], &s_slots[1], sizeof(s_slots[0]) * (MAX_OUTSTANDING - 1));
        slot = MAX_OUTSTANDING - 1;
        s_outstanding--;
        if (s_outstanding > MAX_OUTSTANDING) {
            s_outstanding = MAX_OUTSTANDING;
        }
    }

    s_slots[slot].active = true;
    snprintf(s_slots[slot].id, sizeof(s_slots[slot].id), "%s", id);
    s_slots[slot].last_poll_ms = 0;
    s_outstanding++;
}

static void finish_slot(int index, const char *status, cJSON *result,
                        const char *error)
{
    app_result_t res = {0};
    snprintf(res.request_id, sizeof(res.request_id), "%s", s_slots[index].id);
    snprintf(res.status, sizeof(res.status), "%s", status ? status : "");
    json_to_text(result, res.result, sizeof(res.result));
    if (error) {
        snprintf(res.error, sizeof(res.error), "%s", error);
    }
    app_config_set_result(&res);

    s_slots[index].active = false;
    if (s_outstanding > 0) {
        s_outstanding--;
    }

    ESP_LOGI(TAG, "request %s -> %s", res.request_id, res.status);
}

static bool is_terminal(const char *status)
{
    return status != NULL &&
           (strcmp(status, "completed") == 0 || strcmp(status, "failed") == 0 ||
            strcmp(status, "cancelled") == 0);
}

static void poll_one(int index, const app_config_t *cfg)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/hooks/%s/requests/%s", cfg->server_url,
             cfg->dynamic_slug, s_slots[index].id);

    char auth[APP_CONFIG_KEY_MAX + 8];
    build_auth(cfg, true, auth, sizeof(auth));

    int status = 0;
    esp_err_t err = http_get(url, auth, &status);
    int64_t now = esp_timer_get_time() / 1000;
    s_slots[index].last_poll_ms = now;

    if (err != ESP_OK || status == 0) {
        ESP_LOGW(TAG, "status poll failed: %s", esp_err_to_name(err));
        return;
    }

    if (status == 401 || status == 403) {
        finish_slot(index, "error", NULL, "unauthorized");
        return;
    }
    if (status == 404) {
        finish_slot(index, "error", NULL, "request not found");
        return;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "status poll HTTP %d", status);
        return;
    }

    cJSON *root = cJSON_Parse(s_body);
    if (root == NULL) {
        return;
    }

    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const char *status_text = cJSON_IsString(st) ? st->valuestring : NULL;

    if (is_terminal(status_text)) {
        const char *err_text = cJSON_IsString(error) ? error->valuestring : NULL;
        if ((err_text == NULL || err_text[0] == '\0') &&
            strcmp(status_text, "failed") == 0) {
            err_text = "flow failed";
        }
        if (strcmp(status_text, "cancelled") == 0 &&
            (err_text == NULL || err_text[0] == '\0')) {
            err_text = "cancelled";
        }
        finish_slot(index, status_text, result, err_text);
    }

    cJSON_Delete(root);
}

static void poll_outstanding(const app_config_t *cfg, bool force)
{
    if (!app_config_hook_ready(cfg) || s_outstanding == 0) {
        return;
    }

    int64_t now = esp_timer_get_time() / 1000;

    if (force) {
        /* Rate-limit action-triggered polls, but always poll a fresh request. */
        if (s_last_poll_ms != 0 && now - s_last_poll_ms < FORCE_POLL_MIN_MS) {
            force = false;
        }
    }
    if (!force && s_last_poll_ms != 0 &&
        now - s_last_poll_ms < POLL_INTERVAL_MS) {
        return;
    }

    s_last_poll_ms = now;

    for (int i = 0; i < MAX_OUTSTANDING; i++) {
        if (!s_slots[i].active) {
            continue;
        }
        if (!force && s_slots[i].last_poll_ms != 0 &&
            now - s_slots[i].last_poll_ms < POLL_INTERVAL_MS) {
            continue;
        }
        poll_one(i, cfg);
    }
}

/* ------------------------------------------------------------- multipart -- */

static esp_err_t write_all(esp_http_client_handle_t client, const uint8_t *data,
                           size_t len)
{
    size_t written = 0;
    while (written < len) {
        int chunk = (int)((len - written) > 4096 ? 4096 : (len - written));
        int ret = esp_http_client_write(client, (const char *)(data + written), chunk);
        /* The transport may accept a partial chunk; advance by what it took. */
        if (ret <= 0) {
            return ESP_FAIL;
        }
        written += (size_t)ret;
    }
    return ESP_OK;
}

static esp_err_t post_recording(recorder_item_t *item, const app_config_t *cfg,
                                int *status_out, char *request_id,
                                size_t request_id_len)
{
    *status_out = 0;
    request_id[0] = '\0';

    char url[512];
    snprintf(url, sizeof(url), "%s/api/hooks/%s", cfg->server_url,
             cfg->dynamic_slug);

    char preamble[512];
    int pre_len = snprintf(preamble, sizeof(preamble),
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"state\"\r\n"
                           "\r\n"
                           "%s\r\n"
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"files\"; "
                           "filename=\"recording.wav\"\r\n"
                           "Content-Type: audio/wav\r\n"
                           "\r\n",
                           MULTIPART_BOUNDARY, cfg->state, MULTIPART_BOUNDARY);
    if (pre_len < 0 || pre_len >= (int)sizeof(preamble)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char epilogue[64];
    int epi_len = snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n",
                           MULTIPART_BOUNDARY);

    size_t file_size = recorder_item_size(item);
    int64_t total = (int64_t)pre_len + (int64_t)file_size + epi_len;
    if (total <= 0 || total > 25 * 1024 * 1024) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s",
             MULTIPART_BOUNDARY);
    char auth[APP_CONFIG_KEY_MAX + 8];
    build_auth(cfg, true, auth, sizeof(auth));

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, (int)total);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    err = write_all(client, (const uint8_t *)preamble, (size_t)pre_len);

    if (err == ESP_OK) {
        const char *path = recorder_item_path(item);
        if (path != NULL) {
            FILE *file = fopen(path, "rb");
            if (file == NULL) {
                err = ESP_FAIL;
            } else {
                uint8_t buffer[4096];
                size_t got;
                while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                    if (write_all(client, buffer, got) != ESP_OK) {
                        err = ESP_FAIL;
                        break;
                    }
                }
                fclose(file);
            }
        } else {
            size_t len = 0;
            const uint8_t *data = recorder_item_data(item, &len);
            if (data == NULL) {
                err = ESP_FAIL;
            } else {
                err = write_all(client, data, len);
            }
        }
    }

    if (err == ESP_OK) {
        err = write_all(client, (const uint8_t *)epilogue, (size_t)epi_len);
    }

    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        *status_out = esp_http_client_get_status_code(client);
        read_body(client);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }

    if (*status_out == 200 || *status_out == 201 || *status_out == 202 ||
        *status_out == 204) {
        cJSON *root = cJSON_Parse(s_body);
        if (root) {
            cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "requestId");
            if (cJSON_IsString(rid)) {
                snprintf(request_id, request_id_len, "%s", rid->valuestring);
            }
            cJSON_Delete(root);
        }
    }

    return ESP_OK;
}

static void drain_queue(const app_config_t *cfg)
{
    if (!app_config_hook_ready(cfg)) {
        return;
    }

    int attempts = 0;
    while (attempts++ < 32) {
        recorder_item_t *item = recorder_peek();
        if (item == NULL) {
            break;
        }

        set_message("sending");

        int status = 0;
        char request_id[APP_CONFIG_ID_MAX] = {0};
        esp_err_t err = post_recording(item, cfg, &status, request_id,
                                       sizeof(request_id));

        if (err == ESP_OK && status >= 200 && status < 300) {
            add_outstanding(request_id);

            /* Make the settings page show the call immediately; the poller
             * replaces this with the terminal status/result later. */
            if (request_id[0] != '\0') {
                app_result_t accepted = {0};
                snprintf(accepted.request_id, sizeof(accepted.request_id), "%s",
                         request_id);
                snprintf(accepted.status, sizeof(accepted.status), "accepted");
                app_config_set_result(&accepted);
            }

            recorder_item_sent(item);
            clear_error();
            continue;
        }

        if (err == ESP_OK && status >= 400 && status < 500) {
            char msg[NET_ERROR_MAX];
            snprintf(msg, sizeof(msg), "upload rejected (HTTP %d)", status);
            set_error(msg);

            app_result_t res = {0};
            snprintf(res.request_id, sizeof(res.request_id), "(local)");
            snprintf(res.status, sizeof(res.status), "failed");
            snprintf(res.error, sizeof(res.error), "%s", msg);
            app_config_set_result(&res);

            recorder_item_drop(item);
            continue;
        }

        char msg[NET_ERROR_MAX];
        if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "network error");
        } else {
            snprintf(msg, sizeof(msg), "server error (HTTP %d)", status);
        }
        set_error(msg);
        break;
    }
}

/* ----------------------------------------------------------------- todo -- */

static void fetch_todo(const app_config_t *cfg)
{
    if (!app_config_todo_ready(cfg)) {
        return;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "%s/api/v1/todos?done=false&limit=1&includeArchived=false",
             cfg->server_url);

    char auth[APP_CONFIG_KEY_MAX + 8];
    build_auth(cfg, false, auth, sizeof(auth));

    int status = 0;
    esp_err_t err = http_get(url, auth, &status);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "todo fetch failed (%s, HTTP %d)", esp_err_to_name(err),
                 status);
        return;
    }

    cJSON *root = cJSON_Parse(s_body);
    if (root == NULL) {
        return;
    }

    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    net_todo_t todo = {0};

    if (cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) {
        cJSON *first = cJSON_GetArrayItem(items, 0);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(first, "id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(first, "name");
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(first, "description");
        cJSON *due = cJSON_GetObjectItemCaseSensitive(first, "doByDate");
        cJSON *done = cJSON_GetObjectItemCaseSensitive(first, "done");

        todo.valid = true;
        if (cJSON_IsNumber(id)) {
            snprintf(todo.id, sizeof(todo.id), "%d", id->valueint);
        } else if (cJSON_IsString(id)) {
            snprintf(todo.id, sizeof(todo.id), "%s", id->valuestring);
        }

        if (cJSON_IsString(name) && name->valuestring[0] != '\0') {
            snprintf(todo.name, sizeof(todo.name), "%s", name->valuestring);
        } else if (cJSON_IsString(desc)) {
            snprintf(todo.name, sizeof(todo.name), "%s", desc->valuestring);
        } else {
            snprintf(todo.name, sizeof(todo.name), "(untitled todo)");
        }

        if (cJSON_IsString(due)) {
            snprintf(todo.due, sizeof(todo.due), "%s", due->valuestring);
        }
        todo.done = cJSON_IsTrue(done);
    } else {
        todo.valid = false;
        todo.empty = true;
        snprintf(todo.name, sizeof(todo.name), "All clear");
    }

    cJSON_Delete(root);

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_todo = todo;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void patch_todo(const char *id, bool done)
{
    if (id == NULL || id[0] == '\0') {
        return;
    }

    app_config_t cfg;
    app_config_get(&cfg);
    if (!app_config_todo_ready(&cfg)) {
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/todos/%s", cfg.server_url, id);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_PATCH,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return;
    }

    char auth[APP_CONFIG_KEY_MAX + 8];
    build_auth(&cfg, false, auth, sizeof(auth));
    const char *body = done ? "{\"done\":true}" : "{\"done\":false}";

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || (status != 200 && status != 204)) {
        char msg[NET_ERROR_MAX];
        snprintf(msg, sizeof(msg), "todo update failed (HTTP %d)", status);
        set_error(msg);
        ESP_LOGW(TAG, "%s", msg);
    } else {
        clear_error();
        ESP_LOGI(TAG, "todo %s -> done=%d", id, done);
    }
}

/* --------------------------------------------------------------- worker -- */

static void run_refresh(bool force_poll, bool fetch)
{
    app_config_t cfg;
    app_config_get(&cfg);

    bool connected = wifi_manager_sta_connected();
    bool any = app_config_any_ready(&cfg);

    if (!any) {
        set_message("not configured");
        return;
    }
    if (!connected) {
        set_message("offline");
        return;
    }

    drain_queue(&cfg);
    poll_outstanding(&cfg, force_poll);
    if (fetch) {
        fetch_todo(&cfg);
    }

    if (s_error[0] != '\0') {
        set_message("error");
    } else if (s_outstanding > 0) {
        set_message("pending");
    } else {
        set_message("idle");
    }
}

static void worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        net_msg_t msg;
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_inflight = true;

        if (msg.kind == MSG_PATCH) {
            clear_error();
            patch_todo(msg.todo_id, msg.done);
            run_refresh(true, true);
        } else if (msg.kind == MSG_SEND_ONLY) {
            run_refresh(true, false);
        } else {
            run_refresh(msg.force_poll, true);
        }

        s_inflight = false;
        notify();
    }
}

static void poll_timer_cb(void *arg)
{
    (void)arg;
    if (s_queue == NULL) {
        return;
    }
    net_msg_t msg = {
        .kind = MSG_REFRESH,
        .force_poll = false,
    };
    xQueueSend(s_queue, &msg, 0);
}

/* --------------------------------------------------------------- public -- */

esp_err_t net_client_init(void)
{
    s_body = malloc(BODY_BUF_SIZE);
    if (s_body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(net_msg_t));
    if (s_lock == NULL || s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(worker_task, "net", 12288, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = poll_timer_cb,
        .name = "net_poll",
    };
    esp_timer_handle_t timer;
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, POLL_TIMER_US);
    }

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

void net_client_set_event_callback(net_event_cb_t cb)
{
    s_event_cb = cb;
}

void net_client_kick(net_kick_reason_t reason)
{
    if (s_queue == NULL) {
        return;
    }
    net_msg_t msg = {
        .kind = reason == NET_KICK_RECORDING ? MSG_SEND_ONLY : MSG_REFRESH,
        .force_poll = true,
    };
    xQueueSend(s_queue, &msg, 0);
}

esp_err_t net_client_patch_done(const char *todo_id, bool done)
{
    if (todo_id == NULL || todo_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    net_msg_t msg = {
        .kind = MSG_PATCH,
        .force_poll = true,
        .done = done,
    };
    snprintf(msg.todo_id, sizeof(msg.todo_id), "%s", todo_id);
    return xQueueSend(s_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void net_client_get_todo(net_todo_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    *out = s_todo;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

bool net_client_is_busy(void)
{
    return s_inflight || s_outstanding > 0;
}

void net_client_get_status(net_status_t *out)
{
    if (out == NULL) {
        return;
    }

    app_config_t cfg;
    app_config_get(&cfg);

    memset(out, 0, sizeof(*out));
    out->configured = app_config_any_ready(&cfg);
    out->todo_ready = app_config_todo_ready(&cfg);
    out->hook_ready = app_config_hook_ready(&cfg);
    out->connected = wifi_manager_sta_connected();
    out->busy = net_client_is_busy();

    recorder_status_t rec;
    recorder_get_status(&rec);
    out->queued = rec.queued;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    out->todo = s_todo;
    snprintf(out->message, sizeof(out->message), "%s", s_message);
    snprintf(out->error, sizeof(out->error), "%s", s_error);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}
