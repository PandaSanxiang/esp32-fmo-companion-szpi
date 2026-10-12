#ifndef WG_WEB_CONFIG_H
#define WG_WEB_CONFIG_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动临时 WireGuard Web 配置服务。
 * 成功后访问：http://<当前WiFi IP>/wg
 * 默认 5 分钟后自动关闭。
 */
esp_err_t wg_web_config_start(void);
esp_err_t wg_web_config_stop(void);
bool wg_web_config_is_running(void);

/* 获取当前配置页 URL。未运行时返回空字符串。 */
const char *wg_web_config_get_url(void);

#ifdef __cplusplus
}
#endif

#endif /* WG_WEB_CONFIG_H */
