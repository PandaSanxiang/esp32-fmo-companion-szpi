#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化 QTH 查询模块。
 *
 * 会从 NVS 加载持久化 QTH 缓存。
 *
 * 注意：
 * - 可以在 app_main() 初始化 NVS 和 settings 后主动调用。
 * - 如果不主动调用，qth_lookup_request_grid() 第一次调用时也会自动初始化。
 */
void qth_lookup_init(void);

/*
 * 根据 Maidenhead 网格坐标查询 QTH 地址。
 *
 * 行为：
 * - 如果 QTH 查询关闭：直接返回
 * - 如果缓存命中：立即显示缓存地址
 * - 如果缓存未命中：显示 Grid xxxx，并在资源允许时后台查询
 */
void qth_lookup_request_grid(const char *grid);

#ifdef __cplusplus
}
#endif
