/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_settings.h
 * @brief 应用配置管理接口。
 *
 * 本模块负责应用运行配置的默认值加载、NVS 持久化保存、
 * 配置版本校验以及运行期配置更新。
 *
 * 当前管理的配置包括：
 * - WiFi SSID / 密码；
 * - FMO Host；
 * - WebSocket URL；
 * - 音量；
 * - 背光；
 * - 待机时钟；
 * - 电池校准；
 * - 本机呼号；
 * - QSO 同步状态；
 * - 屏幕旋转状态。
 *
 * @note
 * 修改 app_settings_t 结构体字段后，需要提升 APP_SETTINGS_VERSION。
 * 旧 NVS blob 会被判定无效，然后自动写入默认配置。
 */

#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Version                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 配置结构体版本号。
 *
 * 修改 app_settings_t 的字段布局、字段大小或字段含义后，
 * 应提升该版本号，避免旧 NVS blob 被错误解释。
 */
#define APP_SETTINGS_VERSION  18

/**
 * @brief 最多保存的 WiFi 配置组数量。
 *
 * 多组 WiFi 单独保存到 NVS key，不改变 app_settings_t 主 blob 布局。
 */
#define APP_WIFI_PROFILE_MAX 3

/* -------------------------------------------------------------------------- */
/* buzzer                                                                    */
/* -------------------------------------------------------------------------- */
typedef enum {
    APP_BUZZER_MODE_OFF = 0,
    APP_BUZZER_MODE_BEEP,
    APP_BUZZER_MODE_CW,
} app_buzzer_mode_t;


typedef enum {
    APP_SOUND_EFFECT_OFF = 0,
    APP_SOUND_EFFECT_BEEP,
    APP_SOUND_EFFECT_CW_DE,
    APP_SOUND_EFFECT_POWER_ON,
    APP_SOUND_EFFECT_C4_COUNTDOWN,
    APP_SOUND_EFFECT_MARIO,
    APP_SOUND_EFFECT_MARIO_START,
    APP_SOUND_EFFECT_MARIO_FAIL,
    APP_SOUND_EFFECT_CONTRA_JUNGLE,
    APP_SOUND_EFFECT_GEIGER,
    APP_SOUND_EFFECT_MAX,
} app_sound_effect_t;

typedef enum {
    APP_BATTERY_DISPLAY_ICON = 0,
    APP_BATTERY_DISPLAY_PERCENT,
    APP_BATTERY_DISPLAY_VOLTAGE,
    APP_BATTERY_DISPLAY_MAX,
} app_battery_display_mode_t;

typedef enum {
    APP_COLOR_THEME_DARK_ORANGE = 0,
    APP_COLOR_THEME_LIGHT_BLUE,
    APP_COLOR_THEME_MAX,
} app_color_theme_t;

/* -------------------------------------------------------------------------- */
/* Public types                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief 单组 WiFi 配置。
 *
 * used 为 0 表示该槽位为空；非 0 表示已保存。
 */
typedef struct {
    uint8_t used;
    char ssid[33];
    char password[64];
} app_wifi_profile_t;

/**
 * @brief 应用配置结构体。
 *
 * 该结构体整体以 blob 形式保存到 NVS。
 *
 * @note
 * 修改该结构体后，请同步提升 APP_SETTINGS_VERSION。
 */
typedef struct {
    /**
     * @brief 配置版本号。
     */
    uint32_t version;

    /**
     * @brief 屏幕背光百分比，范围 0~100。
     */
    uint8_t backlight_percent;

    /**
     * @brief 音频音量百分比，范围 0~100。
     */
    uint8_t audio_volume;

    /**
     * @brief 是否启用待机时钟页。
     *
     * 0：禁用
     * 1：启用
     */
    uint8_t idle_image_enabled;

    /**
     * @brief WiFi SSID。
     *
     * 802.11 SSID 最大长度为 32 字节。
     * 这里使用 33 字节用于保存结尾 '\0'。
     *
     * @note
     * 这里限制的是字节数，不是字符数。
     * 中文 UTF-8 SSID 中，一个汉字通常占 3 字节。
     */
    char wifi_ssid[33];

    /**
     * @brief WiFi 密码。
     *
     * WPA/WPA2 密码最长 63 字节，额外 1 字节保存 '\0'。
     */
    char wifi_password[64];

    /**
     * @brief FMO 服务 Host。
     *
     * 用户设置页只填写该字段，例如：
     *
     * @code
     * 192.168.3.165
     * example.com:8080
     * @endcode
     *
     * 保存时会自动生成：
     *
     * @code
     * ws_audio_url   = ws://<fmo_host>/audio
     * ws_event_url   = ws://<fmo_host>/events
     * ws_station_url = ws://<fmo_host>/ws
     * @endcode
     */
    char fmo_host[64];

    /**
     * @brief 音频 WebSocket URL。
     */
    char ws_audio_url[128];

    /**
     * @brief 事件 WebSocket URL。
     */
    char ws_event_url[128];

    /**
     * @brief 站点 WebSocket URL。
     */
    char ws_station_url[128];

    /**
     * @brief 当前站点刷新间隔，单位 ms。
     */
    uint32_t ws_station_current_refresh_ms;

    /**
     * @brief 站点列表刷新间隔，单位 ms。
     */
    uint32_t ws_station_list_refresh_ms;

    /**
     * @brief 待机时钟超时时间，单位 ms。
     */
    uint32_t idle_image_timeout_ms;

    /**
     * @brief 电池空电电压，单位 mV。
     */
    uint16_t battery_empty_mv;

    /**
     * @brief 电池满电电压，单位 mV。
     */
    uint16_t battery_full_mv;

    /**
     * @brief 电池采样偏移校准值，单位 mV。
     */
    int16_t battery_offset_mv;

    /**
     * @brief 本机呼号。
     */
    char owner_callsign[16];

    /**
     * @brief 已同步的 QSO 数量。
     */
    uint32_t qso_count;

    /**
     * @brief 最近一次已同步的 log id。
     */
    int32_t qso_latest_log_id;

    /**
     * @brief QSO 数量是否有效。
     */
    bool qso_count_valid;

    /**
     * @brief 屏幕是否旋转 180 度。
     */
    bool screen_rotate_180;

    /*
    * QTH 地址查询开关。
    * 默认关闭。
    */
    bool qth_lookup_enabled;

    /*
    * 提示音选择。
    * 默认关闭。
    */
    app_buzzer_mode_t buzzer_mode;
        /*
     * 开机音效。
     */
    app_sound_effect_t startup_sound_effect;

    /*
     * 节能模式切换音效。
     */
    app_sound_effect_t power_save_sound_effect;

    app_battery_display_mode_t battery_display_mode;


    /**
     * @brief 是否启用 WireGuard。
     */
    bool wg_enabled;

    /**
     * @brief WireGuard ESP32 私钥，Base64 文本。
     */
    char wg_private_key[64];

    /**
     * @brief ESP32 WireGuard 隧道 IP。
     */
    char wg_local_ip[16];

    /**
     * @brief ESP32 WireGuard 隧道掩码。
     */
    char wg_local_netmask[16];

    /**
     * @brief WireGuard 服务器公钥，Base64 文本。
     */
    char wg_peer_public_key[64];

    /**
     * @brief WireGuard 服务器地址，可为 IP 或域名。
     */
    char wg_peer_endpoint[64];

    /**
     * @brief WireGuard 服务器 UDP 端口。
     */
    uint16_t wg_peer_port;

    /**
     * @brief 本地 WireGuard 监听端口。
     */
    uint16_t wg_listen_port;

    /**
     * @brief Persistent Keepalive，单位秒。
     */
    uint16_t wg_persistent_keepalive;

    /**
    *  @brief UI 配色主题。
    */
    app_color_theme_t color_theme;


} app_settings_t;

/* -------------------------------------------------------------------------- */
/* Initialization and access                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化配置模块。
 *
 * 初始化流程：
 * 1. 先加载默认配置到内存；
 * 2. 尝试从 NVS 读取配置 blob；
 * 3. 校验版本和字段合法性；
 * 4. 如果 NVS 配置有效，则使用 NVS 配置；
 * 5. 如果 NVS 中没有配置或配置无效，则保存默认配置。
 *
 * @return
 *      - ESP_OK：初始化成功
 *      - 其他值：NVS 打开或保存失败
 */
esp_err_t app_settings_init(void);

/**
 * @brief 从当前内存配置复制一份到 cfg。
 *
 * @param cfg 输出配置指针，不能为空。
 *
 * @return
 *      - ESP_OK：复制成功
 *      - ESP_ERR_INVALID_ARG：参数为空
 */
esp_err_t app_settings_load(app_settings_t *cfg);

/**
 * @brief 保存配置到 NVS，并更新内存中的当前配置。
 *
 * 保存前会对配置进行 sanitize，并重新生成 WebSocket URL。
 *
 * @param cfg 待保存配置指针，不能为空。
 *
 * @return
 *      - ESP_OK：保存成功
 *      - ESP_ERR_INVALID_ARG：参数或字段非法
 *      - 其他值：NVS 操作失败
 */
esp_err_t app_settings_save(const app_settings_t *cfg);

/**
 * @brief 获取当前配置指针。
 *
 * @return 当前配置的只读指针。
 *
 * @note
 * 返回的是内部静态对象指针，调用方不应直接修改其内容。
 * 如需修改配置，请复制一份后通过 app_settings_save()
 * 或对应 setter 接口保存。
 */
const app_settings_t *app_settings_get(void);

/**
 * @brief 加载默认配置到 cfg。
 *
 * @param cfg 配置输出指针，不能为空。
 */
void app_settings_load_defaults(app_settings_t *cfg);

/**
 * @brief 根据 cfg->fmo_host 自动生成三个 WebSocket URL。
 *
 * @param cfg 配置指针，不能为空。
 */
void app_settings_build_ws_urls(app_settings_t *cfg);

/* -------------------------------------------------------------------------- */
/* Setters                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 设置 WiFi SSID 和密码。
 *
 * 该函数只修改内存配置并保存到 NVS，不会立即重连 WiFi。
 * 当前 UI 逻辑建议保存后重启生效。
 *
 * @param ssid WiFi SSID，不能为空，最大 32 字节。
 * @param password WiFi 密码，可为空字符串，最大 63 字节。
 *
 * @return
 *      - ESP_OK：保存成功
 *      - ESP_ERR_INVALID_ARG：参数非法
 *      - ESP_ERR_INVALID_SIZE：目标缓存不足
 *      - 其他值：NVS 保存失败
 */
esp_err_t app_settings_set_wifi(const char *ssid, const char *password);

/**
 * @brief 保存指定槽位的 WiFi，并可设置为当前激活 WiFi。
 *
 * @param index 槽位序号，范围 0~APP_WIFI_PROFILE_MAX-1。
 * @param ssid WiFi SSID，不能为空，最大 32 字节。
 * @param password WiFi 密码，可为空字符串，最大 63 字节。
 * @param make_active true 表示同时设置为当前激活 WiFi。
 */
esp_err_t app_settings_set_wifi_profile(uint8_t index,
                                        const char *ssid,
                                        const char *password,
                                        bool make_active);

/**
 * @brief 选择已保存的 WiFi 槽位作为当前激活 WiFi。
 */
esp_err_t app_settings_select_wifi_profile(uint8_t index);

/**
 * @brief 获取 WiFi 槽位列表。
 *
 * @return 实际复制到 out_profiles 的槽位数量。槽位是否有效由 used 字段表示。
 */
size_t app_settings_get_wifi_profiles(app_wifi_profile_t out_profiles[],
                                      size_t max_profiles,
                                      uint8_t *active_index);

/**
 * @brief 设置 FMO Host。
 *
 * 例如：
 *
 * @code
 * 192.168.3.165
 * example.com:8080
 * @endcode
 *
 * 内部会自动生成：
 *
 * @code
 * ws://<host>/audio
 * ws://<host>/events
 * ws://<host>/ws
 * @endcode
 *
 * @param host FMO Host，不能为空。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_fmo_host(const char *host);

/**
 * @brief 设置音量。
 *
 * @param volume 音量百分比，范围 0~100，超过 100 会被限制为 100。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_audio_volume(uint8_t volume);

/**
 * @brief 设置背光。
 *
 * @param percent 背光百分比，范围 0~100，超过 100 会被限制为 100。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_backlight(uint8_t percent);

/**
 * @brief 设置电池校准参数。
 *
 * @param empty_mv 空电电压，单位 mV。
 * @param full_mv 满电电压，单位 mV。
 * @param offset_mv 电压偏移校准值，单位 mV。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_battery_calibration(uint16_t empty_mv,
                                               uint16_t full_mv,
                                               int16_t offset_mv);

/**
 * @brief 设置本机呼号。
 *
 * 小写字母会自动转换为大写。
 *
 * @param callsign 本机呼号，不能为空。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_owner_callsign(const char *callsign);

/**
 * @brief 设置 QSO 同步状态。
 *
 * @param count 当前 QSO 数量。
 * @param latest_log_id 最近一次同步的 log id。
 * @param valid true 表示 QSO 数量有效。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_qso_state(uint32_t count,
                                     int32_t latest_log_id,
                                     bool valid);

/**
 * @brief 设置屏幕是否旋转 180 度。
 *
 * @param enabled true 表示旋转 180 度。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_screen_rotate_180(bool enabled);



esp_err_t app_settings_set_qth_lookup_enabled(bool enabled);


esp_err_t app_settings_set_buzzer_mode(app_buzzer_mode_t mode);

esp_err_t app_settings_set_startup_sound_effect(app_sound_effect_t effect);

esp_err_t app_settings_set_power_save_sound_effect(app_sound_effect_t effect);

const char *app_settings_sound_effect_to_text(app_sound_effect_t effect);

esp_err_t app_settings_set_battery_display_mode(app_battery_display_mode_t mode);

const char *app_settings_battery_display_mode_to_text(app_battery_display_mode_t mode);


/**
 * @brief 保存 WireGuard 配置。
 *
 * @return ESP_OK 或错误码。
 */
esp_err_t app_settings_set_wireguard_config(bool enabled,
                                            const char *private_key,
                                            const char *local_ip,
                                            const char *local_netmask,
                                            const char *peer_public_key,
                                            const char *peer_endpoint,
                                            uint16_t peer_port,
                                            uint16_t listen_port,
                                            uint16_t persistent_keepalive);

/**
 * @brief 保存 配色配置。
 *
 */
esp_err_t app_settings_set_color_theme(app_color_theme_t theme);

const char *app_settings_color_theme_to_text(app_color_theme_t theme);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H */
