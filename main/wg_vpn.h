#ifndef WG_VPN_H
#define WG_VPN_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 连上、SNTP 校时后调用 */
esp_err_t wg_vpn_start(void);
esp_err_t wg_vpn_stop(void);

/* 本机 WireGuard 客户端是否已经启动。 */
bool wg_vpn_is_started(void);

/* WireGuard peer 是否已经完成握手并建立可用会话。 */
bool wg_vpn_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
