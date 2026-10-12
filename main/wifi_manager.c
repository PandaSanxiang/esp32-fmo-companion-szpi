/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file wifi_manager.c
 * @brief WiFi STA 连接、状态查询、扫描与省电恢复实现。
 *
 * 本模块负责：
 * - 初始化 ESP-NETIF / ESP-EVENT / WiFi STA；
 * - 根据 app_settings 中保存的 WiFi 配置连接 AP；
 * - 处理 WiFi 与 IP 事件；
 * - 断线自动重连；
 * - 获取 RSSI；
 * - 同步扫描附近 AP；
 * - 在省电模式下停止 WiFi，并在退出省电后恢复。
 */

#include "wifi_manager.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

/* Project headers ---------------------------------------------------------- */
#include "app_power_save.h"
#include "app_settings.h"
#include "audio_ws.h"
#include "ui_async.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "wifi_manager";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 最大重连次数。
 */
#define WIFI_MAXIMUM_RETRY       5

/**
 * @brief WiFi SSID 最大字节数。
 *
 * 802.11 SSID 最大 32 字节。
 */
#define WIFI_SSID_MAX_BYTES      32

/**
 * @brief WiFi 密码最大字节数。
 */
#define WIFI_PASSWORD_MAX_BYTES  63

/**
 * @brief 自动切换 WiFi 任务栈大小。
 */
#define WIFI_AUTO_SELECT_STACK_SIZE 4096

/**
 * @brief 自动切换 WiFi 任务优先级。
 */
#define WIFI_AUTO_SELECT_TASK_PRIORITY 4


/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi 底层是否已初始化。
 */
static bool s_wifi_inited = false;

/**
 * @brief WiFi 是否已经 start。
 */
static bool s_wifi_started = false;

/**
 * @brief 当前是否正在主动停止 WiFi。
 *
 * 用于阻止 WIFI_EVENT_STA_DISCONNECTED 中的自动重连。
 */
static bool s_wifi_stopping = false;

/**
 * @brief 当前是否已连接 AP 并获得 IP。
 */
static bool s_wifi_connected = false;

/**
 * @brief 当前是否有可用于自动连接的 WiFi 配置。
 */
static bool s_wifi_has_config = false;

/**
 * @brief 当前是否正在手动扫描 WiFi。
 *
 * 扫描期间阻止 DISCONNECTED 事件触发自动重连。
 */
static bool s_wifi_scanning = false;

/**
 * @brief 最近一次获取到的 RSSI。
 */
static int s_wifi_rssi = 0;

/**
 * @brief 当前 STA IP 字符串。
 */
static char s_wifi_ip_string[16] = "0.0.0.0";

/**
 * @brief 当前重连次数。
 */
static int s_retry_num = 0;

/**
 * @brief 当前正在尝试连接的 WiFi 槽位。
 *
 * -1 表示来自旧版 app_settings_t.wifi_ssid，或当前未知。
 */
static int s_wifi_current_profile_index = -1;

/**
 * @brief 最近一次断线原因。
 */
static int s_last_disconnect_reason = 0;

/**
 * @brief 自动选择可用 WiFi 的后台任务。
 */
static TaskHandle_t s_wifi_auto_select_task_handle = NULL;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void log_hex_bytes(const char *name,
                          const uint8_t *data,
                          size_t len);

static const char *wifi_disconnect_reason_to_name(int reason);

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);

static bool cstr_len_range(const char *s,
                           size_t min_len,
                           size_t max_len,
                           size_t *out_len);

static bool wifi_credentials_are_valid(const char *ssid,
                                       const char *password,
                                       size_t *out_ssid_len,
                                       size_t *out_password_len);

static esp_err_t wifi_manager_apply_sta_config(const char *ssid,
                                               const char *password,
                                               int profile_index);

static bool wifi_manager_load_initial_config(void);

static void wifi_manager_start_auto_select_task(void);

static void wifi_manager_auto_select_task(void *arg);

static void wifi_init_sta(void);

/* -------------------------------------------------------------------------- */
/* Debug helpers                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 打印字节序列，便于调试中文 SSID 等非 ASCII 文本。
 */
static void log_hex_bytes(const char *name,
                          const uint8_t *data,
                          size_t len)
{
    if (!name || !data) {
        return;
    }

    char buf[160];
    size_t pos = 0;

    memset(buf, 0, sizeof(buf));

    for (size_t i = 0; i < len && pos + 4 < sizeof(buf); i++) {
        pos += snprintf(buf + pos,
                        sizeof(buf) - pos,
                        "%02X ",
                        data[i]);
    }

    ESP_LOGI(TAG, "%s hex: %s", name, buf);
}

/**
 * @brief 将 WiFi 断开原因转换为字符串。
 *
 * 使用数字判断，避免不同 ESP-IDF 版本中个别枚举名不存在导致编译失败。
 */
static const char *wifi_disconnect_reason_to_name(int reason)
{
    switch (reason) {
    case 1:
        return "UNSPECIFIED";
    case 2:
        return "AUTH_EXPIRE";
    case 3:
        return "AUTH_LEAVE";
    case 4:
        return "ASSOC_EXPIRE";
    case 5:
        return "ASSOC_TOOMANY";
    case 6:
        return "NOT_AUTHED";
    case 7:
        return "NOT_ASSOCED";
    case 8:
        return "ASSOC_LEAVE";
    case 9:
        return "ASSOC_NOT_AUTHED";
    case 15:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:
        return "IE_IN_4WAY_DIFFERS";
    case 18:
        return "GROUP_CIPHER_INVALID";
    case 19:
        return "PAIRWISE_CIPHER_INVALID";
    case 20:
        return "AKMP_INVALID";
    case 21:
        return "UNSUPP_RSN_IE_VERSION";
    case 22:
        return "INVALID_RSN_IE_CAP";
    case 23:
        return "802_1X_AUTH_FAILED";
    case 24:
        return "CIPHER_SUITE_REJECTED";
    case 200:
        return "BEACON_TIMEOUT";
    case 201:
        return "NO_AP_FOUND";
    case 202:
        return "AUTH_FAIL";
    case 203:
        return "ASSOC_FAIL";
    case 204:
        return "HANDSHAKE_TIMEOUT";
    case 205:
        return "CONNECTION_FAIL";
    case 206:
        return "AP_TSF_RESET";
    case 207:
        return "ROAMING";
    default:
        return "UNKNOWN";
    }
}

/* -------------------------------------------------------------------------- */
/* WiFi profile helpers                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief 有界 C 字符串长度检查，避免无界 strlen 越界读。
 */
static bool cstr_len_range(const char *s,
                           size_t min_len,
                           size_t max_len,
                           size_t *out_len)
{
    if (!s) {
        return false;
    }

    const char *end = memchr(s, '\0', max_len + 1);
    if (!end) {
        return false;
    }

    size_t len = (size_t)(end - s);

    if (len < min_len || len > max_len) {
        return false;
    }

    if (out_len) {
        *out_len = len;
    }

    return true;
}

/**
 * @brief 检查 SSID / 密码是否适合写入 esp_wifi_config_t。
 */
static bool wifi_credentials_are_valid(const char *ssid,
                                       const char *password,
                                       size_t *out_ssid_len,
                                       size_t *out_password_len)
{
    size_t ssid_len = 0;
    size_t password_len = 0;

    if (!cstr_len_range(ssid, 1, WIFI_SSID_MAX_BYTES, &ssid_len)) {
        return false;
    }

    if (!password) {
        password = "";
    }

    if (!cstr_len_range(password, 0, WIFI_PASSWORD_MAX_BYTES, &password_len)) {
        return false;
    }

    if (out_ssid_len) {
        *out_ssid_len = ssid_len;
    }

    if (out_password_len) {
        *out_password_len = password_len;
    }

    return true;
}

/**
 * @brief 写入 STA 连接配置。
 *
 * profile_index 为 -1 时表示兼容旧版单 WiFi 配置。
 */
static esp_err_t wifi_manager_apply_sta_config(const char *ssid,
                                               const char *password,
                                               int profile_index)
{
    size_t ssid_len = 0;
    size_t password_len = 0;

    if (!wifi_credentials_are_valid(ssid,
                                    password,
                                    &ssid_len,
                                    &password_len)) {
        s_wifi_has_config = false;
        s_wifi_current_profile_index = -1;
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    /*
     * 中文 SSID 按 UTF-8 原始字节复制。
     */
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);

    if (password_len > 0) {
        memcpy(wifi_config.sta.password, password, password_len);
    }

    /*
     * 扫描所有信道，按信号选择 AP。
     */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    if (password_len > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    /*
     * 兼容 WPA2/WPA3 mixed。
     */
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_wifi_set_config failed: %s",
                 esp_err_to_name(ret));

        s_wifi_has_config = false;
        s_wifi_current_profile_index = -1;
        return ret;
    }

    s_wifi_has_config = true;
    s_wifi_current_profile_index = profile_index;

    ESP_LOGI(TAG,
             "wifi config applied, profile=%d, ssid=%s, ssid_len=%u, password_len=%u",
             profile_index,
             ssid,
             (unsigned)ssid_len,
             (unsigned)password_len);
    log_hex_bytes("ssid", (const uint8_t *)ssid, ssid_len);

    return ESP_OK;
}

/**
 * @brief 从 app_settings 中选择当前启动要连接的 WiFi。
 *
 * 优先使用多 WiFi profiles 的 active_index；
 * 如果没有 profiles，则兼容旧版 cfg->wifi_ssid / cfg->wifi_password。
 */
static bool wifi_manager_load_initial_config(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!cfg) {
        ESP_LOGE(TAG, "settings is null");
        s_wifi_has_config = false;
        s_wifi_current_profile_index = -1;
        return false;
    }

    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX];
    uint8_t active_index = 0;

    memset(profiles, 0, sizeof(profiles));

    size_t profile_count =
        app_settings_get_wifi_profiles(profiles,
                                       APP_WIFI_PROFILE_MAX,
                                       &active_index);

    if (profile_count > APP_WIFI_PROFILE_MAX) {
        profile_count = APP_WIFI_PROFILE_MAX;
    }

    if (active_index < profile_count &&
        profiles[active_index].used) {
        esp_err_t ret =
            wifi_manager_apply_sta_config(profiles[active_index].ssid,
                                          profiles[active_index].password,
                                          active_index);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "use active wifi profile %u: %s",
                     (unsigned)active_index,
                     profiles[active_index].ssid);
            return true;
        }

        ESP_LOGW(TAG,
                 "active wifi profile %u invalid: %s",
                 (unsigned)active_index,
                 esp_err_to_name(ret));
    }

    /*
     * active_index 无效时，使用第一个可用槽位。
     */
    for (size_t i = 0; i < profile_count; i++) {
        if (!profiles[i].used) {
            continue;
        }

        esp_err_t ret = wifi_manager_apply_sta_config(profiles[i].ssid,
                                                      profiles[i].password,
                                                      (int)i);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "use first valid wifi profile %u: %s",
                     (unsigned)i,
                     profiles[i].ssid);
            return true;
        }
    }

    /*
     * 兼容旧版单 WiFi 配置。
     */
    if (cfg->wifi_ssid[0] != '\0') {
        esp_err_t ret = wifi_manager_apply_sta_config(cfg->wifi_ssid,
                                                      cfg->wifi_password,
                                                      -1);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "use legacy wifi config: %s", cfg->wifi_ssid);
            return true;
        }

        ESP_LOGW(TAG,
                 "legacy wifi config invalid: %s",
                 esp_err_to_name(ret));
    }

    s_wifi_has_config = false;
    s_wifi_current_profile_index = -1;

    ESP_LOGW(TAG, "no valid wifi config");
    ui_async_update_status("未配置WiFi");

    return false;
}

/**
 * @brief 启动后台自动选择 WiFi 任务。
 */
static void wifi_manager_start_auto_select_task(void)
{
    if (s_wifi_auto_select_task_handle) {
        ESP_LOGI(TAG, "wifi auto select task already running");
        return;
    }

    if (s_wifi_stopping || app_power_save_is_active()) {
        ESP_LOGI(TAG, "skip wifi auto select because stopping/power save");
        return;
    }

    BaseType_t ok = xTaskCreate(wifi_manager_auto_select_task,
                                "wifi_auto_select",
                                WIFI_AUTO_SELECT_STACK_SIZE,
                                NULL,
                                WIFI_AUTO_SELECT_TASK_PRIORITY,
                                &s_wifi_auto_select_task_handle);

    if (ok != pdPASS) {
        s_wifi_auto_select_task_handle = NULL;
        ESP_LOGE(TAG, "create wifi auto select task failed");
        ui_async_update_status("WiFi连接失败");
    }
}

/**
 * @brief 后台扫描已保存 WiFi，选择当前环境中信号最好的可用槽位。
 */
static void wifi_manager_auto_select_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "wifi auto select task start");

    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX];
    uint8_t active_index = 0;

    memset(profiles, 0, sizeof(profiles));

    size_t profile_count =
        app_settings_get_wifi_profiles(profiles,
                                       APP_WIFI_PROFILE_MAX,
                                       &active_index);

    if (profile_count > APP_WIFI_PROFILE_MAX) {
        profile_count = APP_WIFI_PROFILE_MAX;
    }

    int used_count = 0;
    for (size_t i = 0; i < profile_count; i++) {
        if (profiles[i].used) {
            used_count++;
        }
    }

    if (used_count <= 0) {
        ESP_LOGW(TAG, "no saved wifi profiles for auto select");
        s_wifi_has_config = false;
        ui_async_update_status("未配置WiFi");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (used_count <= 1 && s_wifi_current_profile_index >= 0) {
        ESP_LOGW(TAG, "only current wifi profile is saved, no alternative");
        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (!s_wifi_started) {
        esp_err_t start_ret = esp_wifi_start();
        if (start_ret != ESP_OK &&
            start_ret != ESP_ERR_WIFI_NOT_STOPPED) {
            ESP_LOGW(TAG,
                     "esp_wifi_start for auto select failed: %s",
                     esp_err_to_name(start_ret));
            ui_async_update_status("WiFi连接失败");
            s_wifi_auto_select_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }

        s_wifi_started = true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ui_async_update_status("WiFi切换中");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 120,
    };

    s_wifi_scanning = true;

    esp_err_t disc_ret = esp_wifi_disconnect();
    if (disc_ret != ESP_OK &&
        disc_ret != ESP_ERR_WIFI_NOT_CONNECT &&
        disc_ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG,
                 "esp_wifi_disconnect before auto scan failed: %s",
                 esp_err_to_name(disc_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(150));

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "auto wifi scan start failed: %s",
                 esp_err_to_name(ret));

        s_wifi_scanning = false;
        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK || ap_count == 0) {
        ESP_LOGW(TAG,
                 "auto wifi scan found no ap or get num failed: %s",
                 esp_err_to_name(ret));

        s_wifi_scanning = false;
        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "alloc auto scan records failed");

        s_wifi_scanning = false;
        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint16_t record_count = ap_count;
    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "auto wifi scan get records failed: %s",
                 esp_err_to_name(ret));

        free(records);
        s_wifi_scanning = false;
        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    int best_index = -1;
    int best_rssi = -128;

    /*
     * 失败切换时跳过刚刚失败的槽位，避免密码错误时反复选择同一个 SSID。
     * 如果只有一个槽位，就不会切换，保留失败状态。
     */
    for (size_t i = 0; i < profile_count; i++) {
        if (!profiles[i].used) {
            continue;
        }

        if (used_count > 1 && (int)i == s_wifi_current_profile_index) {
            continue;
        }

        for (uint16_t j = 0; j < record_count; j++) {
            char scanned_ssid[WIFI_SSID_MAX_BYTES + 1];

            memset(scanned_ssid, 0, sizeof(scanned_ssid));
            memcpy(scanned_ssid, records[j].ssid, WIFI_SSID_MAX_BYTES);
            scanned_ssid[WIFI_SSID_MAX_BYTES] = '\0';

            if (strcmp(scanned_ssid, profiles[i].ssid) != 0) {
                continue;
            }

            if ((int)records[j].rssi > best_rssi) {
                best_rssi = records[j].rssi;
                best_index = (int)i;
            }
        }
    }

    free(records);

    s_wifi_scanning = false;

    if (best_index < 0) {
        ESP_LOGW(TAG,
                 "no alternative saved wifi found, last_reason=%d (%s)",
                 s_last_disconnect_reason,
                 wifi_disconnect_reason_to_name(s_last_disconnect_reason));

        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "auto select wifi profile %d: %s, rssi=%d",
             best_index,
             profiles[best_index].ssid,
             best_rssi);

    ret = app_settings_select_wifi_profile((uint8_t)best_index);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "select wifi profile %d failed: %s",
                 best_index,
                 esp_err_to_name(ret));
    }

    ret = wifi_manager_apply_sta_config(profiles[best_index].ssid,
                                        profiles[best_index].password,
                                        best_index);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "apply selected wifi profile failed: %s",
                 esp_err_to_name(ret));

        ui_async_update_status("WiFi连接失败");
        s_wifi_auto_select_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_retry_num = 0;

    ret = esp_wifi_connect();
    if (ret != ESP_OK &&
        ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG,
                 "esp_wifi_connect selected profile failed: %s",
                 esp_err_to_name(ret));

        ui_async_update_status("WiFi连接失败");
    } else {
        ui_async_update_status("WiFi连接中");
    }

    s_wifi_auto_select_task_handle = NULL;
    vTaskDelete(NULL);
}


/* -------------------------------------------------------------------------- */
/* WiFi event handling                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi / IP 事件处理函数。
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START");

        s_wifi_started = true;

        /*
        * 如果正在省电停止 WiFi，不要连接。
        */
        if (s_wifi_stopping || app_power_save_is_active()) {
            ESP_LOGW(TAG, "wifi start ignored because power save/stopping");
            return;
        }

        /*
        * 如果当前正在扫描，不要自动连接，避免扫描和连接抢状态。
        */
        if (s_wifi_scanning) {
            ESP_LOGI(TAG, "wifi start during scan, skip connect");
            return;
        }

        /*
        * 没有 WiFi 配置时，也要保持 WiFi driver started，
        * 这样设置页仍然可以扫描附近 WiFi。
        */
        if (!s_wifi_has_config) {
            ESP_LOGW(TAG, "wifi config empty/invalid, skip connect");
            ui_async_update_status("未配置WiFi");
            return;
        }

        esp_wifi_connect();

        ui_async_update_status("WiFi连接中");
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc =
            (wifi_event_sta_disconnected_t *)event_data;

        int reason = disc ? disc->reason : -1;
        s_last_disconnect_reason = reason;

        ESP_LOGW(TAG,
                 "WiFi disconnected, reason=%d (%s)",
                 reason,
                 wifi_disconnect_reason_to_name(reason));

        s_wifi_connected = false;
        s_wifi_rssi = 0;
        snprintf(s_wifi_ip_string, sizeof(s_wifi_ip_string), "0.0.0.0");

        /*
        * 扫描期间主动 disconnect，不要自动重连。
        */
        if (s_wifi_scanning) {
            ESP_LOGI(TAG, "WiFi disconnected for scan, no reconnect");
            ui_async_update_wifi_rssi(-127);
            return;
        }

        /*
        * 没有有效 WiFi 配置，不自动重连。
        */
        if (!s_wifi_has_config) {
            ESP_LOGW(TAG, "WiFi disconnected but no config, no reconnect");
            ui_async_update_wifi_rssi(-127);
            ui_async_update_status("未配置WiFi");
            return;
        }


        /*
         * 根据常见原因给出更明确的 UI 状态。
         */
        if (reason == 201) {
            ui_async_update_status("未找到WiFi");
        } else if (reason == 202 || reason == 15 || reason == 204) {
            ui_async_update_status("WiFi认证失败");
        } else if (reason == 200) {
            ui_async_update_status("WiFi信号异常");
        } else {
            ui_async_update_status("WiFi断开");
        }

        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            s_retry_num++;

            ESP_LOGI(TAG,
                     "retry to connect to the AP, attempt %d",
                     s_retry_num);

            /*
             * 注意：
             * 事件回调中不建议长时间阻塞。
             * 当前保留 300ms 延迟以避免 AP 刚踢下线就立即重连。
             * 如果后续需要更规范，可改为延迟重连任务或 esp_timer。
             */
            vTaskDelay(pdMS_TO_TICKS(300));

            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "connect failed, try another saved WiFi profile");
            ui_async_update_status("WiFi切换中");
            wifi_manager_start_auto_select_task();
        }

        return;
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        if (event) {
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_wifi_ip_string,
                     sizeof(s_wifi_ip_string),
                     IPSTR,
                     IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "got ip");
            snprintf(s_wifi_ip_string, sizeof(s_wifi_ip_string), "0.0.0.0");
        }

        s_wifi_connected = true;
        s_wifi_started = true;
        s_wifi_stopping = false;
        s_retry_num = 0;

        ui_async_update_status("WiFi已连接");

        /*
         * 正常模式下，WiFi 获取 IP 后启动/恢复 WebSocket。
         * audio_ws_start() 内部应具备防重复启动逻辑。
         */
        if (!app_power_save_is_active()) {
            audio_ws_start();
        }

        return;
    }
}

/* -------------------------------------------------------------------------- */
/* WiFi initialization                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化并启动 WiFi STA。
 *
 * 该函数只在首次启动时调用。
 */
static void wifi_init_sta(void)
{
    /*
     * 无论有没有 WiFi 配置，都初始化并启动 WiFi driver。
     * 这样即使保存的 WiFi 不存在/未配置，设置页也可以扫描附近 WiFi。
     */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler,
                                               NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler,
                                               NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /*
     * 2.4G 兼容模式。
     */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                                          WIFI_PROTOCOL_11B |
                                          WIFI_PROTOCOL_11G |
                                          WIFI_PROTOCOL_11N));

    /*
     * 固定 20 MHz，部分路由器/弱信号环境更稳。
     */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));

    s_wifi_has_config = false;
    s_wifi_current_profile_index = -1;

    /*
     * 检查保存的 WiFi 配置。
     *
     * 优先使用 app_settings 的多 WiFi profiles active 槽位；
     * 如果 profiles 不存在，则回退到旧版 cfg->wifi_ssid / cfg->wifi_password。
     */
    wifi_manager_load_initial_config();

    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_started = true;
    s_wifi_stopping = false;
    s_wifi_connected = false;
    s_retry_num = 0;

    /*
     * 关闭 WiFi 省电模式，提高连接稳定性和音频实时性。
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save disabled");

    ui_async_update_status("WiFi启动完成");
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: start / stop / restart                                  */
/* -------------------------------------------------------------------------- */

esp_err_t wifi_manager_start(void)
{
    if (s_wifi_inited) {
        if (s_wifi_started) {
            ESP_LOGW(TAG, "wifi already started");
            return ESP_OK;
        }

        /*
         * 已初始化但当前停止，直接 restart。
         */
        return wifi_manager_restart();
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG,
                 "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    s_wifi_inited = true;
    s_wifi_started = false;
    s_wifi_stopping = false;
    s_retry_num = 0;

    wifi_init_sta();

    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    ESP_LOGW(TAG, "wifi_manager_stop");

    if (!s_wifi_inited) {
        return ESP_OK;
    }

    /*
     * 设置停止标志，防止 DISCONNECTED 事件中自动重连。
     */
    s_wifi_stopping = true;
    s_wifi_connected = false;
    s_wifi_rssi = 0;
    s_retry_num = 0;

    ui_async_update_wifi_rssi(-127);

    /*
     * 如果 WiFi 本来就没启动，直接返回。
     */
    if (!s_wifi_started) {
        ui_async_update_status("WiFi已关闭");
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK &&
        ret != ESP_ERR_WIFI_NOT_CONNECT &&
        ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG,
                 "esp_wifi_disconnect failed: %s",
                 esp_err_to_name(ret));
    }

    ret = esp_wifi_stop();
    if (ret == ESP_OK ||
        ret == ESP_ERR_WIFI_NOT_STARTED ||
        ret == ESP_ERR_WIFI_NOT_INIT) {

        s_wifi_started = false;
        s_wifi_connected = false;

        ui_async_update_wifi_rssi(-127);
        ui_async_update_status("WiFi已关闭");

        return ESP_OK;
    }

    ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(ret));

    return ret;
}

esp_err_t wifi_manager_restart(void)
{
    ESP_LOGW(TAG, "wifi_manager_restart");

    if (!s_wifi_inited) {
        return wifi_manager_start();
    }

    if (s_wifi_started) {
        ESP_LOGW(TAG, "wifi already started");
        return ESP_OK;
    }

    /*
     * 退出省电恢复时，允许 STA_START 事件连接 AP。
     * 这里重新读取配置，避免省电期间用户修改了 WiFi 槽位。
     */
    s_wifi_stopping = false;
    s_wifi_connected = false;
    s_wifi_rssi = 0;
    s_retry_num = 0;

    wifi_manager_load_initial_config();

    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK) {
        s_wifi_started = true;
        ui_async_update_status("WiFi恢复中");
    } else if (ret == ESP_ERR_WIFI_NOT_STOPPED) {
        s_wifi_started = true;
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* -------------------------------------------------------------------------- */
/* Public interfaces: state query                                             */
/* -------------------------------------------------------------------------- */

bool wifi_manager_is_started(void)
{
    return s_wifi_started;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_connected;
}

int wifi_manager_get_rssi(void)
{
    if (!s_wifi_connected) {
        return 0;
    }

    wifi_ap_record_t ap_info;

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_wifi_rssi = ap_info.rssi;
        return s_wifi_rssi;
    }

    return 0;
}

const char *wifi_manager_get_ip_string(void)
{
    if (!s_wifi_connected) {
        return "0.0.0.0";
    }

    return s_wifi_ip_string;
}


/* -------------------------------------------------------------------------- */
/* Public interfaces: scan                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t wifi_manager_scan(wifi_scan_item_t *items,
                            int max_items,
                            int *out_count)
{
    if (!items || max_items <= 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;

    if (!s_wifi_inited || app_power_save_is_active()) {
        ESP_LOGW(TAG, "wifi not available, cannot scan");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 扫描不要求 WiFi 已连接，只要求 WiFi driver 已启动。
     * 如果当前没 start，先 start。
     */
    if (!s_wifi_started) {
        ESP_LOGW(TAG, "wifi not started, start for scan");

        s_wifi_stopping = false;
        s_wifi_scanning = true;

        esp_err_t start_ret = esp_wifi_start();
        if (start_ret != ESP_OK &&
            start_ret != ESP_ERR_WIFI_NOT_STOPPED) {
            s_wifi_scanning = false;

            ESP_LOGW(TAG,
                     "esp_wifi_start for scan failed: %s",
                     esp_err_to_name(start_ret));
            return start_ret;
        }

        s_wifi_started = true;

        /*
         * 等待 STA_START 事件和 driver 状态稳定。
         */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 50,
        .scan_time.active.max = 120,
    };

    ESP_LOGI(TAG, "start wifi scan");

    /*
     * 扫描前临时暂停自动重连。
     */
    s_wifi_scanning = true;

    /*
     * 如果当前正在连接/已连接，先断开，避免 scan_start 返回 WIFI_STATE。
     * 未连接时返回 ESP_ERR_WIFI_NOT_CONNECT 可以忽略。
     */
    esp_err_t disc_ret = esp_wifi_disconnect();
    if (disc_ret != ESP_OK &&
        disc_ret != ESP_ERR_WIFI_NOT_CONNECT &&
        disc_ret != ESP_ERR_WIFI_NOT_STARTED) {

        ESP_LOGW(TAG,
                 "esp_wifi_disconnect before scan failed: %s",
                 esp_err_to_name(disc_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(150));

    /*
     * 第二个参数 true 表示阻塞等待扫描完成。
     * 因此不要在 LVGL 线程中直接调用该函数。
     */
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wifi scan start failed: %s",
                 esp_err_to_name(ret));

        s_wifi_scanning = false;

        /*
         * 扫描失败后，如果有配置，恢复连接。
         */
        if (s_wifi_has_config &&
            !s_wifi_stopping &&
            !app_power_save_is_active()) {
            s_retry_num = 0;
            esp_wifi_connect();
        }

        return ret;
    }

    uint16_t ap_count = 0;

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wifi scan get ap num failed: %s",
                 esp_err_to_name(ret));

        s_wifi_scanning = false;

        if (s_wifi_has_config &&
            !s_wifi_stopping &&
            !app_power_save_is_active()) {
            s_retry_num = 0;
            esp_wifi_connect();
        }

        return ret;
    }

    if (ap_count == 0) {
        ESP_LOGI(TAG, "wifi scan found 0 ap");

        s_wifi_scanning = false;

        if (s_wifi_has_config &&
            !s_wifi_stopping &&
            !app_power_save_is_active()) {
            s_retry_num = 0;
            esp_wifi_connect();
        }

        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "alloc scan records failed");

        s_wifi_scanning = false;

        if (s_wifi_has_config &&
            !s_wifi_stopping &&
            !app_power_save_is_active()) {
            s_retry_num = 0;
            esp_wifi_connect();
        }

        return ESP_ERR_NO_MEM;
    }

    uint16_t record_count = ap_count;

    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        free(records);

        ESP_LOGW(TAG,
                 "wifi scan get records failed: %s",
                 esp_err_to_name(ret));

        s_wifi_scanning = false;

        if (s_wifi_has_config &&
            !s_wifi_stopping &&
            !app_power_save_is_active()) {
            s_retry_num = 0;
            esp_wifi_connect();
        }

        return ret;
    }

    /*
     * 只取前 max_items 个。
     */
    int count = 0;

    for (int i = 0; i < record_count && count < max_items; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }

        memset(&items[count], 0, sizeof(wifi_scan_item_t));

        memcpy(items[count].ssid, records[i].ssid, WIFI_SSID_MAX_BYTES);
        items[count].ssid[WIFI_SSID_MAX_BYTES] = '\0';

        items[count].rssi = records[i].rssi;
        items[count].authmode = records[i].authmode;
        items[count].channel = records[i].primary;

        ESP_LOGI(TAG,
                 "scan[%d]: ssid=%s rssi=%d auth=%d ch=%d",
                 count,
                 items[count].ssid,
                 items[count].rssi,
                 items[count].authmode,
                 items[count].channel);

        count++;
    }

    free(records);

    *out_count = count;

    ESP_LOGI(TAG, "wifi scan done, count=%d", count);

    /*
     * 扫描结束，允许恢复自动连接。
     */
    s_wifi_scanning = false;

    /*
     * 如果有保存的 WiFi 配置，扫描结束后恢复连接。
     */
    if (s_wifi_has_config &&
        !s_wifi_stopping &&
        !app_power_save_is_active()) {

        s_retry_num = 0;

        esp_err_t conn_ret = esp_wifi_connect();
        if (conn_ret != ESP_OK &&
            conn_ret != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG,
                     "esp_wifi_connect after scan failed: %s",
                     esp_err_to_name(conn_ret));
        }
    }

    return ESP_OK;
}

