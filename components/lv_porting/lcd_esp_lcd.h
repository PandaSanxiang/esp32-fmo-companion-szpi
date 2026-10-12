/**
 * @file lcd_esp_lcd.h
 * @brief 使用 ESP-IDF 官方 esp_lcd 驱动初始化 ST7789（与立创官方例程/出厂固件相同路径）
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SPI 总线 + esp_lcd panel_io + ST7789 面板。
 *
 * 调用前必须已通过 PCA9557 拉低 LCD CS（main 中 board_lcd_cs_init）。
 * 配置与立创官方例程一致：SPI3_HOST、80MHz、spi_mode=2、DC=39、CS=NC、
 * 16bpp、RGB、invert、swap_xy、mirror、disp_on。
 */
esp_err_t lcd_esp_lcd_init(void);

/**
 * @brief LVGL flush 回调，经 esp_lcd_panel_draw_bitmap 输出。
 */
void lcd_esp_lcd_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);

/**
 * @brief DIAG: 全屏填红，验证 esp_lcd 数据路径与面板。
 */
void lcd_esp_lcd_diag_fill_red(void);

#ifdef __cplusplus
}
#endif
