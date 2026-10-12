/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 *
 * 立创实战派ESP32-S3 FMO固件于2026年10月12日由BI9CPS调整修改。
 */

/**
 * @file main.c
 * @brief 应用启动入口。
 *
 * 启动流程：
 * 1. 初始化 NVS；
 * 2. 加载应用配置；
 * 3. 初始化省电管理；
 * 4. 初始化 LVGL；
 * 5. 初始化显示与触摸；
 * 6. 创建 LVGL tick 定时器；
 * 7. 创建 UI；
 * 8. 初始化音频输出抽象层；
 * 9. 启动 WiFi；
 * 10. 启动 RSSI 监控任务；
 * 11. WiFi 连接后启动 WebSocket；
 * 12. 启动电池监测；
 * 13. 进入 LVGL 主循环。
 */

#include <stdio.h>

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

/* LVGL headers ------------------------------------------------------------- */
#include "lvgl.h"

/* Board / display port headers -------------------------------------------- */
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "lvgl_i2c/i2c_manager.h"

/* Project headers ---------------------------------------------------------- */
#include "app_power_save.h"
#include "app_settings.h"
#include "app_ui.h"
#include "audio_output.h"
#include "audio_ws.h"
#include "battery_monitor.h"
#include "board_config.h"
#include "ui_async.h"
#include "wifi_manager.h"
#include "wg_vpn.h"
#include "qth_lookup.h"
#include "gpio_ctrl.h"

/*
 * 如果当前 main.c 不直接调用 st7789.h 中的接口，可以删除该 include。
 * 目前显示初始化由 lv_port_disp_init() 完成。
 */
/* #include "st7789.h" */

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "main";

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static void lv_tick_task(void *arg);
static void app_nvs_init(void);
static void wifi_rssi_task(void *arg);
static void mem_monitor_task(void *arg);
static void audio_ws_start_task(void *arg);
static esp_err_t board_i2c_bus_init(void);
static esp_err_t board_lcd_cs_init(void);
static void user_btn_task(void *arg);
static esp_err_t user_btn_init(void);

/* ===== 用户按键静音（GPIO0 BOOT 键，按下为低电平） ===== */
#define USER_BTN_GPIO        GPIO_NUM_0
#define USER_BTN_DEBOUNCE_MS 30

extern void local_audio_mute(bool mute); /* audio_ws.c */

static volatile bool s_user_btn_pending = false;
static bool s_user_btn_muted = false;

static void IRAM_ATTR user_btn_isr(void *arg)
{
    s_user_btn_pending = true;
}

static void user_btn_task(void *arg)
{
    while (1) {
        if (s_user_btn_pending) {
            vTaskDelay(pdMS_TO_TICKS(USER_BTN_DEBOUNCE_MS)); /* 软件防抖 */
            s_user_btn_pending = false;
            if (gpio_get_level(USER_BTN_GPIO) == 0) { /* 确认仍为按下 */
                s_user_btn_muted = !s_user_btn_muted;
                local_audio_mute(s_user_btn_muted);
                ESP_LOGI(TAG, "user button: audio %s",
                         s_user_btn_muted ? "MUTED" : "UNMUTED");
                ui_async_update_status(s_user_btn_muted ? "已静音" : "已恢复音量");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
    return;
}

static esp_err_t user_btn_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << USER_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = gpio_isr_handler_add(USER_BTN_GPIO, user_btn_isr, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t t = xTaskCreate(user_btn_task, "btn_mute", 2048, NULL, 4, NULL);
    if (t != pdPASS) {
        ESP_LOGE(TAG, "create user_btn_task failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "user button (GPIO0) mute feature enabled");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* LVGL tick                                                                  */
/* -------------------------------------------------------------------------- */

static void lv_tick_task(void *arg)
{
    LV_UNUSED(arg);

    lv_tick_inc(1);
}

/* -------------------------------------------------------------------------- */
/* Board I2C bus + PCA9557 IO expander (LCD CS control)                      */
/* -------------------------------------------------------------------------- */

static esp_err_t board_i2c_bus_init(void)
{
    /*
     * 立创实战派 ESP32-S3 I2C 总线初始化。
     * SDA=GPIO1, SCL=GPIO2, 100kHz。
     * 该总线共享：PCA9557 IO 扩展器、FT6336 触摸、ES8311 编解码器。
     */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(BOARD_I2C_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(BOARD_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C bus initialized, sda=%d, scl=%d, freq=%d",
             BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_I2C_FREQ_HZ);

    return ESP_OK;
}

static esp_err_t board_pca9557_write_output(uint8_t bit, bool value)
{
    /*
     * PCA9557 I2C IO 扩展器写输出。
     * 寄存器：
     *   0x01 = Output Port Register
     *   0x03 = Configuration Register (0=output, 1=input)
     *
     * 先读当前输出值，修改指定 bit，再写回。
     */
    uint8_t reg = 0x01;
    uint8_t current = 0;

    /* 读取当前输出寄存器值 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IO_EXPANDER_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IO_EXPANDER_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &current, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BOARD_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 read output reg failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 修改指定 bit */
    if (value) {
        current |= (1 << bit);
    } else {
        current &= ~(1 << bit);
    }

    /* 写回输出寄存器 */
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IO_EXPANDER_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, current, true);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(BOARD_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 write output reg failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t board_lcd_cs_init(void)
{
    /*
     * 立创实战派的 LCD CS 由 PCA9557 I2C IO 扩展器控制。
     * 需要先配置 PCA9557 对应位为输出，然后拉低 CS（ST7789 CS 低有效）。
     */
    esp_err_t err;

    /* 配置 PCA9557 P0 为输出 */
    uint8_t conf_reg = 0x03;
    uint8_t conf_val = 0; /* 全部为输出 */

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IO_EXPANDER_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, conf_reg, true);
    i2c_master_write_byte(cmd, conf_val, true);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(BOARD_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 config reg write failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 先设 LCD CS 为非选中（高电平），与官方例程 pca9557_init 的 LCD_CS=1 一致。
     * 低电平（选中）由 esp_lcd 驱动在 panel_reset 之后、panel_init 之前拉低，
     * 避免 SWRESET 在选中状态下被屏幕执行导致复位时序异常。 */
    err = board_pca9557_write_output(BOARD_IO_EXP_LCD_CS_BIT, BOARD_LCD_CS_ACTIVE == 0 ? true : false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 set LCD CS failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "LCD CS set to inactive (high) via PCA9557 (bit %d)", BOARD_IO_EXP_LCD_CS_BIT);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* NVS initialization                                                         */
/* -------------------------------------------------------------------------- */

static void app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG,
                 "NVS needs erase, err=%s",
                 esp_err_to_name(err));

        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "NVS initialized");
}

/* -------------------------------------------------------------------------- */
/* Background tasks                                                           */
/* -------------------------------------------------------------------------- */

static void wifi_rssi_task(void *arg)
{
    LV_UNUSED(arg);

    while (true) {
        if (wifi_manager_is_connected()) {
            int rssi = wifi_manager_get_rssi();

            ui_async_update_wifi_rssi(rssi);
        } else {
            ui_async_update_wifi_rssi(-127);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}


#define MEM_MONITOR_INTERVAL_MS       10000
#define MEM_WARN_INTERNAL_FREE_BYTES  30000
#define MEM_WARN_LARGEST_BLOCK_BYTES  12000

static void mem_monitor_task(void *arg)
{
    LV_UNUSED(arg);

    while (true) {
        size_t free_all = esp_get_free_heap_size();
        size_t min_free_all = esp_get_minimum_free_heap_size();

        size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_internal =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

        size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

#ifdef MALLOC_CAP_SPIRAM
        size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largest_spiram =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#else
        size_t free_spiram = 0;
        size_t largest_spiram = 0;
#endif

        UBaseType_t stack_left_words = uxTaskGetStackHighWaterMark(NULL);

        if (free_internal < MEM_WARN_INTERNAL_FREE_BYTES ||
            largest_internal < MEM_WARN_LARGEST_BLOCK_BYTES) {
            ESP_LOGW(TAG,
                     "MEM LOW: free=%u min=%u | 8bit=%u largest=%u | "
                     "internal=%u largest=%u | dma=%u largest=%u | "
                     "spiram=%u largest=%u | mem_task_stack=%u words",
                     (unsigned)free_all,
                     (unsigned)min_free_all,
                     (unsigned)free_8bit,
                     (unsigned)largest_8bit,
                     (unsigned)free_internal,
                     (unsigned)largest_internal,
                     (unsigned)free_dma,
                     (unsigned)largest_dma,
                     (unsigned)free_spiram,
                     (unsigned)largest_spiram,
                     (unsigned)stack_left_words);
        } else {
            ESP_LOGI(TAG,
                     "MEM: free=%u min=%u | 8bit=%u largest=%u | "
                     "internal=%u largest=%u | dma=%u largest=%u | "
                     "spiram=%u largest=%u | mem_task_stack=%u words",
                     (unsigned)free_all,
                     (unsigned)min_free_all,
                     (unsigned)free_8bit,
                     (unsigned)largest_8bit,
                     (unsigned)free_internal,
                     (unsigned)largest_internal,
                     (unsigned)free_dma,
                     (unsigned)largest_dma,
                     (unsigned)free_spiram,
                     (unsigned)largest_spiram,
                     (unsigned)stack_left_words);
        }

        vTaskDelay(pdMS_TO_TICKS(MEM_MONITOR_INTERVAL_MS));
    }
}


static void audio_ws_start_task(void *arg)
{
    LV_UNUSED(arg);

    ui_async_update_status("等待WiFi连接");

    while (!wifi_manager_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ui_async_update_status("连接FMO");

    esp_err_t err = audio_ws_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "audio_ws_start failed: %s",
                 esp_err_to_name(err));
        ui_async_update_status("FMO连接失败");
    }

    /*
     * audio_ws_start() 内部会启动 SNTP。
     * WireGuard 需要等待系统时间同步后再启动。
     */
    err = wg_vpn_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "wg_vpn_start failed: %s",
                 esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Application entry                                                          */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    esp_log_level_set("XPT2046", ESP_LOG_WARN);

    ESP_LOGI(TAG, "app start");

    /*
     * 1. 初始化 NVS。
     * 后续 app_settings、WiFi、Web 配置都会依赖 NVS。
     */
    app_nvs_init();

    /*
     * 2. 初始化应用配置。
     * 如果 NVS 中没有配置或版本不匹配，会自动写入默认配置。
     */
    ESP_ERROR_CHECK(app_settings_init());

    const app_settings_t *cfg = app_settings_get();

    if (cfg) {
        ESP_LOGI(TAG,
                 "config: backlight=%u, volume=%u, rotate180=%d",
                 cfg->backlight_percent,
                 cfg->audio_volume,
                 cfg->screen_rotate_180 ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "app_settings_get returned NULL");
    }

    /*
     * 3. 初始化省电管理。
     */
    app_power_save_init();

    /*
     * 4. 初始化 LVGL。
     */
    lv_init();

    /*
     * 5. 应用屏幕旋转配置。
     * 需要在 lv_port_disp_init() 前设置。
     */
    lv_port_disp_set_rotate_180(cfg && cfg->screen_rotate_180);

    /*
     * 5.5 初始化 I2C 总线和 PCA9557 IO 扩展器。
     *
     * 立创实战派的 LCD CS 由 PCA9557 I2C 扩展器控制，
     * 必须在 LVGL 显示初始化（SPI 传输）之前拉低 CS。
     */
    board_i2c_bus_init();
    board_lcd_cs_init();

    /*
     * 6. 初始化显示。
     */
    lv_port_disp_init();

    /*
     * 7. 初始化触摸。
     */
    lv_port_indev_init();

    /*
     * 8. 创建 LVGL tick 定时器。
     * 周期 1000us，即每 1ms 调用一次 lv_tick_inc(1)。
     */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "lv_tick",
    };

    esp_timer_handle_t periodic_timer = NULL;

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args,
                                     &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));

    /*
     * 9. 创建 UI。
     */
    app_ui_create();

    /* 有源蜂鸣器自动不播放 */
    if (cfg) {
        gpio_ctrl_buzzer_play_effect(cfg->startup_sound_effect);
    }

    /*
     * 10. 使用 NVS 配置刷新背光。
     * app_ui_create() 内部已经会应用配置，这里再次刷新作为兜底。
     */
    if (cfg) {
        ui_async_set_backlight_percent(cfg->backlight_percent);
    }

    ui_async_update_status("配置加载完成");

    /* ===== DIAG: 强制立即刷新一次，验证 LVGL 渲染与 flush 路径 ===== */
    {
        lv_disp_t *d = lv_disp_get_default();
        lv_obj_invalidate(lv_scr_act());
        ESP_LOGI(TAG, "diag: default disp=%p scr=%p", (void *)d, (void *)lv_scr_act());
        if (d) {
            lv_refr_now(d);
        }
    }

    ESP_LOGI(TAG, "ui created");

    /* 背光按 NVS 配置（已由 ui_async_set_backlight_percent 兜底设置） */

    /*
     * 11. 初始化音频输出抽象层。
     *
     * 需要在 audio_ws 播放任务真正写入音频前完成。
     */
    ESP_ERROR_CHECK(audio_output_init());

    /*
     * 12. 启动 WiFi。
     */
    ESP_ERROR_CHECK(wifi_manager_start());

    /*
     * 13. 启动 WiFi RSSI 周期更新任务。
     */
    BaseType_t task_ret = xTaskCreate(wifi_rssi_task,
                                      "wifi_rssi",
                                      3072,
                                      NULL,
                                      1,
                                      NULL);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "create wifi_rssi_task failed");
    }

    /*
     * 14. 启动内存监控任务。
     *
     * 低优先级周期打印 heap / largest block，便于定位低内存和碎片问题。
     */
    task_ret = xTaskCreate(mem_monitor_task,
                           "mem_monitor",
                           3072,
                           NULL,
                           1,
                           NULL);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "create mem_monitor_task failed");
    }

    /*
     * 15. 创建 WebSocket 启动任务。
     *
     * 该任务会等待 WiFi 连接成功后调用 audio_ws_start()。
     */
    task_ret = xTaskCreate(audio_ws_start_task,
                           "audio_ws_start",
                           4096,
                           NULL,
                           5,
                           NULL);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "create audio_ws_start_task failed");
    }

    /*
     * 16. 启动电池监测。
     */
    ESP_ERROR_CHECK(battery_monitor_start());

    /*
     * 16a. 初始化用户按键静音功能（GPIO0 BOOT 键）。
     */
    esp_err_t btn_ret = user_btn_init();
    if (btn_ret != ESP_OK) {
        ESP_LOGW(TAG, "user button mute init failed: %s", esp_err_to_name(btn_ret));
    }

    /*
     * 17. LVGL 主循环。
     *
     * 当前项目中 LVGL 对象操作必须在该主循环上下文或
     * lv_async_call() 回调上下文中执行。
     */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

#if LVGL_VERSION_MAJOR >= 8
        lv_timer_handler();
#else
        lv_task_handler();
#endif
    }
}
