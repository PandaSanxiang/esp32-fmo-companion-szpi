/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_settings.c
 * @brief 应用配置管理实现。
 *
 * 本模块负责：
 * - 默认配置生成；
 * - NVS 配置读取与保存；
 * - 配置版本校验；
 * - 字段合法性检查；
 * - WiFi、FMO、音量、背光、电池校准、本机呼号等设置保存；
 * - WebSocket URL 自动生成。
 */

#include "app_settings.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Project headers ---------------------------------------------------------- */
#include "app_config.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "app_settings";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

#define NVS_NAMESPACE  "appcfg"
#define NVS_KEY_BLOB   "settings"
#define NVS_KEY_WIFI_PROFILES "wifi_profiles"

#define APP_WIFI_PROFILES_VERSION 1

#ifndef APP_DEFAULT_OWNER_CALLSIGN
#define APP_DEFAULT_OWNER_CALLSIGN "BI4TKL"
#endif

/*
 * 下面三个 key 当前未在本文件中单独使用。
 * 如果后续 QSO 状态改为单独 NVS key 保存，可以复用。
 */
#define KEY_QSO_COUNT       "qso_count"
#define KEY_QSO_LATEST      "qso_latest"
#define KEY_QSO_VALID       "qso_valid"

/*
 * WiFi SSID 在 802.11 中最大是 32 字节，不是 32 个字符。
 * 中文 UTF-8 一般一个汉字占 3 字节。
 *
 * 为了作为 C 字符串保存，app_settings_t.wifi_ssid 定义为 33 字节：
 * 32 字节 SSID + 1 字节 '\0'。
 */
#define WIFI_SSID_MAX_BYTES      32

/*
 * WPA/WPA2 密码最大 63 字节。
 */
#define WIFI_PASSWORD_MAX_BYTES  63

/*
 * WireGuard 默认值。
 * app_config.h 中可覆盖这些默认值；这里保留兜底，避免旧配置文件未定义时报错。
 */
#ifndef DEFAULT_WG_ENABLED
#define DEFAULT_WG_ENABLED              0
#endif

#ifndef DEFAULT_WG_PRIVATE_KEY
#define DEFAULT_WG_PRIVATE_KEY          ""
#endif

#ifndef DEFAULT_WG_LOCAL_IP
#define DEFAULT_WG_LOCAL_IP             "10.0.0.4"
#endif

#ifndef DEFAULT_WG_LOCAL_NETMASK
#define DEFAULT_WG_LOCAL_NETMASK        "255.255.255.0"
#endif

#ifndef DEFAULT_WG_PEER_PUBLIC_KEY
#define DEFAULT_WG_PEER_PUBLIC_KEY      ""
#endif

#ifndef DEFAULT_WG_PEER_ENDPOINT
#define DEFAULT_WG_PEER_ENDPOINT        ""
#endif

#ifndef DEFAULT_WG_PEER_PORT
#define DEFAULT_WG_PEER_PORT            51820
#endif

#ifndef DEFAULT_WG_LISTEN_PORT
#define DEFAULT_WG_LISTEN_PORT          51820
#endif

#ifndef DEFAULT_WG_PERSISTENT_KEEPALIVE
#define DEFAULT_WG_PERSISTENT_KEEPALIVE 25
#endif

/*
 * 本机呼号最大长度。
 * app_settings_t.owner_callsign 定义为 16 字节：
 * 15 字节内容 + 1 字节 '\0'。
 */
#define OWNER_CALLSIGN_MAX_LEN   15

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

static app_settings_t s_settings;

typedef struct {
    uint32_t version;
    uint8_t active_index;
    uint8_t reserved[3];
    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX];
} app_wifi_profiles_blob_t;

static app_wifi_profiles_blob_t s_wifi_profiles;

/*
 * v16 配置布局。
 * 新增 WireGuard 字段后用于从旧 NVS blob 迁移，避免升级后丢失 WiFi/FMO/UI 设置。
 */
typedef struct {
    uint32_t version;
    uint8_t backlight_percent;
    uint8_t audio_volume;
    uint8_t idle_image_enabled;
    char wifi_ssid[33];
    char wifi_password[64];
    char fmo_host[64];
    char ws_audio_url[128];
    char ws_event_url[128];
    char ws_station_url[128];
    uint32_t ws_station_current_refresh_ms;
    uint32_t ws_station_list_refresh_ms;
    uint32_t idle_image_timeout_ms;
    uint16_t battery_empty_mv;
    uint16_t battery_full_mv;
    int16_t battery_offset_mv;
    char owner_callsign[16];
    uint32_t qso_count;
    int32_t qso_latest_log_id;
    bool qso_count_valid;
    bool screen_rotate_180;
    bool qth_lookup_enabled;
    app_buzzer_mode_t buzzer_mode;
    app_sound_effect_t startup_sound_effect;
    app_sound_effect_t power_save_sound_effect;
    app_battery_display_mode_t battery_display_mode;
} app_settings_v16_t;


/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void safe_strcpy(char *dst, size_t dst_size, const char *src);
static bool is_non_empty_string(const char *s);
static bool is_c_string_terminated(const char *s, size_t max_size);
static bool cstr_len_range(const char *s,
                           size_t min_len,
                           size_t max_len,
                           size_t *out_len);

static bool owner_callsign_is_valid(const char *callsign);
static esp_err_t copy_owner_callsign(char *dst,
                                     size_t dst_size,
                                     const char *callsign);

static bool wifi_ssid_is_valid(const char *ssid);
static bool wifi_password_is_valid(const char *password);

static esp_err_t copy_wifi_ssid(char *dst,
                                size_t dst_size,
                                const char *ssid);
static esp_err_t copy_wifi_password(char *dst,
                                    size_t dst_size,
                                    const char *password);

static bool app_settings_is_valid(const app_settings_t *cfg);
static void app_settings_sanitize(app_settings_t *cfg);

static void app_settings_wifi_profiles_load_defaults(void);
static bool app_settings_wifi_profiles_is_valid(const app_wifi_profiles_blob_t *blob);
static void app_settings_wifi_profiles_sanitize(app_wifi_profiles_blob_t *blob);
static esp_err_t app_settings_load_wifi_profiles(void);
static esp_err_t app_settings_save_wifi_profiles(void);
static int wifi_profiles_find_by_ssid(const char *ssid);
static int wifi_profiles_find_empty(void);

static bool ipv4_addr_is_valid(const char *s);
static bool wireguard_key_is_valid(const char *s, size_t max_size);
static bool wireguard_endpoint_is_valid(const char *s);
static void app_settings_fill_default_wireguard(app_settings_t *cfg);
static void app_settings_migrate_from_v16(app_settings_t *dst,
                                          const app_settings_v16_t *old_cfg);

/* -------------------------------------------------------------------------- */
/* String helpers                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief 安全复制字符串。
 *
 * 与 strncpy 不同，该函数始终保证目标缓冲区以 '\0' 结尾。
 *
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param src 源字符串，可为 NULL。
 */
static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* -------------------------------------------------------------------------- */
/* WireGuard validation / defaults / migration                                */
/* -------------------------------------------------------------------------- */

static bool ipv4_addr_is_valid(const char *s)
{
    if (!s || s[0] == '\0') {
        return false;
    }

    int a = -1;
    int b = -1;
    int c = -1;
    int d = -1;
    char tail = '\0';

    if (sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }

    return a >= 0 && a <= 255 &&
           b >= 0 && b <= 255 &&
           c >= 0 && c <= 255 &&
           d >= 0 && d <= 255;
}

static bool wireguard_key_is_valid(const char *s, size_t max_size)
{
    if (!s || s[0] == '\0' || max_size == 0) {
        return false;
    }

    if (!is_c_string_terminated(s, max_size)) {
        return false;
    }

    size_t len = strlen(s);

    /*
     * WireGuard base64 key 通常为 44 字符。
     * 这里允许 32~63，避免过度限制；真正的长度错误仍会在 WireGuard 初始化时失败。
     */
    if (len < 32 || len >= max_size) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = s[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '+' ||
            c == '/' ||
            c == '=') {
            continue;
        }

        return false;
    }

    return true;
}

static bool wireguard_endpoint_is_valid(const char *s)
{
    if (!s || s[0] == '\0') {
        return false;
    }

    size_t len = strlen(s);
    if (len == 0 || len >= 64) {
        return false;
    }

    /*
     * endpoint 只保存主机名或 IP，不保存 scheme/path。
     * 端口由 wg_peer_port 单独保存。
     */
    for (size_t i = 0; i < len; i++) {
        char c = s[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '/' || c == '\\') {
            return false;
        }
    }

    return true;
}

static void app_settings_fill_default_wireguard(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->wg_enabled = DEFAULT_WG_ENABLED ? true : false;

    safe_strcpy(cfg->wg_private_key,
                sizeof(cfg->wg_private_key),
                DEFAULT_WG_PRIVATE_KEY);

    safe_strcpy(cfg->wg_local_ip,
                sizeof(cfg->wg_local_ip),
                DEFAULT_WG_LOCAL_IP);

    safe_strcpy(cfg->wg_local_netmask,
                sizeof(cfg->wg_local_netmask),
                DEFAULT_WG_LOCAL_NETMASK);

    safe_strcpy(cfg->wg_peer_public_key,
                sizeof(cfg->wg_peer_public_key),
                DEFAULT_WG_PEER_PUBLIC_KEY);

    safe_strcpy(cfg->wg_peer_endpoint,
                sizeof(cfg->wg_peer_endpoint),
                DEFAULT_WG_PEER_ENDPOINT);

    cfg->wg_peer_port = DEFAULT_WG_PEER_PORT;
    cfg->wg_listen_port = DEFAULT_WG_LISTEN_PORT;
    cfg->wg_persistent_keepalive = DEFAULT_WG_PERSISTENT_KEEPALIVE;
}

static void app_settings_migrate_from_v16(app_settings_t *dst,
                                          const app_settings_v16_t *old_cfg)
{
    if (!dst || !old_cfg) {
        return;
    }

    app_settings_load_defaults(dst);

    dst->backlight_percent = old_cfg->backlight_percent;
    dst->audio_volume = old_cfg->audio_volume;
    dst->idle_image_enabled = old_cfg->idle_image_enabled;

    safe_strcpy(dst->wifi_ssid, sizeof(dst->wifi_ssid), old_cfg->wifi_ssid);
    safe_strcpy(dst->wifi_password, sizeof(dst->wifi_password), old_cfg->wifi_password);
    safe_strcpy(dst->fmo_host, sizeof(dst->fmo_host), old_cfg->fmo_host);

    dst->ws_station_current_refresh_ms = old_cfg->ws_station_current_refresh_ms;
    dst->ws_station_list_refresh_ms = old_cfg->ws_station_list_refresh_ms;
    dst->idle_image_timeout_ms = old_cfg->idle_image_timeout_ms;

    dst->battery_empty_mv = old_cfg->battery_empty_mv;
    dst->battery_full_mv = old_cfg->battery_full_mv;
    dst->battery_offset_mv = old_cfg->battery_offset_mv;

    safe_strcpy(dst->owner_callsign,
                sizeof(dst->owner_callsign),
                old_cfg->owner_callsign);

    dst->qso_count = old_cfg->qso_count;
    dst->qso_latest_log_id = old_cfg->qso_latest_log_id;
    dst->qso_count_valid = old_cfg->qso_count_valid;

    dst->screen_rotate_180 = old_cfg->screen_rotate_180;
    dst->qth_lookup_enabled = old_cfg->qth_lookup_enabled;
    dst->buzzer_mode = old_cfg->buzzer_mode;
    dst->startup_sound_effect = old_cfg->startup_sound_effect;
    dst->power_save_sound_effect = old_cfg->power_save_sound_effect;
    dst->battery_display_mode = old_cfg->battery_display_mode;

    /*
     * v16 没有 WireGuard 字段，升级时补默认值。
     */
    app_settings_fill_default_wireguard(dst);

    dst->version = APP_SETTINGS_VERSION;
    app_settings_build_ws_urls(dst);
}

/**
 * @brief 判断字符串是否非空。
 */
static bool is_non_empty_string(const char *s)
{
    return s && s[0] != '\0';
}

/**
 * @brief 检查字符串在限定范围内是否存在 '\0'。
 *
 * 用于避免从 NVS blob 读取出的字符串没有结束符，
 * 从而导致 strlen 等函数越界读取。
 */
static bool is_c_string_terminated(const char *s, size_t max_size)
{
    if (!s || max_size == 0) {
        return false;
    }

    return memchr(s, '\0', max_size) != NULL;
}

/**
 * @brief 检查 C 字符串长度是否在范围内。
 *
 * 与 strlen 不同，该函数最多只检查 max_len + 1 字节。
 * 如果在该范围内找不到 '\0'，会直接判定为非法，避免无界越界读取。
 */
static bool cstr_len_range(const char *s,
                           size_t min_len,
                           size_t max_len,
                           size_t *out_len)
{
    if (!s) {
        return false;
    }

    const char *end = (const char *)memchr(s, '\0', max_len + 1);
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

/* -------------------------------------------------------------------------- */
/* Owner callsign validation and copy                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief 本机呼号合法性检查。
 *
 * 允许字符：
 * - A-Z
 * - a-z
 * - 0-9
 * - '-'
 * - '_'
 * - '/'
 * - '.'
 *
 * 保存时会自动转成大写。
 */
static bool owner_callsign_is_valid(const char *callsign)
{
    if (!callsign || callsign[0] == '\0') {
        return false;
    }

    size_t len = strlen(callsign);

    if (len == 0 || len > OWNER_CALLSIGN_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)callsign[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' ||
            c == '_' ||
            c == '/' ||
            c == '.') {
            continue;
        }

        return false;
    }

    return true;
}

/**
 * @brief 复制并规范化本机呼号。
 *
 * 小写字母会自动转为大写。
 */
static esp_err_t copy_owner_callsign(char *dst,
                                     size_t dst_size,
                                     const char *callsign)
{
    if (!dst || dst_size == 0 || !callsign) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!owner_callsign_is_valid(callsign)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(callsign);

    if (len >= dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(dst, 0, dst_size);

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)callsign[i];

        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)toupper(c);
        }

        dst[i] = (char)c;
    }

    dst[len] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WiFi validation and copy                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief WiFi SSID 合法性检查。
 *
 * 注意：
 * 这里检查的是字节长度，不检查 UTF-8 字符完整性。
 * 对 WiFi 驱动来说，SSID 本质是字节序列。
 *
 * 当前规则：
 * - 不能为空；
 * - 长度必须为 1~32 字节。
 */
static bool wifi_ssid_is_valid(const char *ssid)
{
    return cstr_len_range(ssid, 1, WIFI_SSID_MAX_BYTES, NULL);
}

/**
 * @brief WiFi 密码合法性检查。
 *
 * 这里不强制 8~63 字节，是为了兼容开放网络空密码。
 */
static bool wifi_password_is_valid(const char *password)
{
    if (!password) {
        return true;
    }

    return cstr_len_range(password, 0, WIFI_PASSWORD_MAX_BYTES, NULL);
}

/**
 * @brief 复制 WiFi SSID。
 *
 * 不允许截断。
 *
 * 原因：
 * - 中文 SSID 如果被截断，可能破坏 UTF-8 字节序列；
 * - SSID 字节序列被截断后也无法匹配路由器广播的真实 SSID。
 */
static esp_err_t copy_wifi_ssid(char *dst,
                                size_t dst_size,
                                const char *ssid)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = 0;
    if (!cstr_len_range(ssid, 1, WIFI_SSID_MAX_BYTES, &len)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * dst 至少需要容纳 32 字节 SSID + '\0'。
     */
    if (dst_size < WIFI_SSID_MAX_BYTES + 1 || len >= dst_size) {
        ESP_LOGE(TAG,
                 "wifi_ssid buffer too small: %u, need at least 33",
                 (unsigned)dst_size);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(dst, 0, dst_size);
    memcpy(dst, ssid, len);
    dst[len] = '\0';

    return ESP_OK;
}

/**
 * @brief 复制 WiFi 密码。
 *
 * 密码数组通常是 64 字节，最大密码 63 字节 + '\0'。
 */
static esp_err_t copy_wifi_password(char *dst,
                                    size_t dst_size,
                                    const char *password)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!password) {
        memset(dst, 0, dst_size);
        return ESP_OK;
    }

    size_t len = 0;
    if (!cstr_len_range(password, 0, WIFI_PASSWORD_MAX_BYTES, &len)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(dst, 0, dst_size);
    memcpy(dst, password, len);
    dst[len] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* WiFi profiles                                                              */
/* -------------------------------------------------------------------------- */

static void app_settings_wifi_profiles_load_defaults(void)
{
    memset(&s_wifi_profiles, 0, sizeof(s_wifi_profiles));

    s_wifi_profiles.version = APP_WIFI_PROFILES_VERSION;
    s_wifi_profiles.active_index = 0;

    if (s_settings.wifi_ssid[0] != '\0' &&
        wifi_ssid_is_valid(s_settings.wifi_ssid) &&
        wifi_password_is_valid(s_settings.wifi_password)) {
        app_wifi_profile_t *profile = &s_wifi_profiles.profiles[0];

        profile->used = 1;
        copy_wifi_ssid(profile->ssid,
                       sizeof(profile->ssid),
                       s_settings.wifi_ssid);
        copy_wifi_password(profile->password,
                           sizeof(profile->password),
                           s_settings.wifi_password);
    }
}

static bool app_settings_wifi_profiles_is_valid(const app_wifi_profiles_blob_t *blob)
{
    if (!blob) {
        return false;
    }

    if (blob->version != APP_WIFI_PROFILES_VERSION) {
        return false;
    }

    if (blob->active_index >= APP_WIFI_PROFILE_MAX) {
        return false;
    }

    for (int i = 0; i < APP_WIFI_PROFILE_MAX; i++) {
        const app_wifi_profile_t *profile = &blob->profiles[i];

        if (profile->used > 1) {
            return false;
        }

        if (!is_c_string_terminated(profile->ssid,
                                    sizeof(profile->ssid)) ||
            !is_c_string_terminated(profile->password,
                                    sizeof(profile->password))) {
            return false;
        }

        if (!profile->used) {
            continue;
        }

        if (!wifi_ssid_is_valid(profile->ssid) ||
            !wifi_password_is_valid(profile->password)) {
            return false;
        }
    }

    if (blob->profiles[blob->active_index].used == 0) {
        for (int i = 0; i < APP_WIFI_PROFILE_MAX; i++) {
            if (blob->profiles[i].used) {
                return true;
            }
        }

        return s_settings.wifi_ssid[0] == '\0';
    }

    return true;
}

static void app_settings_wifi_profiles_sanitize(app_wifi_profiles_blob_t *blob)
{
    if (!blob) {
        return;
    }

    blob->version = APP_WIFI_PROFILES_VERSION;

    bool has_used = false;

    for (int i = 0; i < APP_WIFI_PROFILE_MAX; i++) {
        app_wifi_profile_t *profile = &blob->profiles[i];

        profile->used = profile->used ? 1 : 0;
        profile->ssid[sizeof(profile->ssid) - 1] = '\0';
        profile->password[sizeof(profile->password) - 1] = '\0';

        if (!profile->used) {
            memset(profile, 0, sizeof(*profile));
            continue;
        }

        if (!wifi_ssid_is_valid(profile->ssid) ||
            !wifi_password_is_valid(profile->password)) {
            ESP_LOGW(TAG, "wifi profile %d invalid, clear it", i);
            memset(profile, 0, sizeof(*profile));
            continue;
        }

        has_used = true;
    }

    if (blob->active_index >= APP_WIFI_PROFILE_MAX ||
        !blob->profiles[blob->active_index].used) {
        blob->active_index = 0;

        for (int i = 0; i < APP_WIFI_PROFILE_MAX; i++) {
            if (blob->profiles[i].used) {
                blob->active_index = (uint8_t)i;
                break;
            }
        }
    }

    if (!has_used) {
        blob->active_index = 0;
    }
}

static esp_err_t app_settings_save_wifi_profiles(void)
{
    app_settings_wifi_profiles_sanitize(&s_wifi_profiles);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "nvs_open for wifi profiles failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle,
                       NVS_KEY_WIFI_PROFILES,
                       &s_wifi_profiles,
                       sizeof(s_wifi_profiles));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "wifi profiles save failed: %s",
                 esp_err_to_name(err));
    }

    return err;
}

static esp_err_t app_settings_load_wifi_profiles(void)
{
    app_settings_wifi_profiles_load_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "nvs_open for wifi profiles failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_blob(handle, NVS_KEY_WIFI_PROFILES, NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(handle);

        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "wifi profiles not found, create defaults");
            return app_settings_save_wifi_profiles();
        }

        ESP_LOGW(TAG,
                 "wifi profiles size query failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (required_size != sizeof(app_wifi_profiles_blob_t)) {
        nvs_close(handle);
        ESP_LOGW(TAG,
                 "wifi profiles size mismatch, stored=%u expected=%u",
                 (unsigned)required_size,
                 (unsigned)sizeof(app_wifi_profiles_blob_t));
        return app_settings_save_wifi_profiles();
    }

    app_wifi_profiles_blob_t loaded;
    size_t read_size = sizeof(loaded);
    memset(&loaded, 0, sizeof(loaded));

    err = nvs_get_blob(handle, NVS_KEY_WIFI_PROFILES, &loaded, &read_size);
    nvs_close(handle);

    if (err == ESP_OK &&
        read_size == sizeof(loaded) &&
        app_settings_wifi_profiles_is_valid(&loaded)) {
        app_settings_wifi_profiles_sanitize(&loaded);
        s_wifi_profiles = loaded;

        ESP_LOGI(TAG,
                 "wifi profiles loaded, active=%u",
                 (unsigned)s_wifi_profiles.active_index);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "wifi profiles invalid or load failed, recreate defaults: %s",
             esp_err_to_name(err));
    app_settings_wifi_profiles_load_defaults();
    return app_settings_save_wifi_profiles();
}

static int wifi_profiles_find_by_ssid(const char *ssid)
{
    if (!ssid) {
        return -1;
    }

    for (int i = 0; i < APP_WIFI_PROFILE_MAX; i++) {
        const app_wifi_profile_t *profile = &s_wifi_profiles.profiles[i];

        if (profile->used && strcmp(profile->ssid, ssid) == 0) {
            return i;
        }
    }

    return -1;
}

static int wifi_profiles_find_empty(void)
{
    for (int i = 0; i < APP_WIFI_PROFILE_MAX; i++) {
        if (!s_wifi_profiles.profiles[i].used) {
            return i;
        }
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* WebSocket URL builder                                                      */
/* -------------------------------------------------------------------------- */

void app_settings_build_ws_urls(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    /*
     * 如果 fmo_host 为空，回退到默认 host。
     */
    if (!is_non_empty_string(cfg->fmo_host)) {
        safe_strcpy(cfg->fmo_host, sizeof(cfg->fmo_host), DEFAULT_FMO_HOST);
    }

    snprintf(cfg->ws_audio_url,
             sizeof(cfg->ws_audio_url),
             "ws://%s/audio",
             cfg->fmo_host);

    snprintf(cfg->ws_event_url,
             sizeof(cfg->ws_event_url),
             "ws://%s/events",
             cfg->fmo_host);

    snprintf(cfg->ws_station_url,
             sizeof(cfg->ws_station_url),
             "ws://%s/ws",
             cfg->fmo_host);

    cfg->ws_audio_url[sizeof(cfg->ws_audio_url) - 1] = '\0';
    cfg->ws_event_url[sizeof(cfg->ws_event_url) - 1] = '\0';
    cfg->ws_station_url[sizeof(cfg->ws_station_url) - 1] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Defaults                                                                   */
/* -------------------------------------------------------------------------- */

void app_settings_load_defaults(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(app_settings_t));

    cfg->version = APP_SETTINGS_VERSION;

    cfg->backlight_percent = DEFAULT_BACKLIGHT_PERCENT;
    cfg->audio_volume = DEFAULT_AUDIO_VOLUME;
    cfg->idle_image_enabled = DEFAULT_IDLE_IMAGE_ENABLED;

    /*
     * 默认本机呼号。
     */
    if (copy_owner_callsign(cfg->owner_callsign,
                            sizeof(cfg->owner_callsign),
                            APP_DEFAULT_OWNER_CALLSIGN) != ESP_OK) {
        ESP_LOGW(TAG, "APP_DEFAULT_OWNER_CALLSIGN invalid, use BI4TKL");
        safe_strcpy(cfg->owner_callsign,
                    sizeof(cfg->owner_callsign),
                    "BI4TKL");
    }

    /*
     * 默认 WiFi SSID 不允许截断。
     * 如果 DEFAULT_WIFI_SSID 超过 32 字节，会保存失败并置空。
     */
    if (copy_wifi_ssid(cfg->wifi_ssid,
                       sizeof(cfg->wifi_ssid),
                       DEFAULT_WIFI_SSID) != ESP_OK) {
        ESP_LOGW(TAG, "DEFAULT_WIFI_SSID invalid or too long, clear wifi_ssid");
        cfg->wifi_ssid[0] = '\0';
    }

    if (copy_wifi_password(cfg->wifi_password,
                           sizeof(cfg->wifi_password),
                           DEFAULT_WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGW(TAG,
                 "DEFAULT_WIFI_PASSWORD invalid or too long, clear wifi_password");
        cfg->wifi_password[0] = '\0';
    }

    /*
     * 用户只需要配置 FMO host。
     * 三个 WebSocket URL 由 fmo_host 自动生成。
     */
    safe_strcpy(cfg->fmo_host, sizeof(cfg->fmo_host), DEFAULT_FMO_HOST);
    app_settings_build_ws_urls(cfg);

    cfg->ws_station_current_refresh_ms =
        DEFAULT_WS_STATION_CURRENT_REFRESH_MS;
    cfg->ws_station_list_refresh_ms =
        DEFAULT_WS_STATION_LIST_REFRESH_MS;
    cfg->idle_image_timeout_ms =
        DEFAULT_IDLE_IMAGE_TIMEOUT_MS;

    cfg->battery_empty_mv = BATTERY_PERCENT_EMPTY_MV;
    cfg->battery_full_mv = BATTERY_PERCENT_FULL_MV;
    cfg->battery_offset_mv = BATTERY_VOLTAGE_OFFSET_MV;

    cfg->qso_count = 0;
    cfg->qso_latest_log_id = -1;
    cfg->qso_count_valid = false;

    cfg->screen_rotate_180 = false;

    cfg->qso_count_valid = false;

    cfg->buzzer_mode = APP_BUZZER_MODE_OFF;

    cfg->startup_sound_effect = APP_SOUND_EFFECT_POWER_ON;

    cfg->power_save_sound_effect = APP_SOUND_EFFECT_C4_COUNTDOWN;

    cfg->battery_display_mode = APP_BATTERY_DISPLAY_ICON;

    cfg->color_theme = APP_COLOR_THEME_DARK_ORANGE;

    app_settings_fill_default_wireguard(cfg);
}

/* -------------------------------------------------------------------------- */
/* Validation and sanitize                                                    */
/* -------------------------------------------------------------------------- */

static bool app_settings_is_valid(const app_settings_t *cfg)
{
    if (!cfg) {
        return false;
    }

    if (cfg->version != APP_SETTINGS_VERSION) {
        return false;
    }

    if (cfg->backlight_percent > 100) {
        return false;
    }

    if (cfg->audio_volume > 100) {
        return false;
    }

    if (cfg->idle_image_enabled > 1) {
        return false;
    }

    /*
     * 先确保 NVS 中读出的字符串都有 '\0' 结尾。
     */
    if (!is_c_string_terminated(cfg->wifi_ssid,
                                sizeof(cfg->wifi_ssid))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->wifi_password,
                                sizeof(cfg->wifi_password))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->fmo_host,
                                sizeof(cfg->fmo_host))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->ws_audio_url,
                                sizeof(cfg->ws_audio_url))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->ws_event_url,
                                sizeof(cfg->ws_event_url))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->ws_station_url,
                                sizeof(cfg->ws_station_url))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->owner_callsign,
                                sizeof(cfg->owner_callsign))) {
        return false;
    }

    /*
     * WiFi 允许处于“未配置”状态。
     * 只有 SSID 非空时才要求 1~32 字节；SSID 为空时密码也必须为空。
     */
    if (cfg->wifi_ssid[0] != '\0' &&
        !wifi_ssid_is_valid(cfg->wifi_ssid)) {
        return false;
    }

    if (!wifi_password_is_valid(cfg->wifi_password)) {
        return false;
    }

    if (cfg->wifi_ssid[0] == '\0' && cfg->wifi_password[0] != '\0') {
        return false;
    }

    if (!owner_callsign_is_valid(cfg->owner_callsign)) {
        return false;
    }

    if (!is_non_empty_string(cfg->fmo_host)) {
        return false;
    }

    if (!is_non_empty_string(cfg->ws_audio_url)) {
        return false;
    }

    if (!is_non_empty_string(cfg->ws_event_url)) {
        return false;
    }

    if (!is_non_empty_string(cfg->ws_station_url)) {
        return false;
    }

    /*
     * 电池校准基本合法性。
     */
    if (cfg->battery_empty_mv < 2500 || cfg->battery_empty_mv > 4200) {
        return false;
    }

    if (cfg->battery_full_mv < 3500 || cfg->battery_full_mv > 4500) {
        return false;
    }

    if (cfg->battery_full_mv <= cfg->battery_empty_mv) {
        return false;
    }


    if (!is_c_string_terminated(cfg->wg_private_key,
                                sizeof(cfg->wg_private_key))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->wg_local_ip,
                                sizeof(cfg->wg_local_ip))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->wg_local_netmask,
                                sizeof(cfg->wg_local_netmask))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->wg_peer_public_key,
                                sizeof(cfg->wg_peer_public_key))) {
        return false;
    }

    if (!is_c_string_terminated(cfg->wg_peer_endpoint,
                                sizeof(cfg->wg_peer_endpoint))) {
        return false;
    }

    if (cfg->wg_peer_port == 0 || cfg->wg_listen_port == 0) {
        return false;
    }

    if (cfg->wg_enabled) {
        if (!wireguard_key_is_valid(cfg->wg_private_key,
                                    sizeof(cfg->wg_private_key))) {
            return false;
        }

        if (!wireguard_key_is_valid(cfg->wg_peer_public_key,
                                    sizeof(cfg->wg_peer_public_key))) {
            return false;
        }

        if (!ipv4_addr_is_valid(cfg->wg_local_ip) ||
            !ipv4_addr_is_valid(cfg->wg_local_netmask)) {
            return false;
        }

        if (!wireguard_endpoint_is_valid(cfg->wg_peer_endpoint)) {
            return false;
        }
    }

    return true;
}

static void app_settings_sanitize(app_settings_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->version = APP_SETTINGS_VERSION;

    if (cfg->backlight_percent > 100) {
        cfg->backlight_percent = 100;
    }

    if (cfg->audio_volume > 100) {
        cfg->audio_volume = 100;
    }

    cfg->idle_image_enabled = cfg->idle_image_enabled ? 1 : 0;

    /*
     * 先保证字符串有结尾，避免 strlen 越界。
     */
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->fmo_host[sizeof(cfg->fmo_host) - 1] = '\0';
    cfg->owner_callsign[sizeof(cfg->owner_callsign) - 1] = '\0';

    /*
     * 本机呼号非法则恢复默认。
     */
    if (!owner_callsign_is_valid(cfg->owner_callsign)) {
        ESP_LOGW(TAG, "owner_callsign invalid, restore default");

        copy_owner_callsign(cfg->owner_callsign,
                            sizeof(cfg->owner_callsign),
                            APP_DEFAULT_OWNER_CALLSIGN);
    } else {
        /*
         * 规范化成大写。
         */
        char tmp_owner[sizeof(cfg->owner_callsign)];

        safe_strcpy(tmp_owner, sizeof(tmp_owner), cfg->owner_callsign);

        copy_owner_callsign(cfg->owner_callsign,
                            sizeof(cfg->owner_callsign),
                            tmp_owner);
    }

    /*
     * WiFi SSID 超过 32 字节时不能简单截断。
     */
    if (cfg->wifi_ssid[0] != '\0') {
        size_t ssid_len = strlen(cfg->wifi_ssid);

        if (ssid_len > WIFI_SSID_MAX_BYTES) {
            ESP_LOGW(TAG,
                     "wifi_ssid too long in sanitize: %u bytes, clear it",
                     (unsigned)ssid_len);
            cfg->wifi_ssid[0] = '\0';
        }
    }

    if (cfg->wifi_ssid[0] == '\0') {
        cfg->wifi_password[0] = '\0';
    }

    /*
     * 密码超过 63 字节时清空。
     */
    if (cfg->wifi_password[0] != '\0') {
        size_t pwd_len = strlen(cfg->wifi_password);

        if (pwd_len > WIFI_PASSWORD_MAX_BYTES) {
            ESP_LOGW(TAG,
                     "wifi_password too long in sanitize: %u bytes, clear it",
                     (unsigned)pwd_len);
            cfg->wifi_password[0] = '\0';
        }
    }

    if (!is_non_empty_string(cfg->fmo_host)) {
        safe_strcpy(cfg->fmo_host, sizeof(cfg->fmo_host), DEFAULT_FMO_HOST);
    }

    /*
     * 保存前统一重新生成 WebSocket URL。
     */
    app_settings_build_ws_urls(cfg);

    cfg->ws_audio_url[sizeof(cfg->ws_audio_url) - 1] = '\0';
    cfg->ws_event_url[sizeof(cfg->ws_event_url) - 1] = '\0';
    cfg->ws_station_url[sizeof(cfg->ws_station_url) - 1] = '\0';

    if (cfg->ws_station_current_refresh_ms < 5000) {
        cfg->ws_station_current_refresh_ms =
            DEFAULT_WS_STATION_CURRENT_REFRESH_MS;
    }

    if (cfg->ws_station_list_refresh_ms < 30000) {
        cfg->ws_station_list_refresh_ms =
            DEFAULT_WS_STATION_LIST_REFRESH_MS;
    }

    if (cfg->idle_image_timeout_ms < 5000) {
        cfg->idle_image_timeout_ms = DEFAULT_IDLE_IMAGE_TIMEOUT_MS;
    }

    if (cfg->battery_empty_mv < 2500 || cfg->battery_empty_mv > 4200) {
        cfg->battery_empty_mv = BATTERY_PERCENT_EMPTY_MV;
    }

    if (cfg->battery_full_mv < 3500 || cfg->battery_full_mv > 4500) {
        cfg->battery_full_mv = BATTERY_PERCENT_FULL_MV;
    }

    if (cfg->battery_full_mv <= cfg->battery_empty_mv) {
        cfg->battery_empty_mv = BATTERY_PERCENT_EMPTY_MV;
        cfg->battery_full_mv = BATTERY_PERCENT_FULL_MV;
    }

        if (cfg->startup_sound_effect < APP_SOUND_EFFECT_OFF ||
        cfg->startup_sound_effect >= APP_SOUND_EFFECT_MAX) {
        cfg->startup_sound_effect = APP_SOUND_EFFECT_POWER_ON;
    }

    if (cfg->power_save_sound_effect < APP_SOUND_EFFECT_OFF ||
        cfg->power_save_sound_effect >= APP_SOUND_EFFECT_MAX) {
        cfg->power_save_sound_effect = APP_SOUND_EFFECT_C4_COUNTDOWN;
    }

    if (cfg->battery_display_mode < APP_BATTERY_DISPLAY_ICON ||
        cfg->battery_display_mode >= APP_BATTERY_DISPLAY_MAX) {
        cfg->battery_display_mode = APP_BATTERY_DISPLAY_ICON;
    }


    /* sanitize WireGuard */
    cfg->wg_enabled = cfg->wg_enabled ? true : false;

    cfg->wg_private_key[sizeof(cfg->wg_private_key) - 1] = '\0';
    cfg->wg_local_ip[sizeof(cfg->wg_local_ip) - 1] = '\0';
    cfg->wg_local_netmask[sizeof(cfg->wg_local_netmask) - 1] = '\0';
    cfg->wg_peer_public_key[sizeof(cfg->wg_peer_public_key) - 1] = '\0';
    cfg->wg_peer_endpoint[sizeof(cfg->wg_peer_endpoint) - 1] = '\0';

    if (cfg->wg_enabled) {
        if (!wireguard_key_is_valid(cfg->wg_private_key,
                                    sizeof(cfg->wg_private_key))) {
            safe_strcpy(cfg->wg_private_key,
                        sizeof(cfg->wg_private_key),
                        DEFAULT_WG_PRIVATE_KEY);
        }

        if (!wireguard_key_is_valid(cfg->wg_peer_public_key,
                                    sizeof(cfg->wg_peer_public_key))) {
            safe_strcpy(cfg->wg_peer_public_key,
                        sizeof(cfg->wg_peer_public_key),
                        DEFAULT_WG_PEER_PUBLIC_KEY);
        }

        if (!ipv4_addr_is_valid(cfg->wg_local_ip)) {
            safe_strcpy(cfg->wg_local_ip,
                        sizeof(cfg->wg_local_ip),
                        DEFAULT_WG_LOCAL_IP);
        }

        if (!ipv4_addr_is_valid(cfg->wg_local_netmask)) {
            safe_strcpy(cfg->wg_local_netmask,
                        sizeof(cfg->wg_local_netmask),
                        DEFAULT_WG_LOCAL_NETMASK);
        }

        if (!wireguard_endpoint_is_valid(cfg->wg_peer_endpoint)) {
            safe_strcpy(cfg->wg_peer_endpoint,
                        sizeof(cfg->wg_peer_endpoint),
                        DEFAULT_WG_PEER_ENDPOINT);
        }
    }

    if (cfg->wg_peer_port == 0) {
        cfg->wg_peer_port = DEFAULT_WG_PEER_PORT;
    }

    if (cfg->wg_listen_port == 0) {
        cfg->wg_listen_port = DEFAULT_WG_LISTEN_PORT;
    }

    if (cfg->color_theme < APP_COLOR_THEME_DARK_ORANGE ||
        cfg->color_theme >= APP_COLOR_THEME_MAX) {
        cfg->color_theme = APP_COLOR_THEME_DARK_ORANGE;
    }

}


/* -------------------------------------------------------------------------- */
/* Save / load / init                                                         */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_screen_rotate_180(bool enabled)
{
    app_settings_t tmp = s_settings;

    tmp.screen_rotate_180 = enabled;

    return app_settings_save(&tmp);
}

esp_err_t app_settings_save(const app_settings_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = *cfg;

    app_settings_sanitize(&tmp);

    /*
     * WiFi 允许未配置；只有 SSID 非空时才校验 SSID 长度。
     */
    if (tmp.wifi_ssid[0] != '\0' &&
        !wifi_ssid_is_valid(tmp.wifi_ssid)) {
        ESP_LOGE(TAG,
                 "invalid wifi_ssid, len=%u",
                 (unsigned)strlen(tmp.wifi_ssid));
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_password_is_valid(tmp.wifi_password)) {
        ESP_LOGE(TAG,
                 "invalid wifi_password, len=%u",
                 (unsigned)strlen(tmp.wifi_password));
        return ESP_ERR_INVALID_ARG;
    }

    if (tmp.wifi_ssid[0] == '\0') {
        tmp.wifi_password[0] = '\0';
    }

    if (!owner_callsign_is_valid(tmp.owner_callsign)) {
        ESP_LOGE(TAG, "invalid owner_callsign=%s", tmp.owner_callsign);
        return ESP_ERR_INVALID_ARG;
    }

    if (tmp.wg_enabled) {
        if (!wireguard_key_is_valid(tmp.wg_private_key,
                                    sizeof(tmp.wg_private_key)) ||
            !wireguard_key_is_valid(tmp.wg_peer_public_key,
                                    sizeof(tmp.wg_peer_public_key)) ||
            !ipv4_addr_is_valid(tmp.wg_local_ip) ||
            !ipv4_addr_is_valid(tmp.wg_local_netmask) ||
            !wireguard_endpoint_is_valid(tmp.wg_peer_endpoint) ||
            tmp.wg_peer_port == 0 ||
            tmp.wg_listen_port == 0) {
            ESP_LOGE(TAG, "invalid WireGuard settings");
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t handle;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_BLOB, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        s_settings = tmp;

        ESP_LOGI(TAG, "settings saved");
        ESP_LOGI(TAG, "owner_callsign=%s", s_settings.owner_callsign);
        ESP_LOGI(TAG,
                 "wifi_ssid=%s, len=%u bytes",
                 s_settings.wifi_ssid,
                 (unsigned)strlen(s_settings.wifi_ssid));
        ESP_LOGI(TAG, "fmo_host=%s", s_settings.fmo_host);
        ESP_LOGI(TAG, "ws_audio=%s", s_settings.ws_audio_url);
        ESP_LOGI(TAG, "ws_event=%s", s_settings.ws_event_url);
        ESP_LOGI(TAG, "ws_station=%s", s_settings.ws_station_url);
        ESP_LOGI(TAG,
                 "backlight=%u, volume=%u",
                 s_settings.backlight_percent,
                 s_settings.audio_volume);
        ESP_LOGI(TAG,
                 "battery empty=%u full=%u offset=%d",
                 s_settings.battery_empty_mv,
                 s_settings.battery_full_mv,
                 s_settings.battery_offset_mv);
        ESP_LOGI(TAG,
                 "wireguard enabled=%d endpoint=%s:%u local=%s",
                 s_settings.wg_enabled ? 1 : 0,
                 s_settings.wg_peer_endpoint,
                 (unsigned)s_settings.wg_peer_port,
                 s_settings.wg_local_ip);
    } else {
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t app_settings_load(app_settings_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    *cfg = s_settings;

    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_settings;
}

esp_err_t app_settings_init(void)
{
    /*
     * 默认先加载默认配置，避免 NVS 读取失败时 s_settings 未初始化。
     */
    app_settings_load_defaults(&s_settings);
    app_settings_wifi_profiles_load_defaults();

    nvs_handle_t handle;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "nvs_open failed, use defaults: %s",
                 esp_err_to_name(err));
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_blob(handle, NVS_KEY_BLOB, NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(handle);

        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "settings not found, save defaults");
        } else {
            ESP_LOGW(TAG,
                     "settings size query failed, save defaults: %s",
                     esp_err_to_name(err));
        }

        err = app_settings_save(&s_settings);
        if (err == ESP_OK) {
            app_settings_load_wifi_profiles();
        }
        return err;
    }

    if (required_size == sizeof(app_settings_t)) {
        app_settings_t loaded;
        memset(&loaded, 0, sizeof(loaded));

        err = nvs_get_blob(handle, NVS_KEY_BLOB, &loaded, &required_size);
        nvs_close(handle);

        if (err == ESP_OK && app_settings_is_valid(&loaded)) {
            /*
             * 即使 NVS 中已有 URL，也重新按 fmo_host 生成一次，
             * 保证规则统一。
             */
            app_settings_sanitize(&loaded);

            s_settings = loaded;

            ESP_LOGI(TAG, "settings loaded from NVS");
            ESP_LOGI(TAG, "owner_callsign=%s", s_settings.owner_callsign);
            ESP_LOGI(TAG,
                     "wifi_ssid=%s, len=%u bytes",
                     s_settings.wifi_ssid,
                     (unsigned)strlen(s_settings.wifi_ssid));
            ESP_LOGI(TAG, "fmo_host=%s", s_settings.fmo_host);
            ESP_LOGI(TAG,
                     "backlight=%u, volume=%u, idle=%u",
                     s_settings.backlight_percent,
                     s_settings.audio_volume,
                     s_settings.idle_image_enabled);
            ESP_LOGI(TAG,
                     "wireguard enabled=%d endpoint=%s:%u local=%s",
                     s_settings.wg_enabled ? 1 : 0,
                     s_settings.wg_peer_endpoint,
                     (unsigned)s_settings.wg_peer_port,
                     s_settings.wg_local_ip);

            app_settings_load_wifi_profiles();
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "settings invalid or load failed, save defaults, err=%s",
                 esp_err_to_name(err));

        err = app_settings_save(&s_settings);
        if (err == ESP_OK) {
            app_settings_load_wifi_profiles();
        }
        return err;
    }

    if (required_size == sizeof(app_settings_v16_t)) {
        app_settings_v16_t old_cfg;
        size_t old_size = sizeof(old_cfg);
        memset(&old_cfg, 0, sizeof(old_cfg));

        err = nvs_get_blob(handle, NVS_KEY_BLOB, &old_cfg, &old_size);
        nvs_close(handle);

        if (err == ESP_OK && old_size == sizeof(old_cfg)) {
            ESP_LOGW(TAG,
                     "migrate settings from v16 to v%u",
                     APP_SETTINGS_VERSION);

            app_settings_migrate_from_v16(&s_settings, &old_cfg);
            app_settings_sanitize(&s_settings);

            err = app_settings_save(&s_settings);
            if (err == ESP_OK) {
                app_settings_load_wifi_profiles();
            }
            return err;
        }

        ESP_LOGW(TAG,
                 "read v16 settings failed, save defaults: %s",
                 esp_err_to_name(err));

        err = app_settings_save(&s_settings);
        if (err == ESP_OK) {
            app_settings_load_wifi_profiles();
        }
        return err;
    }

    nvs_close(handle);

    ESP_LOGW(TAG,
             "settings size mismatch, stored=%u expected=%u, save defaults",
             (unsigned)required_size,
             (unsigned)sizeof(app_settings_t));

    err = app_settings_save(&s_settings);
    if (err == ESP_OK) {
        app_settings_load_wifi_profiles();
    }
    return err;
}

/* -------------------------------------------------------------------------- */
/* Setters: WiFi and FMO                                                      */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_wifi(const char *ssid, const char *password)
{
    if (!wifi_ssid_is_valid(ssid) ||
        !wifi_password_is_valid(password ? password : "")) {
        ESP_LOGE(TAG, "invalid wifi ssid or password");
        return ESP_ERR_INVALID_ARG;
    }

    int index = wifi_profiles_find_by_ssid(ssid);

    if (index < 0) {
        index = wifi_profiles_find_empty();
    }

    if (index < 0) {
        index = s_wifi_profiles.active_index;
        if (index >= APP_WIFI_PROFILE_MAX) {
            index = 0;
        }
    }

    return app_settings_set_wifi_profile((uint8_t)index,
                                         ssid,
                                         password ? password : "",
                                         true);
}

esp_err_t app_settings_set_wifi_profile(uint8_t index,
                                        const char *ssid,
                                        const char *password,
                                        bool make_active)
{
    if (index >= APP_WIFI_PROFILE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_ssid_is_valid(ssid) ||
        !wifi_password_is_valid(password ? password : "")) {
        return ESP_ERR_INVALID_ARG;
    }

    app_wifi_profile_t new_profile;
    memset(&new_profile, 0, sizeof(new_profile));
    new_profile.used = 1;

    esp_err_t err = copy_wifi_ssid(new_profile.ssid,
                                   sizeof(new_profile.ssid),
                                   ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy wifi profile ssid failed: %s", esp_err_to_name(err));
        return err;
    }

    err = copy_wifi_password(new_profile.password,
                             sizeof(new_profile.password),
                             password ? password : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy wifi profile password failed: %s", esp_err_to_name(err));
        return err;
    }

    app_settings_t tmp = s_settings;

    if (make_active) {
        err = copy_wifi_ssid(tmp.wifi_ssid,
                             sizeof(tmp.wifi_ssid),
                             new_profile.ssid);
        if (err != ESP_OK) {
            return err;
        }

        err = copy_wifi_password(tmp.wifi_password,
                                 sizeof(tmp.wifi_password),
                                 new_profile.password);
        if (err != ESP_OK) {
            return err;
        }

        err = app_settings_save(&tmp);
        if (err != ESP_OK) {
            return err;
        }

        s_wifi_profiles.active_index = index;
    }

    s_wifi_profiles.profiles[index] = new_profile;

    err = app_settings_save_wifi_profiles();
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "wifi profile %u saved, active=%u, ssid=%s",
                 (unsigned)index,
                 (unsigned)s_wifi_profiles.active_index,
                 new_profile.ssid);
    }

    return err;
}

esp_err_t app_settings_select_wifi_profile(uint8_t index)
{
    if (index >= APP_WIFI_PROFILE_MAX ||
        !s_wifi_profiles.profiles[index].used) {
        return ESP_ERR_INVALID_ARG;
    }

    const app_wifi_profile_t *profile = &s_wifi_profiles.profiles[index];
    app_settings_t tmp = s_settings;

    esp_err_t err = copy_wifi_ssid(tmp.wifi_ssid,
                                   sizeof(tmp.wifi_ssid),
                                   profile->ssid);
    if (err != ESP_OK) {
        return err;
    }

    err = copy_wifi_password(tmp.wifi_password,
                             sizeof(tmp.wifi_password),
                             profile->password);
    if (err != ESP_OK) {
        return err;
    }

    err = app_settings_save(&tmp);
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_profiles.active_index = index;
    return app_settings_save_wifi_profiles();
}

size_t app_settings_get_wifi_profiles(app_wifi_profile_t out_profiles[],
                                      size_t max_profiles,
                                      uint8_t *active_index)
{
    if (active_index) {
        *active_index = s_wifi_profiles.active_index;
    }

    if (!out_profiles || max_profiles == 0) {
        return 0;
    }

    size_t count = max_profiles;
    if (count > APP_WIFI_PROFILE_MAX) {
        count = APP_WIFI_PROFILE_MAX;
    }

    memcpy(out_profiles, s_wifi_profiles.profiles,
           count * sizeof(out_profiles[0]));

    return count;
}

esp_err_t app_settings_set_fmo_host(const char *host)
{
    app_settings_t tmp = s_settings;

    if (!cstr_len_range(host, 1, sizeof(tmp.fmo_host) - 1, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    safe_strcpy(tmp.fmo_host, sizeof(tmp.fmo_host), host);

    app_settings_build_ws_urls(&tmp);

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: audio and display                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_audio_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    app_settings_t tmp = s_settings;

    tmp.audio_volume = volume;

    return app_settings_save(&tmp);
}

esp_err_t app_settings_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    app_settings_t tmp = s_settings;

    tmp.backlight_percent = percent;

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: battery calibration                                               */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_battery_calibration(uint16_t empty_mv,
                                               uint16_t full_mv,
                                               int16_t offset_mv)
{
    if (empty_mv < 2500 || empty_mv > 4200) {
        return ESP_ERR_INVALID_ARG;
    }

    if (full_mv < 3500 || full_mv > 4500) {
        return ESP_ERR_INVALID_ARG;
    }

    if (full_mv <= empty_mv) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = s_settings;

    tmp.battery_empty_mv = empty_mv;
    tmp.battery_full_mv = full_mv;
    tmp.battery_offset_mv = offset_mv;

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: owner callsign                                                    */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_owner_callsign(const char *callsign)
{
    if (!callsign || callsign[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t tmp = s_settings;

    esp_err_t err = copy_owner_callsign(tmp.owner_callsign,
                                        sizeof(tmp.owner_callsign),
                                        callsign);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "copy_owner_callsign failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "set owner_callsign=%s", tmp.owner_callsign);

    return app_settings_save(&tmp);
}

/* -------------------------------------------------------------------------- */
/* Setters: QSO state                                                         */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_qso_state(uint32_t count,
                                     int32_t latest_log_id,
                                     bool valid)
{
    app_settings_t tmp = s_settings;

    tmp.qso_count = count;
    tmp.qso_latest_log_id = latest_log_id;
    tmp.qso_count_valid = valid;

    esp_err_t ret = app_settings_save(&tmp);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "qso state saved: valid=%d count=%lu latest=%ld",
                 valid ? 1 : 0,
                 (unsigned long)count,
                 (long)latest_log_id);
    } else {
        ESP_LOGW(TAG,
                 "qso state save failed: %s",
                 esp_err_to_name(ret));
    }

    return ret;
}

/* -------------------------------------------------------------------------- */
/* Setters: QTH                                                               */
/* -------------------------------------------------------------------------- */
esp_err_t app_settings_set_qth_lookup_enabled(bool enabled)
{
    app_settings_t tmp = s_settings;

    tmp.qth_lookup_enabled = enabled;

    esp_err_t ret = app_settings_save(&tmp);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "qth lookup enabled=%d", enabled ? 1 : 0);
    } else {
        ESP_LOGW(TAG,
                 "save qth lookup enabled failed: %s",
                 esp_err_to_name(ret));
    }

    return ret;
}

/* -------------------------------------------------------------------------- */
/* Setters: buzzer                                                            */
/* -------------------------------------------------------------------------- */
esp_err_t app_settings_set_buzzer_mode(app_buzzer_mode_t mode)
{
    if (mode < APP_BUZZER_MODE_OFF || mode > APP_BUZZER_MODE_CW) {
        mode = APP_BUZZER_MODE_OFF;
    }

    app_settings_t tmp = s_settings;

    tmp.buzzer_mode = mode;

    esp_err_t ret = app_settings_save(&tmp);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "buzzer mode=%d", (int)mode);
    } else {
        ESP_LOGW(TAG,
                 "save buzzer mode failed: %s",
                 esp_err_to_name(ret));
    }

    return ret;
}

const char *app_settings_sound_effect_to_text(app_sound_effect_t effect)
{
    switch (effect) {
    case APP_SOUND_EFFECT_OFF:
        return "关闭";
    case APP_SOUND_EFFECT_BEEP:
        return "短音";
    case APP_SOUND_EFFECT_CW_DE:
        return "CW DE";
    case APP_SOUND_EFFECT_POWER_ON:
        return "诺基亚";
    case APP_SOUND_EFFECT_C4_COUNTDOWN:
        return "C4倒计时";
    case APP_SOUND_EFFECT_MARIO:
        return "马里奥1";
    case APP_SOUND_EFFECT_MARIO_START:
        return "马里奥2";
    case APP_SOUND_EFFECT_MARIO_FAIL:
        return "马里奥3";
    case APP_SOUND_EFFECT_CONTRA_JUNGLE:
        return "魂斗罗";
    case APP_SOUND_EFFECT_GEIGER:
        return "盖革计数";
    default:
        return "未知";
    }
}

esp_err_t app_settings_set_startup_sound_effect(app_sound_effect_t effect)
{
    if (effect < APP_SOUND_EFFECT_OFF || effect >= APP_SOUND_EFFECT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t cfg = s_settings;
    cfg.startup_sound_effect = effect;

    return app_settings_save(&cfg);
}

esp_err_t app_settings_set_power_save_sound_effect(app_sound_effect_t effect)
{
    if (effect < APP_SOUND_EFFECT_OFF || effect >= APP_SOUND_EFFECT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t cfg = s_settings;
    cfg.power_save_sound_effect = effect;

    return app_settings_save(&cfg);
}

const char *app_settings_battery_display_mode_to_text(app_battery_display_mode_t mode)
{
    switch (mode) {
    case APP_BATTERY_DISPLAY_ICON:
        return "图标";
    case APP_BATTERY_DISPLAY_PERCENT:
        return "百分比";
    case APP_BATTERY_DISPLAY_VOLTAGE:
        return "电压";
    default:
        return "图标";
    }
}

esp_err_t app_settings_set_battery_display_mode(app_battery_display_mode_t mode)
{
    if (mode < APP_BATTERY_DISPLAY_ICON ||
        mode >= APP_BATTERY_DISPLAY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t cfg = s_settings;
    cfg.battery_display_mode = mode;

    return app_settings_save(&cfg);
}


/* -------------------------------------------------------------------------- */
/* Setters: WireGuard                                                         */
/* -------------------------------------------------------------------------- */

esp_err_t app_settings_set_wireguard_config(bool enabled,
                                            const char *private_key,
                                            const char *local_ip,
                                            const char *local_netmask,
                                            const char *peer_public_key,
                                            const char *peer_endpoint,
                                            uint16_t peer_port,
                                            uint16_t listen_port,
                                            uint16_t persistent_keepalive)
{
    if (enabled) {
        if (!wireguard_key_is_valid(private_key,
                                    sizeof(s_settings.wg_private_key)) ||
            !wireguard_key_is_valid(peer_public_key,
                                    sizeof(s_settings.wg_peer_public_key)) ||
            !ipv4_addr_is_valid(local_ip) ||
            !ipv4_addr_is_valid(local_netmask) ||
            !wireguard_endpoint_is_valid(peer_endpoint) ||
            peer_port == 0 ||
            listen_port == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    app_settings_t cfg = s_settings;

    cfg.wg_enabled = enabled;

    if (private_key && private_key[0]) {
        safe_strcpy(cfg.wg_private_key,
                    sizeof(cfg.wg_private_key),
                    private_key);
    }

    if (local_ip && local_ip[0]) {
        safe_strcpy(cfg.wg_local_ip,
                    sizeof(cfg.wg_local_ip),
                    local_ip);
    }

    if (local_netmask && local_netmask[0]) {
        safe_strcpy(cfg.wg_local_netmask,
                    sizeof(cfg.wg_local_netmask),
                    local_netmask);
    }

    if (peer_public_key && peer_public_key[0]) {
        safe_strcpy(cfg.wg_peer_public_key,
                    sizeof(cfg.wg_peer_public_key),
                    peer_public_key);
    }

    if (peer_endpoint && peer_endpoint[0]) {
        safe_strcpy(cfg.wg_peer_endpoint,
                    sizeof(cfg.wg_peer_endpoint),
                    peer_endpoint);
    }

    if (peer_port != 0) {
        cfg.wg_peer_port = peer_port;
    }

    if (listen_port != 0) {
        cfg.wg_listen_port = listen_port;
    }

    cfg.wg_persistent_keepalive = persistent_keepalive;

    return app_settings_save(&cfg);
}

const char *app_settings_color_theme_to_text(app_color_theme_t theme)
{
    switch (theme) {
    case APP_COLOR_THEME_DARK_ORANGE:
        return "黑橙";

    case APP_COLOR_THEME_LIGHT_BLUE:
        return "白蓝";

    default:
        return "黑橙";
    }
}

esp_err_t app_settings_set_color_theme(app_color_theme_t theme)
{
    if (theme < APP_COLOR_THEME_DARK_ORANGE ||
        theme >= APP_COLOR_THEME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t cfg = s_settings;
    cfg.color_theme = theme;

    return app_settings_save(&cfg);
}
