/**
 * @file lcd_esp_lcd.c
 * @brief 使用 ESP-IDF 官方 esp_lcd 驱动初始化 ST7789（与立创官方例程/出厂固件相同路径）
 *
 * 背景：lvgl_esp32_drivers 的自定义 SPI 驱动（disp_spi）在这块板上
 * 初始化命令可被屏幕部分接受（白屏->黑屏）但像素数据不显示；
 * 立创出厂固件使用 esp_lcd_panel_io_spi + esp_lcd_panel_st7789 可正常点亮，
 * 故改为官方 esp_lcd 路径。
 */
#include "lcd_esp_lcd.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"

#define TAG "esp_lcd"

/* 立创实战派引脚定义（与官方例程一致，避免依赖 main 组件头文件） */
#define LCD_SCLK_GPIO   GPIO_NUM_41
#define LCD_MOSI_GPIO   GPIO_NUM_40
#define LCD_DC_GPIO     GPIO_NUM_39

/* PCA9557 IO 扩展器（控制 LCD CS），避免依赖 main 组件头文件 */
#define LCD_I2C_NUM          (0)
#define LCD_PCA9557_ADDR     (0x19)
#define LCD_PCA9557_OUT_REG  (0x01)   /* 输出寄存器 */
#define LCD_CS_BIT           (0)      /* P0 控制 LCD CS，低有效 */

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;

/* 通过 PCA9557 设置 LCD CS 电平（level=1 高/非选中，0 低/选中） */
static esp_err_t lcd_cs_set(uint8_t level)
{
    /* 读-改-写输出寄存器 */
    uint8_t current = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_PCA9557_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, LCD_PCA9557_OUT_REG, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_PCA9557_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &current, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(LCD_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 read out reg failed: %s", esp_err_to_name(err));
        return err;
    }

    if (level) {
        current |= (1 << LCD_CS_BIT);
    } else {
        current &= (uint8_t)~(1 << LCD_CS_BIT);
    }

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_PCA9557_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, LCD_PCA9557_OUT_REG, true);
    i2c_master_write_byte(cmd, current, true);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(LCD_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 write out reg failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t lcd_esp_lcd_init(void)
{
    /* 1. SPI 总线（与官方例程一致） */
    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCLK_GPIO,        /* IO41 */
        .mosi_io_num = LCD_MOSI_GPIO,        /* IO40 */
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 320 * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI init failed");

    /* 2. panel IO（官方例程同参：80MHz、mode 2、DC=39、CS=NC 由 PCA9557 控制） */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC_GPIO,          /* IO39 */
        .cs_gpio_num = GPIO_NUM_NC,          /* PCA9557 外部控制，常低 */
        .pclk_hz = 80 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                                 &io_config, &s_io),
                        TAG, "New panel IO failed");

    /* 3. ST7789 面板（官方例程同参：RST=NC 软复位、RGB、16bpp） */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel),
                        TAG, "New panel failed");

    /* 4. 初始化顺序与官方例程一致：
     *    reset（CS 高/非选中，SWRESET 不生效）→ 拉低 CS → init → invert → swap → mirror → disp_on */
    lcd_cs_set(1);                        /* 保险：CS 高（main 已设，此处再确认） */
    esp_lcd_panel_reset(s_panel);         /* SWRESET，CS 高时屏幕不接收，与官方一致 */
    lcd_cs_set(0);                        /* 拉低 CS，屏幕开始接收命令 */
    esp_lcd_panel_init(s_panel);          /* SLPOUT/MADCTL/COLMOD/RAMCTRL */
    esp_lcd_panel_invert_color(s_panel, true); /* INVON */
    /* ST7789 RAM 为 240x320，模组 320x240 显示依赖 MADCTL 0x60（MV+MX），
     * 与官方例程一致：swap_xy + mirror(true,false) */
    esp_lcd_panel_swap_xy(s_panel, true);     /* MADCTL MV */
    esp_lcd_panel_mirror(s_panel, true, false); /* MADCTL MX */
    esp_lcd_panel_disp_on_off(s_panel, true);  /* DISPON */

    ESP_LOGI(TAG, "esp_lcd ST7789 init done (320x240, SPI3, 80MHz, mode2)");
    return ESP_OK;
}

void lcd_esp_lcd_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    static bool s_first = true;
    if (s_first) {
        ESP_LOGI(TAG, "flush #1: area=(%d,%d)-(%d,%d) %dx%d",
                 area->x1, area->y1, area->x2, area->y2,
                 area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
        s_first = false;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                              area->x2 + 1, area->y2 + 1,
                                              (void *)color_p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed: %s (area %d,%d-%d,%d)",
                 esp_err_to_name(err), area->x1, area->y1, area->x2, area->y2);
    }
    lv_disp_flush_ready(drv);
}

/* ===== DIAG: 四色象限标记图案，精确定位旋转/镜像方向 =====
 * 期望（按 LVGL 视口 320x240）：左上=红, 右上=绿, 左下=蓝, 右下=黄
 * 用户看到实际颜色位置后可反推坐标变换。 */
void lcd_esp_lcd_diag_fill_red(void)
{
    static uint16_t s_quad[320 * 40];
    for (int y0 = 0; y0 < 240; y0 += 40) {
        for (int y = 0; y < 40; y++) {
            for (int x = 0; x < 320; x++) {
                uint16_t c;
                if (x < 160 && (y0 + y) < 120)       c = 0xF800; /* 红 左上 */
                else if (x >= 160 && (y0 + y) < 120) c = 0x07E0; /* 绿 右上 */
                else if (x < 160 && (y0 + y) >= 120) c = 0x001F; /* 蓝 左下 */
                else                                  c = 0xFFE0; /* 黄 右下 */
                s_quad[y * 320 + x] = c;
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, 320, y0 + 40, (void *)s_quad);
    }
    ESP_LOGI(TAG, "diag quadrants 320x240 done");
}
