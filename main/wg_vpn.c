#include "wg_vpn.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wireguard.h"

#include "app_settings.h"
#include "ui_async.h"

/* 部分 esp_wireguard.h 版本未公开声明该接口，这里补充同签名声明。 */
esp_err_t esp_wireguardif_peer_is_up(wireguard_ctx_t *ctx);

static const char *TAG = "wg_vpn";

static wireguard_ctx_t s_ctx = {0};
static wireguard_config_t s_wg_cfg = {0};
static bool s_started = false;
static bool s_connected = false;
static TaskHandle_t s_status_task_handle = NULL;

/* 等 SNTP 校时完成（年份 > 2020 视为已同步）。 */
static bool time_is_synced(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    return (tm_now.tm_year + 1900) > 2020;
}

static bool wait_time_synced(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (!time_is_synced()) {
        if (waited_ms >= timeout_ms) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        waited_ms += 500;
    }

    return true;
}

static void wg_status_task(void *arg)
{
    (void)arg;

    bool last_connected = false;

    while (s_started) {
        bool connected = false;

        if (s_ctx.netif) {
            connected = (esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK);
        }

        if (connected != last_connected || connected != s_connected) {
            s_connected = connected;
            last_connected = connected;

            ui_async_update_wireguard_status(connected);

            ESP_LOGI(TAG,
                     "WireGuard peer %s",
                     connected ? "connected" : "not connected");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    s_connected = false;
    ui_async_update_wireguard_status(false);
    s_status_task_handle = NULL;

    vTaskDelete(NULL);
}

static bool wg_config_has_required_fields(const app_settings_t *cfg)
{
    return cfg &&
           cfg->wg_private_key[0] &&
           cfg->wg_peer_public_key[0] &&
           cfg->wg_local_ip[0] &&
           cfg->wg_local_netmask[0] &&
           cfg->wg_peer_endpoint[0] &&
           cfg->wg_peer_port != 0 &&
           cfg->wg_listen_port != 0;
}

esp_err_t wg_vpn_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    const app_settings_t *settings = app_settings_get();
    if (!settings || !settings->wg_enabled) {
        ESP_LOGI(TAG, "WireGuard disabled by settings");
        s_connected = false;
        ui_async_update_wireguard_status(false);
        return ESP_OK;
    }

    if (!wg_config_has_required_fields(settings)) {
        ESP_LOGE(TAG, "WireGuard settings incomplete, abort WG start");
        ui_async_update_status("WG配置缺失");
        s_connected = false;
        ui_async_update_wireguard_status(false);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * audio_ws.c 中启动 SNTP。
     * 这里不重复启动 SNTP，只等待系统时间已经同步。
     */
    ESP_LOGI(TAG, "waiting for time sync before WG...");
    ui_async_update_status("等待WG校时");

    if (!wait_time_synced(60000)) {
        ESP_LOGE(TAG, "time sync timeout, abort WG start");
        ui_async_update_status("WG校时超时");
        s_connected = false;
        ui_async_update_wireguard_status(false);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "time synced, starting WireGuard");
    ui_async_update_status("启动WireGuard");

    /*
     * 网络栈稳定一下，避免刚获取 IP 时 netif 回调拥挤。
     */
    vTaskDelay(pdMS_TO_TICKS(500));

    memset(&s_ctx, 0, sizeof(s_ctx));

    /*
     * ESP_WIREGUARD_CONFIG_DEFAULT() 在该组件中是花括号初始化器，
     * 不能用于赋值语句；这里手动清零并填写需要的字段。
     *
     * esp_wireguard_init() 会保存 config 指针，因此必须使用静态 s_wg_cfg，
     * 不能使用函数栈上的局部 wireguard_config_t。
     */
    memset(&s_wg_cfg, 0, sizeof(s_wg_cfg));

    s_wg_cfg.private_key = (char *)settings->wg_private_key;
    s_wg_cfg.listen_port = settings->wg_listen_port;
    s_wg_cfg.public_key = (char *)settings->wg_peer_public_key;
    s_wg_cfg.allowed_ip = (char *)settings->wg_local_ip;
    s_wg_cfg.allowed_ip_mask = (char *)settings->wg_local_netmask;
    s_wg_cfg.endpoint = (char *)settings->wg_peer_endpoint;
    s_wg_cfg.port = settings->wg_peer_port;
    s_wg_cfg.persistent_keepalive = settings->wg_persistent_keepalive;
    s_wg_cfg.preshared_key = NULL;

    esp_err_t err = esp_wireguard_init(&s_wg_cfg, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wireguard init failed: %s", esp_err_to_name(err));
        ui_async_update_status("WG初始化失败");
        s_connected = false;
        ui_async_update_wireguard_status(false);
        return err;
    }

    err = esp_wireguard_connect(&s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wireguard connect failed: %s", esp_err_to_name(err));
        ui_async_update_status("WG连接失败");
        s_connected = false;
        ui_async_update_wireguard_status(false);
        return err;
    }

    s_started = true;
    s_connected = false;
    ui_async_update_wireguard_status(false);

    if (!s_status_task_handle) {
        BaseType_t task_ok = xTaskCreate(wg_status_task,
                                         "wg_status",
                                         3072,
                                         NULL,
                                         3,
                                         &s_status_task_handle);
        if (task_ok != pdPASS) {
            s_status_task_handle = NULL;
            ESP_LOGW(TAG, "create wg_status task failed");
        }
    }

    ESP_LOGI(TAG,
             "WireGuard started, local ip=%s, endpoint=%s:%u, keepalive=%u",
             settings->wg_local_ip,
             settings->wg_peer_endpoint,
             (unsigned)settings->wg_peer_port,
             (unsigned)settings->wg_persistent_keepalive);

    ui_async_update_status("WG已启动");

    return ESP_OK;
}

esp_err_t wg_vpn_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    s_started = false;
    s_connected = false;
    ui_async_update_wireguard_status(false);

    if (s_status_task_handle) {
        TaskHandle_t task = s_status_task_handle;
        s_status_task_handle = NULL;
        vTaskDelete(task);
    }

    esp_err_t err = esp_wireguard_disconnect(&s_ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wireguard disconnect failed: %s", esp_err_to_name(err));
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    memset(&s_wg_cfg, 0, sizeof(s_wg_cfg));

    ui_async_update_status("WG已停止");

    return err;
}

bool wg_vpn_is_started(void)
{
    return s_started;
}

bool wg_vpn_is_connected(void)
{
    return s_connected;
}
