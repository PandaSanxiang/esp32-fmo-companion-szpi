/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file board_config.h
 * @brief 当前硬件板级引脚与外设参数配置。
 *
 * 本文件用于描述当前硬件板子的 GPIO 分配、屏幕分辨率、
 * 触摸校准参数、音频控制引脚、SD 卡 SDMMC 引脚和电池 ADC 引脚。
 *
 * @note
 * 本文件只放“硬件板级配置”。
 * 应用默认值、功能开关、WebSocket、音频缓冲、省电阈值等配置
 * 应放在 app_config.h。
 *
 * @warning
 * 修改本文件前请确认实际硬件原理图。
 * GPIO 冲突会导致 LCD、触摸、SD、音频或电池检测异常。
 *
 * 立创实战派 ESP32-S3 开发板 (ESP32-S3-WROOM-1-N16R8)
 * - 16MB Flash, 8MB Octal PSRAM
 * - ST7789 LCD (240x240), SPI via SPI3_HOST
 * - FT6336 触摸 (I2C)
 * - ES8311 音频编解码器 + NS4150 功放 (I2S + I2C)
 * - PCA9557 I2C IO 扩展器 (控制 LCD CS 等)
 * - SD 卡 (1-bit SDMMC 模式)
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* I2C 总线 (共享: 触摸 FT6336 + ES8311 + PCA9557 IO 扩展器)                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief I2C 总线编号。
 */
#define BOARD_I2C_NUM               (0)

/**
 * @brief I2C SDA 引脚。
 */
#define BOARD_I2C_SDA_GPIO          GPIO_NUM_1

/**
 * @brief I2C SCL 引脚。
 */
#define BOARD_I2C_SCL_GPIO          GPIO_NUM_2

/**
 * @brief I2C 总线频率 (Hz)。
 */
#define BOARD_I2C_FREQ_HZ           (100000)

/* -------------------------------------------------------------------------- */
/* PCA9557 I2C IO 扩展器 (控制 LCD CS 等)                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief PCA9557 I2C 地址 (7-bit)。
 *
 * 立创实战派上的 IO 扩展器地址为 0x19。
 */
#define BOARD_IO_EXPANDER_ADDR      (0x19)

/**
 * @brief PCA9557 LCD CS 输出位 (在扩展器端口上的位索引)。
 *
 * P0 用于 LCD CS。
 */
#define BOARD_IO_EXP_LCD_CS_BIT     (0)

/**
 * @brief LCD CS 有效电平 (0 = 低电平有效)。
 *
 * ST7789 CS 低电平有效，因此 LCD CS 需要在 SPI 传输期间拉低。
 */
#define BOARD_LCD_CS_ACTIVE         (0)

/* -------------------------------------------------------------------------- */
/* LCD: ST7789                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief LCD 水平分辨率。
 */
#define BOARD_LCD_H_RES             240

/**
 * @brief LCD 垂直分辨率。
 */
#define BOARD_LCD_V_RES             240

/**
 * @brief LCD SPI CS 引脚。
 *
 * @note
 * 立创实战派的 LCD CS 由 PCA9557 I2C IO 扩展器控制，而非 ESP32 GPIO。
 * 这里设为 GPIO_NUM_NC 表示 ESP32 端不直接驱动 CS，改由 I2C 扩展器。
 */
#define BOARD_LCD_CS_GPIO           GPIO_NUM_NC

/**
 * @brief LCD 数据/命令选择引脚。
 */
#define BOARD_LCD_DC_GPIO           GPIO_NUM_39

/**
 * @brief LCD SPI MOSI 引脚。
 */
#define BOARD_LCD_MOSI_GPIO         GPIO_NUM_40

/**
 * @brief LCD SPI MISO 引脚。
 *
 * 对于只写 LCD，该引脚不使用。
 */
#define BOARD_LCD_MISO_GPIO         GPIO_NUM_NC

/**
 * @brief LCD SPI SCLK 引脚。
 */
#define BOARD_LCD_SCLK_GPIO         GPIO_NUM_41

/**
 * @brief LCD 背光控制引脚。
 *
 * app_ui.c 中会使用 LEDC PWM 控制该引脚。
 */
#define BOARD_LCD_BL_GPIO           GPIO_NUM_42

/**
 * @brief LCD 复位引脚。
 *
 * GPIO_NUM_NC 表示当前硬件未连接独立复位脚。
 */
#define BOARD_LCD_RST_GPIO          GPIO_NUM_NC

/**
 * @brief LCD 背光点亮电平。
 *
 * 立创实战派背光使用 PMOS 管，低电平点亮。
 */
#define BOARD_LCD_BL_ON_LEVEL       0

/**
 * @brief LCD 背光关闭电平。
 */
#define BOARD_LCD_BL_OFF_LEVEL      1

/**
 * @brief LCD SPI 主机编号。
 *
 * 立创实战派 LCD 接在 SPI3_HOST 上。
 */
#define BOARD_LCD_SPI_HOST          (SPI3_HOST)

/**
 * @brief LCD SPI 模式。
 */
#define BOARD_LCD_SPI_MODE          (2)

/**
 * @brief LCD SPI 像素时钟 (Hz)。
 */
#define BOARD_LCD_SPI_PIXEL_CLK_HZ  (80 * 1000 * 1000)

/* -------------------------------------------------------------------------- */
/* Touch: FT6336 (I2C)                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief FT6336 触摸 I2C 地址 (7-bit)。
 */
#define BOARD_TOUCH_I2C_ADDR        (0x38)

/**
 * @brief 触摸中断引脚。
 *
 * 立创实战派未连接触摸中断脚。
 */
#define BOARD_TOUCH_IRQ_GPIO        GPIO_NUM_NC

/**
 * @brief 触摸中断有效电平。
 *
 * 0：低电平有效
 * 1：高电平有效
 */
#define BOARD_TOUCH_IRQ_ACTIVE      0

/**
 * @brief 触摸 X 方向镜像。
 */
#define BOARD_TOUCH_MIRROR_X        1

/**
 * @brief 触摸 Y 方向镜像。
 */
#define BOARD_TOUCH_MIRROR_Y        0

/**
 * @brief 触摸 XY 交换。
 */
#define BOARD_TOUCH_SWAP_XY         1

/* -------------------------------------------------------------------------- */
/* Audio output (ES8311 + NS4150)                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief I2S 编号。
 */
#define BOARD_AUDIO_I2S_NUM         (0)

/**
 * @brief I2S MCLK (主时钟) 引脚。
 */
#define BOARD_AUDIO_I2S_MCK_GPIO    GPIO_NUM_38

/**
 * @brief I2S BCLK (位时钟) 引脚。
 */
#define BOARD_AUDIO_I2S_BCLK_GPIO   GPIO_NUM_12

/**
 * @brief I2S WS (字选择/LRCK) 引脚。
 */
#define BOARD_AUDIO_I2S_WS_GPIO     GPIO_NUM_13

/**
 * @brief I2S DOUT (数据输出) 引脚。
 */
#define BOARD_AUDIO_I2S_DOUT_GPIO   GPIO_NUM_14

/**
 * @brief I2S 采样率 (Hz)。
 */
#define BOARD_AUDIO_SAMPLE_RATE     (16000)

/**
 * @brief ES8311 I2C 地址 (7-bit)。
 */
#define BOARD_AUDIO_ES8311_I2C_ADDR (0x18)

/**
 * @brief 音频功放 (NS4150) 使能/静音控制引脚。
 *
 * @note
 * 立创实战派的功放使能脚可能由 PCA9557 扩展器控制；
 * 如果直接接到 ESP32 GPIO，请填实际引脚号。
 */
#define BOARD_AUDIO_EN_GPIO         GPIO_NUM_NC

/**
 * @brief 音频功放使能有效电平。
 *
 * 0：低电平使能
 * 1：高电平使能
 */
#define BOARD_AUDIO_EN_ACTIVE       0

/**
 * @brief 音频功放静音有效电平。
 *
 * 0：低电平静音
 * 1：高电平静音
 */
#define BOARD_AUDIO_MUTE_ACTIVE     1

/* -------------------------------------------------------------------------- */
/* SD card (1-bit SDMMC 模式)                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief SD 卡 SDMMC 编号。
 *
 * 立创实战派 SD 卡接在 SDMMC_HOST 上，使用 1-bit 模式。
 */
#define BOARD_SD_HOST               (SDMMC_HOST_SLOT_1)

/**
 * @brief SD 卡 CMD 引脚。
 */
#define BOARD_SD_CMD_GPIO           GPIO_NUM_45

/**
 * @brief SD 卡 CLK 引脚。
 */
#define BOARD_SD_CLK_GPIO           GPIO_NUM_47

/**
 * @brief SD 卡 D0 引脚。
 */
#define BOARD_SD_D0_GPIO            GPIO_NUM_46

/* -------------------------------------------------------------------------- */
/* Battery ADC                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 电池电压 ADC 输入引脚。
 *
 * @note
 * ESP32-S3 的 ADC1 通道覆盖 GPIO1~GPIO4 (ADC1_CH0~CH3)，
 * 但 GPIO1/GPIO2 已用于 I2C，因此可选用空闲的 ADC 引脚。
 * GPIO_NUM_NC 表示当前未连接电池检测电路。
 */
#define BOARD_BAT_ADC_GPIO          GPIO_NUM_NC

/**
 * @brief 蜂鸣器控制引脚。
 *
 * @note
 * 立创实战派无独立蜂鸣器，此处保留为 NC。
 */
#define BUZZER_GPIO                 GPIO_NUM_NC

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CONFIG_H */
