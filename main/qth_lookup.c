#include "qth_lookup.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "cJSON.h"

#include "app_settings.h"
#include "ui_async.h"
#include "audio_ws.h"

static const char *TAG = "qth_lookup";

/* -------------------------------------------------------------------------- */
/* Config                                                                     */
/* -------------------------------------------------------------------------- */

#define QTH_LOOKUP_URL_MAX          160
#define QTH_LOOKUP_GRID_MAX         16
#define QTH_LOOKUP_RESP_MAX         768
#define QTH_LOOKUP_ADDR_MAX         96

#define QTH_LOOKUP_CACHE_SIZE       20

#define QTH_LOOKUP_INITIAL_DELAY_MS     1500
#define QTH_LOOKUP_AUDIO_WAIT_RETRY     2
#define QTH_LOOKUP_AUDIO_WAIT_MS        500

#define QTH_LOOKUP_FAIL_COOLDOWN_MS     60000   /* 失败冷却时间 */

#define QTH_NVS_NAMESPACE          "qth"
#define QTH_NVS_KEY_CACHE          "cache"
#define QTH_CACHE_STORE_MAGIC      0x51544831U
#define QTH_CACHE_STORE_VERSION    1

// Worker 配置
#define QTH_WORKER_STACK_SIZE      5120
#define QTH_REQUEST_QUEUE_SIZE     8

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    char grid[QTH_LOOKUP_GRID_MAX];
    bool allow_stale_cache;   /* true = 音频开启，需要等待；false = 直接查 */
} qth_request_t;

typedef struct {
    char grid[QTH_LOOKUP_GRID_MAX];
    char addr[QTH_LOOKUP_ADDR_MAX];
    uint32_t used_tick;
} qth_cache_item_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t cache_size;
    uint32_t tick;
    qth_cache_item_t items[QTH_LOOKUP_CACHE_SIZE];
} qth_cache_store_t;

/* -------------------------------------------------------------------------- */
/* Static variables                                                           */
/* -------------------------------------------------------------------------- */

static portMUX_TYPE s_qth_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_qth_inited = false;
static QueueHandle_t s_qth_request_queue = NULL;
static TaskHandle_t s_qth_worker_handle = NULL;

static qth_cache_item_t s_qth_cache[QTH_LOOKUP_CACHE_SIZE];
static uint32_t s_qth_cache_tick = 0;

static char s_active_grid[QTH_LOOKUP_GRID_MAX] = {0};

/* 失败冷却 */
static char s_last_failed_grid[QTH_LOOKUP_GRID_MAX] = {0};
static TickType_t s_last_failed_tick = 0;

/* -------------------------------------------------------------------------- */
/* Grid helpers                                                               */
/* -------------------------------------------------------------------------- */

static bool qth_grid_is_valid(const char *grid)
{
    if (!grid) return false;
    size_t len = strlen(grid);
    if (len < 4 || len > 10) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)grid[i])) return false;
    }
    return true;
}

static void qth_grid_normalize(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c)) dst[j++] = c;
    }
    dst[j] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Failed cooldown                                                            */
/* -------------------------------------------------------------------------- */

static void qth_lookup_mark_failed(const char *grid)
{
    if (!grid || !grid[0]) return;
    portENTER_CRITICAL(&s_qth_mux);
    snprintf(s_last_failed_grid, sizeof(s_last_failed_grid), "%s", grid);
    s_last_failed_tick = xTaskGetTickCount();
    portEXIT_CRITICAL(&s_qth_mux);
    ESP_LOGW(TAG, "qth mark failed grid=%s", grid);
}

static bool qth_lookup_in_fail_cooldown(const char *grid)
{
    if (!grid || !grid[0]) return false;
    bool hit = false;
    portENTER_CRITICAL(&s_qth_mux);
    if (s_last_failed_grid[0] && strcmp(s_last_failed_grid, grid) == 0) {
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed = (uint32_t)((now - s_last_failed_tick) * portTICK_PERIOD_MS);
        if (elapsed < QTH_LOOKUP_FAIL_COOLDOWN_MS) hit = true;
    }
    portEXIT_CRITICAL(&s_qth_mux);
    return hit;
}

/* -------------------------------------------------------------------------- */
/* JSON parse                                                                 */
/* -------------------------------------------------------------------------- */

static const char *qth_json_get_zh(cJSON *root, const char *name)
{
    if (!root || !name) return NULL;
    cJSON *obj = cJSON_GetObjectItem(root, name);
    if (!cJSON_IsObject(obj)) return NULL;
    cJSON *zh = cJSON_GetObjectItem(obj, "zh");
    if (cJSON_IsString(zh) && zh->valuestring && zh->valuestring[0]) {
        return zh->valuestring;
    }
    return NULL;
}

static bool qth_parse_address(const char *json, char *out, size_t out_size)
{
    if (!json || !out || out_size == 0) return false;
    out[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "cJSON_Parse failed");
        return false;
    }
    const char *admin1 = qth_json_get_zh(root, "admin1");
    const char *city   = qth_json_get_zh(root, "city");
    if (!admin1 && !city) {
        cJSON_Delete(root);
        return false;
    }
    snprintf(out, out_size, "%s%s%s",
             admin1 ? admin1 : "",
             (admin1 && city) ? " " : "",
             city ? city : "");
    cJSON_Delete(root);
    return out[0] != '\0';
}

/* -------------------------------------------------------------------------- */
/* Active grid & Cache                                                        */
/* -------------------------------------------------------------------------- */

static void qth_set_active_grid(const char *grid)
{
    portENTER_CRITICAL(&s_qth_mux);
    if (grid && grid[0]) {
        snprintf(s_active_grid, sizeof(s_active_grid), "%s", grid);
    } else {
        s_active_grid[0] = '\0';
    }
    portEXIT_CRITICAL(&s_qth_mux);
}

static bool qth_is_active_grid(const char *grid)
{
    bool active = false;
    portENTER_CRITICAL(&s_qth_mux);
    if (grid && grid[0] && s_active_grid[0] && strcmp(grid, s_active_grid) == 0) {
        active = true;
    }
    portEXIT_CRITICAL(&s_qth_mux);
    return active;
}

static bool qth_cache_get(const char *grid, char *addr, size_t addr_size)
{
    if (!grid || !addr || addr_size == 0) return false;
    bool found = false;
    portENTER_CRITICAL(&s_qth_mux);
    for (int i = 0; i < QTH_LOOKUP_CACHE_SIZE; i++) {
        if (s_qth_cache[i].grid[0] && strcmp(s_qth_cache[i].grid, grid) == 0) {
            snprintf(addr, addr_size, "%s", s_qth_cache[i].addr);
            s_qth_cache_tick++;
            s_qth_cache[i].used_tick = s_qth_cache_tick;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_qth_mux);
    return found;
}

static bool qth_cache_put(const char *grid, const char *addr)
{
    if (!grid || !grid[0] || !addr || !addr[0]) return false;
    bool changed = false;
    portENTER_CRITICAL(&s_qth_mux);
    s_qth_cache_tick++;
    for (int i = 0; i < QTH_LOOKUP_CACHE_SIZE; i++) {
        if (s_qth_cache[i].grid[0] && strcmp(s_qth_cache[i].grid, grid) == 0) {
            if (strcmp(s_qth_cache[i].addr, addr) != 0) {
                snprintf(s_qth_cache[i].addr, sizeof(s_qth_cache[i].addr), "%s", addr);
                changed = true;
            }
            s_qth_cache[i].used_tick = s_qth_cache_tick;
            portEXIT_CRITICAL(&s_qth_mux);
            return changed;
        }
    }
    for (int i = 0; i < QTH_LOOKUP_CACHE_SIZE; i++) {
        if (s_qth_cache[i].grid[0] == '\0') {
            snprintf(s_qth_cache[i].grid, sizeof(s_qth_cache[i].grid), "%s", grid);
            snprintf(s_qth_cache[i].addr, sizeof(s_qth_cache[i].addr), "%s", addr);
            s_qth_cache[i].used_tick = s_qth_cache_tick;
            changed = true;
            portEXIT_CRITICAL(&s_qth_mux);
            return changed;
        }
    }
    int oldest = 0;
    uint32_t oldest_tick = s_qth_cache[0].used_tick;
    for (int i = 1; i < QTH_LOOKUP_CACHE_SIZE; i++) {
        if (s_qth_cache[i].used_tick < oldest_tick) {
            oldest = i;
            oldest_tick = s_qth_cache[i].used_tick;
        }
    }
    snprintf(s_qth_cache[oldest].grid, sizeof(s_qth_cache[oldest].grid), "%s", grid);
    snprintf(s_qth_cache[oldest].addr, sizeof(s_qth_cache[oldest].addr), "%s", addr);
    s_qth_cache[oldest].used_tick = s_qth_cache_tick;
    changed = true;
    portEXIT_CRITICAL(&s_qth_mux);
    return changed;
}

static void qth_cache_load_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(QTH_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "qth nvs cache not found: %s", esp_err_to_name(ret));
        return;
    }

    qth_cache_store_t *store = calloc(1, sizeof(qth_cache_store_t));
    if (!store) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "alloc qth cache store failed");
        return;
    }

    size_t len = sizeof(qth_cache_store_t);
    ret = nvs_get_blob(nvs, QTH_NVS_KEY_CACHE, store, &len);
    nvs_close(nvs);

    if (ret != ESP_OK || len != sizeof(qth_cache_store_t) ||
        store->magic != QTH_CACHE_STORE_MAGIC ||
        store->version != QTH_CACHE_STORE_VERSION ||
        store->cache_size != QTH_LOOKUP_CACHE_SIZE) {
        ESP_LOGW(TAG, "qth nvs cache invalid");
        free(store);
        return;
    }

    portENTER_CRITICAL(&s_qth_mux);
    memcpy(s_qth_cache, store->items, sizeof(s_qth_cache));
    s_qth_cache_tick = store->tick;
    portEXIT_CRITICAL(&s_qth_mux);

    free(store);

    int count = 0;
    for (int i = 0; i < QTH_LOOKUP_CACHE_SIZE; i++) {
        if (s_qth_cache[i].grid[0]) count++;
    }
    ESP_LOGI(TAG, "qth cache loaded from nvs, count=%d", count);
}

static void qth_cache_save_to_nvs(void)
{
    qth_cache_store_t *store = calloc(1, sizeof(qth_cache_store_t));
    if (!store) {
        ESP_LOGW(TAG, "alloc qth cache store for save failed");
        return;
    }

    store->magic = QTH_CACHE_STORE_MAGIC;
    store->version = QTH_CACHE_STORE_VERSION;
    store->cache_size = QTH_LOOKUP_CACHE_SIZE;

    portENTER_CRITICAL(&s_qth_mux);
    store->tick = s_qth_cache_tick;
    memcpy(store->items, s_qth_cache, sizeof(s_qth_cache));
    portEXIT_CRITICAL(&s_qth_mux);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(QTH_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        free(store);
        return;
    }

    ret = nvs_set_blob(nvs, QTH_NVS_KEY_CACHE, store, sizeof(qth_cache_store_t));
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    free(store);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "qth cache saved to nvs");
    } else {
        ESP_LOGW(TAG, "qth cache save failed: %s", esp_err_to_name(ret));
    }
}

/* -------------------------------------------------------------------------- */
/* Audio & Resource checks                                                    */
/* -------------------------------------------------------------------------- */

static bool qth_lookup_audio_busy(void)
{
    return audio_ws_audio_is_enabled();   /* true = 音频开启，需要等待 */
}

static bool qth_lookup_wait_before_http(const char *grid, bool allow_stale_cache)
{
    vTaskDelay(pdMS_TO_TICKS(QTH_LOOKUP_INITIAL_DELAY_MS));

    const app_settings_t *cfg = app_settings_get();
    if (!cfg || !cfg->qth_lookup_enabled) return false;

    if (!allow_stale_cache && !qth_is_active_grid(grid)) return false;

    /* 音频关闭时直接查，无需等待 */
    if (!allow_stale_cache) {
        return true;
    }

    /* 音频开启时等待空闲 */
    for (int i = 0; i < QTH_LOOKUP_AUDIO_WAIT_RETRY; i++) {
        if (!qth_is_active_grid(grid)) return false;
        if (!qth_lookup_audio_busy()) return true;   /* 音频已关闭 */
        vTaskDelay(pdMS_TO_TICKS(QTH_LOOKUP_AUDIO_WAIT_MS));
        cfg = app_settings_get();
        if (!cfg || !cfg->qth_lookup_enabled) return false;
    }
    return false;   /* 仍忙 → 重新入队 */
}

/* -------------------------------------------------------------------------- */
/* Worker task                                                                */
/* -------------------------------------------------------------------------- */

static void qth_worker_task(void *arg)
{
    (void)arg;
    qth_request_t req;

    ESP_LOGI(TAG, "qth worker task started");

    while (1) {
        if (xQueueReceive(s_qth_request_queue, &req, pdMS_TO_TICKS(3000)) != pdPASS) {
            continue;
        }

        const app_settings_t *cfg = app_settings_get();
        if (!cfg || !cfg->qth_lookup_enabled) continue;

        if (!qth_grid_is_valid(req.grid)) continue;

        if (qth_lookup_in_fail_cooldown(req.grid)) {
            ESP_LOGI(TAG, "qth skip failed cooldown: %s", req.grid);
            continue;
        }

        /* 缓存检查 */
        char cached_addr[QTH_LOOKUP_ADDR_MAX];
        if (qth_cache_get(req.grid, cached_addr, sizeof(cached_addr))) {
            if (qth_is_active_grid(req.grid)) {
                ui_async_update_qth_address(cached_addr);
            }
            continue;
        }

        /* 等待合适时机 */
        if (!qth_lookup_wait_before_http(req.grid, req.allow_stale_cache)) {
            vTaskDelay(pdMS_TO_TICKS(2000));   /* 背压，避免高频循环 */
            xQueueSendToBack(s_qth_request_queue, &req, 0);
            continue;
        }

        /* 再次检查缓存 */
        if (qth_cache_get(req.grid, cached_addr, sizeof(cached_addr))) {
            if (qth_is_active_grid(req.grid)) {
                ui_async_update_qth_address(cached_addr);
            }
            continue;
        }

        if (!req.allow_stale_cache && !qth_is_active_grid(req.grid)) continue;

        /* HTTP 查询 */
        char url[QTH_LOOKUP_URL_MAX];
        snprintf(url, sizeof(url), "http://excavation.com:18081/api/grid/%s", req.grid);

        char resp[QTH_LOOKUP_RESP_MAX] = {0};
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 5000,
            .buffer_size = 512,
            .buffer_size_tx = 256,
        };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            qth_lookup_mark_failed(req.grid);
            continue;
        }

        esp_err_t ret = esp_http_client_open(client, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(ret));
            esp_http_client_cleanup(client);
            qth_lookup_mark_failed(req.grid);
            continue;
        }

        int content_length = esp_http_client_fetch_headers(client);
        int total = 0;
        while (total < (int)sizeof(resp) - 1) {
            int read_len = esp_http_client_read(client, resp + total, sizeof(resp) - 1 - total);
            if (read_len <= 0) break;
            total += read_len;
        }
        resp[total] = '\0';

        int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (status != 200 || total <= 0) {
            qth_lookup_mark_failed(req.grid);
            continue;
        }

        char addr[QTH_LOOKUP_ADDR_MAX];
        if (qth_parse_address(resp, addr, sizeof(addr))) {
            bool changed = qth_cache_put(req.grid, addr);
            if (changed) qth_cache_save_to_nvs();

            if (qth_is_active_grid(req.grid)) {
                ui_async_update_qth_address(addr);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void qth_lookup_init(void)
{
    if (s_qth_inited) return;

    memset(s_qth_cache, 0, sizeof(s_qth_cache));
    s_qth_cache_tick = 0;
    qth_cache_load_from_nvs();

    if (!s_qth_request_queue) {
        s_qth_request_queue = xQueueCreate(QTH_REQUEST_QUEUE_SIZE, sizeof(qth_request_t));
    }
    if (!s_qth_worker_handle && s_qth_request_queue) {
        xTaskCreate(qth_worker_task, "qth_worker", QTH_WORKER_STACK_SIZE,
                    NULL, 2, &s_qth_worker_handle);
    }

    s_qth_inited = true;
    ESP_LOGI(TAG, "qth lookup init done");
}

void qth_lookup_request_grid(const char *grid)
{
    qth_lookup_init();

    const app_settings_t *cfg = app_settings_get();
    if (!cfg || !cfg->qth_lookup_enabled) {
        qth_set_active_grid(NULL);
        ui_async_update_qth_address(NULL);
        return;
    }

    char norm[QTH_LOOKUP_GRID_MAX];
    qth_grid_normalize(norm, sizeof(norm), grid);
    if (!qth_grid_is_valid(norm)) return;

    qth_set_active_grid(norm);

    char cached_addr[QTH_LOOKUP_ADDR_MAX];
    if (qth_cache_get(norm, cached_addr, sizeof(cached_addr))) {
        if (qth_is_active_grid(norm)) {
            ui_async_update_qth_address(cached_addr);
        }
        return;
    }

    ui_async_update_qth_address(NULL);

    /* 失败冷却检查 */
    if (qth_lookup_in_fail_cooldown(norm)) {
        ESP_LOGI(TAG, "qth lookup skip failed cooldown: %s", norm);
        return;
    }

    qth_request_t req;
    snprintf(req.grid, sizeof(req.grid), "%s", norm);
    req.allow_stale_cache = qth_lookup_audio_busy();   /* 音频开启 → 需要等待 */

    xQueueSendToBack(s_qth_request_queue, &req, 0);
}
