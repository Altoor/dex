#include "recorder.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "storage.h"

static const char *TAG = "rec";

#define SAMPLE_RATE        16000
#define MONO_BYTES_PER_SEC (SAMPLE_RATE * 2)
#define MAX_PCM_BYTES      (MONO_BYTES_PER_SEC * (RECORDER_MAX_MS / 1000))
#define STEREO_CHUNK       4096
#define MONO_CHUNK         (STEREO_CHUNK / 2)
#define RAM_START_BYTES    (64 * 1024)
#define RAM_MAX_BYTES      (384 * 1024)
#define REC_DIR            STORAGE_ROOT "/dex"
#define MAX_SCAN_ITEMS     64

struct recorder_item {
    struct recorder_item *next;
    char path[RECORDER_PATH_MAX];
    uint8_t *data;
    size_t size;
};

typedef enum {
    REC_CMD_START = 0,
    REC_CMD_STOP,
    REC_CMD_CANCEL,
} rec_cmd_t;

typedef struct {
    bool to_file;
    FILE *file;
    char part_path[RECORDER_PATH_MAX];
    char final_path[RECORDER_PATH_MAX];
    uint8_t *mem;
    size_t mem_len;
    size_t mem_cap;
    uint32_t data_bytes;
} capture_t;

static QueueHandle_t s_cmd_queue;
static SemaphoreHandle_t s_lock;
static recorder_event_cb_t s_event_cb;

static volatile bool s_available;
static volatile bool s_recording;
static volatile bool s_session_active;
static volatile uint32_t s_data_bytes;
static volatile uint32_t s_queued;

static recorder_item_t *s_head;
static recorder_item_t *s_tail;
static uint32_t s_next_seq = 1;

static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_if_t *s_codec_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static uint8_t *s_stereo;
static uint8_t *s_mono;

static void notify(void)
{
    if (s_event_cb) {
        s_event_cb();
    }
}

static uint32_t elapsed_ms(void)
{
    return s_data_bytes / (MONO_BYTES_PER_SEC / 1000);
}

/* ---------------------------------------------------------------- WAV ---- */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wav_header(uint8_t *h, uint32_t data_bytes)
{
    memcpy(h + 0, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1); /* PCM */
    put_le16(h + 22, 1); /* mono */
    put_le32(h + 24, SAMPLE_RATE);
    put_le32(h + 28, SAMPLE_RATE * 2);
    put_le16(h + 32, 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

/* -------------------------------------------------------------- queue ---- */

static void enqueue_item(recorder_item_t *item)
{
    item->next = NULL;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_tail) {
        s_tail->next = item;
    } else {
        s_head = item;
    }
    s_tail = item;
    s_queued++;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "queued recording (%u bytes, %u total)", (unsigned)item->size,
             (unsigned)s_queued);
    notify();
}

static void free_item(recorder_item_t *item)
{
    if (item == NULL) {
        return;
    }
    if (item->path[0] != '\0') {
        remove(item->path);
    }
    if (item->data) {
        heap_caps_free(item->data);
    }
    free(item);
}

static void remove_item(recorder_item_t *item)
{
    if (item == NULL) {
        return;
    }

    bool found = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    recorder_item_t *prev = NULL;
    for (recorder_item_t *it = s_head; it != NULL; prev = it, it = it->next) {
        if (it == item) {
            if (prev) {
                prev->next = it->next;
            } else {
                s_head = it->next;
            }
            if (s_tail == it) {
                s_tail = prev;
            }
            if (s_queued > 0) {
                s_queued--;
            }
            found = true;
            break;
        }
    }

    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    if (found) {
        free_item(item);
    }
    notify();
}

recorder_item_t *recorder_peek(void)
{
    recorder_item_t *item = NULL;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    item = s_head;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return item;
}

const char *recorder_item_path(const recorder_item_t *item)
{
    return item && item->path[0] != '\0' ? item->path : NULL;
}

const uint8_t *recorder_item_data(const recorder_item_t *item, size_t *len)
{
    if (item == NULL || item->data == NULL) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = item->size;
    }
    return item->data;
}

size_t recorder_item_size(const recorder_item_t *item)
{
    return item ? item->size : 0;
}

void recorder_item_sent(recorder_item_t *item)
{
    remove_item(item);
}

void recorder_item_drop(recorder_item_t *item)
{
    remove_item(item);
}

static recorder_item_t *item_from_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 44) {
        return NULL;
    }

    recorder_item_t *item = calloc(1, sizeof(*item));
    if (item == NULL) {
        return NULL;
    }
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->size = (size_t)st.st_size;
    return item;
}

/* Load any recordings left on the card from a previous session. */
static uint32_t scan_existing(void)
{
    if (!storage_available()) {
        return 1;
    }

    mkdir(REC_DIR, 0777);
    DIR *dir = opendir(REC_DIR);
    if (dir == NULL) {
        return 1;
    }

    struct {
        uint32_t seq;
        char path[RECORDER_PATH_MAX];
    } entries[MAX_SCAN_ITEMS];
    int count = 0;
    uint32_t max_seq = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);

        /* Drop partial files from an interrupted recording. */
        if (len > 5 && strcmp(name + len - 5, ".part") == 0 &&
            strncmp(name, "rec-", 4) == 0) {
            char stale[RECORDER_PATH_MAX];
            snprintf(stale, sizeof(stale), "%s/%s", REC_DIR, name);
            remove(stale);
            continue;
        }

        if (len < 8 || len > 30 || strncmp(name, "rec-", 4) != 0 ||
            strcmp(name + len - 4, ".wav") != 0) {
            continue;
        }

        unsigned seq = 0;
        if (sscanf(name, "rec-%u.wav", &seq) != 1) {
            continue;
        }

        if (seq > max_seq) {
            max_seq = seq;
        }
        if (count < MAX_SCAN_ITEMS) {
            entries[count].seq = seq;
            snprintf(entries[count].path, sizeof(entries[count].path), "%s/%s",
                     REC_DIR, name);
            count++;
        }
    }
    closedir(dir);

    /* Insertion sort by sequence number (small counts). */
    for (int i = 1; i < count; i++) {
        typeof(entries[0]) key = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].seq > key.seq) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }

    for (int i = 0; i < count; i++) {
        recorder_item_t *item = item_from_path(entries[i].path);
        if (item) {
            enqueue_item(item);
        }
    }

    if (count > 0) {
        ESP_LOGI(TAG, "recovered %d queued recording(s) from SD", count);
    }
    return max_seq + 1;
}

/* ------------------------------------------------------------ capture ---- */

static void capture_init(capture_t *cap)
{
    memset(cap, 0, sizeof(*cap));
}

static esp_err_t capture_begin(capture_t *cap, uint32_t seq)
{
    capture_init(cap);

    /* SD first: recordings survive power loss. */
    if (storage_available()) {
        mkdir(REC_DIR, 0777);
        snprintf(cap->part_path, sizeof(cap->part_path),
                 "%s/rec-%06u.wav.part", REC_DIR, (unsigned)seq);
        snprintf(cap->final_path, sizeof(cap->final_path),
                 "%s/rec-%06u.wav", REC_DIR, (unsigned)seq);

        cap->file = fopen(cap->part_path, "wb+");
        if (cap->file != NULL) {
            uint8_t header[44];
            wav_header(header, 0);
            if (fwrite(header, 1, sizeof(header), cap->file) != sizeof(header)) {
                fclose(cap->file);
                cap->file = NULL;
                remove(cap->part_path);
            } else {
                cap->to_file = true;
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "could not open %s; using RAM", cap->part_path);
    }

    /* RAM fallback only when no card is available. */
    size_t cap_bytes = RAM_START_BYTES;
    while (cap_bytes >= 16 * 1024) {
        cap->mem = heap_caps_malloc(cap_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (cap->mem) {
            cap->mem_cap = cap_bytes;
            return ESP_OK;
        }
        cap_bytes /= 2;
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t capture_write(capture_t *cap, const uint8_t *data, size_t len)
{
    if (cap->to_file) {
        if (fwrite(data, 1, len, cap->file) != len) {
            return ESP_FAIL;
        }
        cap->data_bytes += (uint32_t)len;
        return ESP_OK;
    }

    if (cap->mem_len + len <= cap->mem_cap) {
        memcpy(cap->mem + cap->mem_len, data, len);
        cap->mem_len += len;
        cap->data_bytes += (uint32_t)len;
        return ESP_OK;
    }

    size_t need = cap->mem_len + len;
    size_t new_cap = cap->mem_cap ? cap->mem_cap : RAM_START_BYTES;
    while (new_cap < need && new_cap < RAM_MAX_BYTES) {
        new_cap *= 2;
    }
    if (new_cap > RAM_MAX_BYTES) {
        new_cap = RAM_MAX_BYTES;
    }
    if (new_cap < need) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *grown = heap_caps_realloc(cap->mem, new_cap,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (grown == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cap->mem = grown;
    cap->mem_cap = new_cap;

    memcpy(cap->mem + cap->mem_len, data, len);
    cap->mem_len += len;
    cap->data_bytes += (uint32_t)len;
    return ESP_OK;
}

static void enqueue_ram(uint8_t *wav, size_t size)
{
    recorder_item_t *item = calloc(1, sizeof(*item));
    if (item == NULL) {
        heap_caps_free(wav);
        return;
    }
    item->data = wav;
    item->size = size;
    enqueue_item(item);
}

static void persist_ram_to_sd(uint8_t *wav, size_t size)
{
    uint32_t seq = s_next_seq++;
    char path[RECORDER_PATH_MAX];
    snprintf(path, sizeof(path), "%s/rec-%06u.wav", REC_DIR, (unsigned)seq);

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        enqueue_ram(wav, size);
        return;
    }

    bool ok = fwrite(wav, 1, size, file) == size;
    fclose(file);

    if (!ok) {
        remove(path);
        enqueue_ram(wav, size);
        return;
    }

    heap_caps_free(wav);

    recorder_item_t *item = calloc(1, sizeof(*item));
    if (item == NULL) {
        remove(path);
        return;
    }
    snprintf(item->path, sizeof(item->path), "%s", path);
    item->size = size;
    enqueue_item(item);
}

static void capture_finish(capture_t *cap, bool keep)
{
    if (cap->to_file) {
        if (cap->file == NULL) {
            return;
        }

        if (!keep || cap->data_bytes == 0) {
            fclose(cap->file);
            remove(cap->part_path);
            cap->file = NULL;
            return;
        }

        uint8_t header[44];
        wav_header(header, cap->data_bytes);
        fseek(cap->file, 0, SEEK_SET);
        bool ok = fwrite(header, 1, sizeof(header), cap->file) == sizeof(header);
        fflush(cap->file);
        fclose(cap->file);
        cap->file = NULL;

        if (!ok || rename(cap->part_path, cap->final_path) != 0) {
            ESP_LOGW(TAG, "finalising %s failed", cap->part_path);
            remove(cap->part_path);
            return;
        }

        recorder_item_t *item = item_from_path(cap->final_path);
        if (item) {
            enqueue_item(item);
        }
        return;
    }

    if (!keep || cap->mem == NULL || cap->mem_len == 0) {
        if (cap->mem) {
            heap_caps_free(cap->mem);
        }
        cap->mem = NULL;
        return;
    }

    /* Add the WAV header in front of the PCM data. */
    size_t total = cap->mem_len + 44;
    uint8_t *wav = heap_caps_realloc(cap->mem, total,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (wav == NULL) {
        heap_caps_free(cap->mem);
        cap->mem = NULL;
        ESP_LOGE(TAG, "out of RAM finalising recording");
        return;
    }
    memmove(wav + 44, wav, cap->mem_len);
    wav_header(wav, (uint32_t)cap->mem_len);
    cap->mem = NULL;

    /* If a card appeared (or was mounted late), persist for durability. */
    if (storage_available()) {
        persist_ram_to_sd(wav, total);
    } else {
        enqueue_ram(wav, total);
    }
}

/* -------------------------------------------------------------- codec ---- */

static esp_err_t codec_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG,
                        "create I2S RX");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = -1,
            .din = BOARD_I2S_DIN,
            .invert_flags = {false, false, false},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG,
                        "init I2S RX");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_FAIL, TAG, "create I2S data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle(),
        .clock_speed_hz = 400000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_FAIL, TAG, "create codec ctrl");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_FAIL, TAG, "create codec gpio");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .no_dac_ref = true,
        .mclk_div = 256,
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_FAIL, TAG, "create ES8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_FAIL, TAG, "create codec dev");

    return ESP_OK;
}

/* ----------------------------------------------------------- recording ---- */

static void do_record(void)
{
    s_session_active = true;
    s_data_bytes = 0;

    if (!s_available) {
        s_session_active = false;
        notify();
        return;
    }

    capture_t cap;
    uint32_t seq = s_next_seq++;
    esp_err_t err = capture_begin(&cap, seq);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot start capture: %s", esp_err_to_name(err));
        s_session_active = false;
        notify();
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = 256,
    };

    int ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed (%d)", ret);
        capture_finish(&cap, false);
        s_session_active = false;
        notify();
        return;
    }
    esp_codec_dev_set_in_gain(s_codec, 30.0f);

    if (s_stereo == NULL) {
        s_stereo = heap_caps_malloc(STEREO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_mono = heap_caps_malloc(MONO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_stereo == NULL || s_mono == NULL) {
        ESP_LOGE(TAG, "cannot allocate audio buffers");
        esp_codec_dev_close(s_codec);
        capture_finish(&cap, false);
        s_session_active = false;
        notify();
        return;
    }

    s_recording = true;
    notify();

    bool keep = true;
    rec_cmd_t cmd;
    int frames = STEREO_CHUNK / 4;

    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd == REC_CMD_CANCEL) {
                keep = false;
                break;
            }
            if (cmd == REC_CMD_STOP) {
                break;
            }
        }

        if (cap.data_bytes >= MAX_PCM_BYTES) {
            break;
        }

        ret = esp_codec_dev_read(s_codec, s_stereo, STEREO_CHUNK);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "I2S read failed (%d)", ret);
            break;
        }

        /* Interleaved stereo -> left-channel mono. */
        for (int i = 0; i < frames; i++) {
            s_mono[2 * i] = s_stereo[4 * i];
            s_mono[2 * i + 1] = s_stereo[4 * i + 1];
        }

        if (capture_write(&cap, s_mono, (size_t)frames * 2) != ESP_OK) {
            ESP_LOGW(TAG, "recording storage full");
            break;
        }
        s_data_bytes = cap.data_bytes;
    }

    esp_codec_dev_close(s_codec);
    s_recording = false;
    capture_finish(&cap, keep);
    s_data_bytes = 0;
    s_session_active = false;
    notify();
}

static void recorder_task(void *arg)
{
    (void)arg;

    for (;;) {
        rec_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd == REC_CMD_START) {
            do_record();
        }
        /* STOP/CANCEL while idle are harmless no-ops. */
    }
}

/* ------------------------------------------------------------- public ---- */

esp_err_t recorder_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(6, sizeof(rec_cmd_t));
    if (s_lock == NULL || s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_next_seq = scan_existing();

    /* The ES8311 needs its rail before the first I2C access. */
    board_audio_power(true);
    board_pa_enable(false);
    vTaskDelay(pdMS_TO_TICKS(40));

    esp_err_t err = codec_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "codec unavailable (%s); recording disabled",
                 esp_err_to_name(err));
        s_available = false;
    } else {
        s_available = true;
    }

    if (xTaskCreate(recorder_task, "recorder", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ready (%u queued, %s)", (unsigned)s_queued,
             s_available ? "mic ok" : "no mic");
    return ESP_OK;
}

void recorder_set_event_callback(recorder_event_cb_t cb)
{
    s_event_cb = cb;
}

bool recorder_available(void)
{
    return s_available;
}

esp_err_t recorder_start(void)
{
    if (!s_available) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }

    s_session_active = true;
    rec_cmd_t cmd = REC_CMD_START;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        s_session_active = false;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    if (!s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_cmd_t cmd = REC_CMD_STOP;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void recorder_cancel(void)
{
    if (!s_session_active) {
        return;
    }
    rec_cmd_t cmd = REC_CMD_CANCEL;
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void recorder_get_status(recorder_status_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->available = s_available;
    out->recording = s_recording;
    out->elapsed_ms = elapsed_ms();
    out->max_ms = RECORDER_MAX_MS;
    out->queued = s_queued;
}
