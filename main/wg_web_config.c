#include "wg_web_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_settings.h"
#include "ui_async.h"
#include "wifi_manager.h"
#include "wg_vpn.h"

static const char *TAG = "wg_web_config";

#define WG_WEB_PORT             80
#define WG_WEB_AUTO_STOP_US     (5LL * 60LL * 1000000LL)
#define WG_WEB_MAX_BODY_LEN     3072

static httpd_handle_t s_httpd = NULL;
static esp_timer_handle_t s_stop_timer = NULL;
static char s_url[64] = {0};

static void safe_copy(char *dst, size_t dst_size, const char *src)
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

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void url_decode(char *s)
{
    char *src = s;
    char *dst = s;

    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            int hi = hex_to_int(src[1]);
            int lo = hex_to_int(src[2]);

            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_size)
{
    if (!body || !key || !out || out_size == 0) {
        return false;
    }

    out[0] = '\0';

    size_t key_len = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;

            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);

            if (len >= out_size) {
                len = out_size - 1;
            }

            memcpy(out, p, len);
            out[len] = '\0';
            url_decode(out);

            return true;
        }

        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }

    return false;
}

static esp_err_t send_chunk(httpd_req_t *req, const char *s)
{
    if (!s) {
        s = "";
    }

    return httpd_resp_send_chunk(req, s, strlen(s));
}

static esp_err_t send_escaped(httpd_req_t *req, const char *s)
{
    if (!s) {
        return ESP_OK;
    }

    for (size_t i = 0; s[i]; i++) {
        const char *rep = NULL;
        char one[2] = { s[i], '\0' };

        switch (s[i]) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        case '\'':
            rep = "&#39;";
            break;
        default:
            rep = one;
            break;
        }

        esp_err_t err = send_chunk(req, rep);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t send_u32(httpd_req_t *req, unsigned value)
{
    char buf[16];

    snprintf(buf, sizeof(buf), "%u", value);

    return send_chunk(req, buf);
}

static esp_err_t send_page_header(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

    esp_err_t err = send_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"referrer\" content=\"no-referrer\">"
        "<title>");
    if (err != ESP_OK) {
        return err;
    }

    err = send_escaped(req, title ? title : "WireGuard Config");
    if (err != ESP_OK) {
        return err;
    }

    return send_chunk(req,
        "</title>"
        "<style>"
        "body{font-family:Arial;background:#111;color:#eee;margin:16px;}"
        ".card{max-width:720px;margin:auto;background:#222;padding:18px;border-radius:8px;}"
        "label{display:block;margin-top:12px;color:#ddd;}"
        "input{width:100%;padding:10px;margin-top:5px;box-sizing:border-box;background:#333;color:#fff;border:1px solid #555;border-radius:4px;}"
        "input[type=checkbox]{width:auto;margin-right:8px;}"
        "button{margin-top:18px;padding:12px 20px;background:#2d8cff;color:#fff;border:0;border-radius:4px;font-size:16px;}"
        ".hint{color:#aaa;font-size:13px;margin-top:4px;line-height:1.4;}"
        ".status{background:#111;padding:10px;border-radius:4px;margin-bottom:12px;color:#ccc;line-height:1.5;}"
        ".msg{background:#243b24;color:#9f9;padding:10px;border-radius:4px;margin-bottom:12px;}"
        ".err{background:#3b2424;color:#f99;padding:10px;border-radius:4px;margin-bottom:12px;}"
        ".row{display:flex;gap:12px;}.row>div{flex:1;}"
        "@media(max-width:560px){.row{display:block;}}"
        "</style></head><body><div class=\"card\">");
}

static esp_err_t send_config_page(httpd_req_t *req, const char *message, bool is_error)
{
    const app_settings_t *cfg = app_settings_get();

    const char *ip = wifi_manager_get_ip_string();
    if (!ip || ip[0] == '\0') {
        ip = "0.0.0.0";
    }

    const char *private_hint =
        (cfg && cfg->wg_private_key[0]) ? "已保存，留空不修改" : "未设置";

    const char *public_hint =
        (cfg && cfg->wg_peer_public_key[0]) ? "已保存，留空不修改" : "未设置";

    esp_err_t err = send_page_header(req, "WireGuard Config");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req, "<h2>WireGuard 设置</h2>");
    if (err != ESP_OK) {
        return err;
    }

    if (message && message[0]) {
        err = send_chunk(req, is_error ? "<div class=\"err\">" : "<div class=\"msg\">");
        if (err != ESP_OK) {
            return err;
        }

        err = send_escaped(req, message);
        if (err != ESP_OK) {
            return err;
        }

        err = send_chunk(req, "</div>");
        if (err != ESP_OK) {
            return err;
        }
    }

    err = send_chunk(req, "<div class=\"status\">当前 WiFi IP: ");
    if (err != ESP_OK) {
        return err;
    }
    err = send_escaped(req, ip);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req, "<br>配置服务 5 分钟后自动关闭<br>WireGuard: ");
    if (err != ESP_OK) {
        return err;
    }
    err = send_chunk(req, (cfg && cfg->wg_enabled) ? "已启用" : "已禁用");
    if (err != ESP_OK) {
        return err;
    }
    err = send_chunk(req, " / ");
    if (err != ESP_OK) {
        return err;
    }
    err = send_chunk(req, wg_vpn_is_started() ? "已启动" : "未启动");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "</div>"
        "<form method=\"POST\" action=\"/wg\" autocomplete=\"off\" "
        "enctype=\"application/x-www-form-urlencoded\" accept-charset=\"UTF-8\">"
        "<label><input type=\"checkbox\" name=\"wg_enabled\" value=\"1\" ");
    if (err != ESP_OK) {
        return err;
    }

    if (cfg && cfg->wg_enabled) {
        err = send_chunk(req, "checked");
        if (err != ESP_OK) {
            return err;
        }
    }

    err = send_chunk(req,
        ">启用 WireGuard</label>"
        "<label>ESP32 私钥</label>"
        "<input name=\"wg_private_key\" value=\"\" maxlength=\"63\" type=\"password\" "
        "autocomplete=\"new-password\" placeholder=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_escaped(req, private_hint);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\">"
        "<div class=\"hint\">为了安全，私钥不回显；留空表示不修改。</div>"
        "<label>ESP32 VPN IP</label>"
        "<input name=\"wg_local_ip\" value=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_escaped(req, cfg ? cfg->wg_local_ip : "");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\" maxlength=\"15\" placeholder=\"10.0.0.4\">"
        "<label>ESP32 VPN 掩码</label>"
        "<input name=\"wg_local_netmask\" value=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_escaped(req, cfg ? cfg->wg_local_netmask : "");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\" maxlength=\"15\" placeholder=\"255.255.255.0\">"
        "<label>服务器公钥</label>"
        "<input name=\"wg_peer_public_key\" value=\"\" maxlength=\"63\" "
        "autocomplete=\"off\" placeholder=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_escaped(req, public_hint);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\">"
        "<div class=\"hint\">服务器公钥留空表示不修改。</div>"
        "<label>服务器地址</label>"
        "<input name=\"wg_peer_endpoint\" value=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_escaped(req, cfg ? cfg->wg_peer_endpoint : "");
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\" maxlength=\"63\" placeholder=\"example.com 或 1.2.3.4\">"
        "<div class=\"row\"><div>"
        "<label>服务器端口</label>"
        "<input name=\"wg_peer_port\" value=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_u32(req, cfg ? (unsigned)cfg->wg_peer_port : 51820);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\" maxlength=\"5\" inputmode=\"numeric\">"
        "</div><div>"
        "<label>本地监听端口</label>"
        "<input name=\"wg_listen_port\" value=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_u32(req, cfg ? (unsigned)cfg->wg_listen_port : 51820);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\" maxlength=\"5\" inputmode=\"numeric\">"
        "</div></div>"
        "<label>Persistent Keepalive 秒</label>"
        "<input name=\"wg_keepalive\" value=\"");
    if (err != ESP_OK) {
        return err;
    }
    err = send_u32(req, cfg ? (unsigned)cfg->wg_persistent_keepalive : 25);
    if (err != ESP_OK) {
        return err;
    }

    err = send_chunk(req,
        "\" maxlength=\"5\" inputmode=\"numeric\">"
        "<div class=\"hint\">常用 25 秒。保存后建议重启设备，让 WireGuard 按新配置重新初始化。</div>"
        "<button type=\"submit\">保存 WireGuard 设置</button>"
        "</form>"
        "</div></body></html>");
    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t wg_get_handler(httpd_req_t *req)
{
    return send_config_page(req, NULL, false);
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    char buf[192];

    const app_settings_t *cfg = app_settings_get();
    const char *ip = wifi_manager_get_ip_string();
    if (!ip || ip[0] == '\0') {
        ip = "0.0.0.0";
    }

    snprintf(buf,
             sizeof(buf),
             "ip=%s\nwg_enabled=%d\nwg_started=%d\nendpoint=%s:%u\n",
             ip,
             (cfg && cfg->wg_enabled) ? 1 : 0,
             wg_vpn_is_started() ? 1 : 0,
             cfg ? cfg->wg_peer_endpoint : "",
             cfg ? (unsigned)cfg->wg_peer_port : 0);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, buf);

    return ESP_OK;
}

static esp_err_t wg_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > WG_WEB_MAX_BODY_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }

    char *body = calloc(1, (size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    int received = 0;

    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }

            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }

        received += ret;
    }

    body[total] = '\0';

    const app_settings_t *old = app_settings_get();
    app_settings_t cfg;

    if (old) {
        cfg = *old;
    } else {
        memset(&cfg, 0, sizeof(cfg));
        app_settings_load_defaults(&cfg);
    }

    char v[192];

    cfg.wg_enabled = form_get(body, "wg_enabled", v, sizeof(v));

    if (form_get(body, "wg_private_key", v, sizeof(v)) && v[0]) {
        safe_copy(cfg.wg_private_key, sizeof(cfg.wg_private_key), v);
    }

    if (form_get(body, "wg_peer_public_key", v, sizeof(v)) && v[0]) {
        safe_copy(cfg.wg_peer_public_key, sizeof(cfg.wg_peer_public_key), v);
    }

    if (form_get(body, "wg_local_ip", v, sizeof(v)) && v[0]) {
        safe_copy(cfg.wg_local_ip, sizeof(cfg.wg_local_ip), v);
    }

    if (form_get(body, "wg_local_netmask", v, sizeof(v)) && v[0]) {
        safe_copy(cfg.wg_local_netmask, sizeof(cfg.wg_local_netmask), v);
    }

    if (form_get(body, "wg_peer_endpoint", v, sizeof(v)) && v[0]) {
        safe_copy(cfg.wg_peer_endpoint, sizeof(cfg.wg_peer_endpoint), v);
    }

    if (form_get(body, "wg_peer_port", v, sizeof(v)) && v[0]) {
        unsigned long port = strtoul(v, NULL, 10);
        if (port > 0 && port <= 65535) {
            cfg.wg_peer_port = (uint16_t)port;
        }
    }

    if (form_get(body, "wg_listen_port", v, sizeof(v)) && v[0]) {
        unsigned long port = strtoul(v, NULL, 10);
        if (port > 0 && port <= 65535) {
            cfg.wg_listen_port = (uint16_t)port;
        }
    }

    if (form_get(body, "wg_keepalive", v, sizeof(v)) && v[0]) {
        unsigned long keepalive = strtoul(v, NULL, 10);
        if (keepalive <= 65535) {
            cfg.wg_persistent_keepalive = (uint16_t)keepalive;
        }
    }

    free(body);

    esp_err_t err = app_settings_set_wireguard_config(cfg.wg_enabled,
                                                      cfg.wg_private_key,
                                                      cfg.wg_local_ip,
                                                      cfg.wg_local_netmask,
                                                      cfg.wg_peer_public_key,
                                                      cfg.wg_peer_endpoint,
                                                      cfg.wg_peer_port,
                                                      cfg.wg_listen_port,
                                                      cfg.wg_persistent_keepalive);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save WireGuard settings failed: %s", esp_err_to_name(err));
        return send_config_page(req, "保存失败，请检查字段格式。启用 WireGuard 时私钥、公钥、IP、端口都必须有效。", true);
    }

    ui_async_update_status("WG设置已保存");

    return send_config_page(req, "WireGuard 设置已保存。请重启设备让新配置完全生效。", false);
}

static void wg_web_stop_task(void *arg)
{
    (void)arg;

    wg_web_config_stop();

    vTaskDelete(NULL);
}

static void stop_timer_cb(void *arg)
{
    (void)arg;

    xTaskCreate(wg_web_stop_task,
                "wg_web_stop",
                3072,
                NULL,
                3,
                NULL);
}

esp_err_t wg_web_config_start(void)
{
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, cannot start WG web config");
        ui_async_update_status("WiFi未连接");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_url,
             sizeof(s_url),
             "http://%s/wg",
             wifi_manager_get_ip_string());

    if (s_httpd) {
        if (s_stop_timer) {
            esp_timer_stop(s_stop_timer);
            esp_err_t timer_err =
                esp_timer_start_once(s_stop_timer, WG_WEB_AUTO_STOP_US);
            if (timer_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "restart stop timer failed: %s",
                         esp_err_to_name(timer_err));
            }
        }

        ESP_LOGI(TAG, "WG web already running: %s", s_url);
        ui_async_update_status("WG配置Web已重置");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WG_WEB_PORT;
    config.ctrl_port = 32769;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    /*
     * 这里保留 6 KB 栈，同时页面使用分块发送，避免 2 KB/8 KB 大数组压在 httpd 栈上。
     * 如果后续页面继续变复杂，可改为 8192。
     */
    config.stack_size = 6144;

    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_uri_handlers = 6;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        s_url[0] = '\0';
        ui_async_update_status("WG配置Web失败");
        return err;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = wg_get_handler,
    };

    httpd_uri_t wg_get_uri = {
        .uri = "/wg",
        .method = HTTP_GET,
        .handler = wg_get_handler,
    };

    httpd_uri_t wg_post_uri = {
        .uri = "/wg",
        .method = HTTP_POST,
        .handler = wg_post_handler,
    };

    httpd_uri_t state_uri = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
    };

    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_get_handler,
    };

    err = httpd_register_uri_handler(s_httpd, &root_uri);
    if (err != ESP_OK) {
        goto fail;
    }
    err = httpd_register_uri_handler(s_httpd, &wg_get_uri);
    if (err != ESP_OK) {
        goto fail;
    }
    err = httpd_register_uri_handler(s_httpd, &wg_post_uri);
    if (err != ESP_OK) {
        goto fail;
    }
    err = httpd_register_uri_handler(s_httpd, &state_uri);
    if (err != ESP_OK) {
        goto fail;
    }
    err = httpd_register_uri_handler(s_httpd, &favicon_uri);
    if (err != ESP_OK) {
        goto fail;
    }

    if (!s_stop_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = stop_timer_cb,
            .name = "wg_web_stop",
        };

        err = esp_timer_create(&timer_args, &s_stop_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "create stop timer failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (s_stop_timer) {
        esp_timer_stop(s_stop_timer);
        esp_err_t timer_err =
            esp_timer_start_once(s_stop_timer, WG_WEB_AUTO_STOP_US);
        if (timer_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "start stop timer failed: %s",
                     esp_err_to_name(timer_err));
        }
    }

    ESP_LOGI(TAG, "WG web config started: %s", s_url);
    ui_async_update_status("WG配置Web已开启");

    return ESP_OK;

fail:
    ESP_LOGE(TAG, "register uri failed: %s", esp_err_to_name(err));
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_url[0] = '\0';
    ui_async_update_status("WG配置Web失败");
    return err;
}

esp_err_t wg_web_config_stop(void)
{
    if (s_stop_timer) {
        esp_timer_stop(s_stop_timer);
    }

    if (!s_httpd) {
        s_url[0] = '\0';
        return ESP_OK;
    }

    httpd_handle_t httpd = s_httpd;
    s_httpd = NULL;

    esp_err_t err = httpd_stop(httpd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_stop failed: %s", esp_err_to_name(err));
    }

    s_url[0] = '\0';

    ESP_LOGI(TAG, "WG web config stopped");
    ui_async_update_status("WG配置Web已关闭");

    return err;
}

bool wg_web_config_is_running(void)
{
    return s_httpd != NULL;
}

const char *wg_web_config_get_url(void)
{
    return s_url;
}
