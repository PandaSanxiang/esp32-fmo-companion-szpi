/**
 * @file st7789.c
 *
 * Mostly taken from lbthomsen/esp-idf-littlevgl github.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "esp_log.h"

#include "st7789.h"

#include "disp_spi.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "st7789"
/**********************
 *      TYPEDEFS
 **********************/

/*The LCD needs a bunch of command/argument values to be initialized. They are stored in this struct. */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} lcd_init_cmd_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void st7789_set_orientation(uint8_t orientation);

static void st7789_send_color(void *data, size_t length);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void st7789_init(void)
{
    lcd_init_cmd_t st7789_init_cmds[] = {
        /* ===== 官方 esp_lcd 极简初始化序列（与立创例程/出厂固件一致） ===== */
        {ST7789_SLPOUT, {0}, 0x80},
        {ST7789_MADCTL, {0x00}, 1},
        {ST7789_COLMOD, {0x55}, 1},
        {0xB0, {0x00, 0xF0}, 2},   /* RAMCTRL: big endian（官方值） */
        {ST7789_INVON, {0}, 0},    /* 官方例程 invert_color(true) */
        {ST7789_DISPON, {0}, 0x80},
        {0, {0}, 0xff},
    };

    //Initialize non-SPI GPIOs
    esp_rom_gpio_pad_select_gpio(ST7789_DC);
    gpio_set_direction(ST7789_DC, GPIO_MODE_OUTPUT);

#if !defined(ST7789_SOFT_RST)
    gpio_pad_select_gpio(ST7789_RST);
    gpio_set_direction(ST7789_RST, GPIO_MODE_OUTPUT);
#endif

    //Reset the display
#if !defined(ST7789_SOFT_RST)
    gpio_set_level(ST7789_RST, 0);
    vTaskDelay(100 / portTICK_RATE_MS);
    gpio_set_level(ST7789_RST, 1);
    vTaskDelay(100 / portTICK_RATE_MS);
#else
    st7789_send_cmd(ST7789_SWRESET);
    /* ST7789 数据手册：SWRESET 后需等待约 120ms 才能发送后续命令 */
    vTaskDelay(150 / portTICK_PERIOD_MS);
#endif

    printf("ST7789 initialization.\n");

    //Send all the commands
    uint16_t cmd = 0;
    while (st7789_init_cmds[cmd].databytes!=0xff) {
        st7789_send_cmd(st7789_init_cmds[cmd].cmd);
        st7789_send_data(st7789_init_cmds[cmd].data, st7789_init_cmds[cmd].databytes&0x1F);
        if (st7789_init_cmds[cmd].databytes & 0x80) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        cmd++;
    }

    st7789_set_orientation(CONFIG_LV_DISPLAY_ORIENTATION);
}

/* 延迟诊断：全屏（320x240 全 GRAM）填红，用于验证显示链路（由 main 在 UI 创建后调用） */
void st7789_diag_red_fill(void)
{
    uint8_t diag_caset[4] = {0x00, 0x00, 0x01, 0x3F}; /* 列 0..319 */
    uint8_t diag_raset[4] = {0x00, 0x00, 0x00, 0xEF}; /* 行 0..239 */
    st7789_send_cmd(0x2A);
    st7789_send_data(diag_caset, 4);
    st7789_send_cmd(0x2B);
    st7789_send_data(diag_raset, 4);
    st7789_send_cmd(0x2C); /* RAMWR */
    static uint8_t diag_buf[10240];
    for (int j = 0; j < 5120; j++) {
        diag_buf[j * 2] = 0xF8;     /* RGB565 红色高字节 */
        diag_buf[j * 2 + 1] = 0x00; /* 低字节 */
    }
    for (int sent = 0; sent < 320 * 240 * 2; sent += sizeof(diag_buf)) {
        int n = (320 * 240 * 2 - sent) > (int)sizeof(diag_buf) ? (int)sizeof(diag_buf) : (320 * 240 * 2 - sent);
        st7789_send_data(diag_buf, n);
    }
    ESP_LOGI(TAG, "DIAG: delayed red fill 320x240 done (%d bytes)", 320 * 240 * 2);
}

/* The ST7789 display controller can drive up to 320*240 displays, when using a 240*240 or 240*135
 * displays there's a gap of 80px or 40/52/53px respectively. 52px or 53x offset depends on display orientation.
 * We need to edit the coordinates to take into account those gaps, this is not necessary in all orientations. */
void st7789_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_map)
{
    uint8_t data[4] = {0};

    uint16_t offsetx1 = area->x1;
    uint16_t offsetx2 = area->x2;
    uint16_t offsety1 = area->y1;
    uint16_t offsety2 = area->y2;

#if (CONFIG_LV_TFT_DISPLAY_OFFSETS)
    offsetx1 += CONFIG_LV_TFT_DISPLAY_X_OFFSET;
    offsetx2 += CONFIG_LV_TFT_DISPLAY_X_OFFSET;
    offsety1 += CONFIG_LV_TFT_DISPLAY_Y_OFFSET;
    offsety2 += CONFIG_LV_TFT_DISPLAY_Y_OFFSET;

#elif (LV_HOR_RES_MAX == 240) && (LV_VER_RES_MAX == 240)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT)
        offsetx1 += 80;
        offsetx2 += 80;
    #elif (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsety1 += 80;
        offsety2 += 80;
    #endif
#elif (LV_HOR_RES_MAX == 240) && (LV_VER_RES_MAX == 135)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT) || \
        (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT_INVERTED)
        offsetx1 += 40;
        offsetx2 += 40;
        offsety1 += 53;
        offsety2 += 53;
    #endif
#elif (LV_HOR_RES_MAX == 135) && (LV_VER_RES_MAX == 240)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE) || \
        (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsetx1 += 52;
        offsetx2 += 52;
        offsety1 += 40;
        offsety2 += 40;
    #endif
#endif

    /*Column addresses*/
    st7789_send_cmd(ST7789_CASET);
    data[0] = (offsetx1 >> 8) & 0xFF;
    data[1] = offsetx1 & 0xFF;
    data[2] = (offsetx2 >> 8) & 0xFF;
    data[3] = offsetx2 & 0xFF;
    st7789_send_data(data, 4);

    /*Page addresses*/
    st7789_send_cmd(ST7789_RASET);
    data[0] = (offsety1 >> 8) & 0xFF;
    data[1] = offsety1 & 0xFF;
    data[2] = (offsety2 >> 8) & 0xFF;
    data[3] = offsety2 & 0xFF;
    st7789_send_data(data, 4);

    /*Memory write*/
    st7789_send_cmd(ST7789_RAMWR);

    size_t size = (size_t)lv_area_get_width(area) * (size_t)lv_area_get_height(area);

    st7789_send_color((void*)color_map, size * 2);

}

/**********************
 *   STATIC FUNCTIONS
 **********************/
void st7789_send_cmd(uint8_t cmd)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 0);
    disp_spi_send_data(&cmd, 1);
}

void st7789_send_data(void * data, uint16_t length)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 1);
    disp_spi_send_data(data, length);
}

static void st7789_send_color(void * data, size_t length)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 1);
    disp_spi_send_colors(data, length);
}

static void st7789_set_orientation(uint8_t orientation)
{
    // ESP_ASSERT(orientation < 4);

    const char *orientation_str[] = {
        "PORTRAIT", "PORTRAIT_INVERTED", "LANDSCAPE", "LANDSCAPE_INVERTED"
    };

    ESP_LOGI(TAG, "Display orientation: %s", orientation_str[orientation]);

    uint8_t data[] =
    {
#if CONFIG_LV_PREDEFINED_DISPLAY_TTGO
	0x60, 0xA0, 0x00, 0xC0
#else
	0x00, 0xC0, 0x60, 0xA0
#endif
    };

    ESP_LOGI(TAG, "0x36 command value: 0x%02X", data[orientation]);

    st7789_send_cmd(ST7789_MADCTL);
    st7789_send_data((void *) &data[orientation], 1);
}
