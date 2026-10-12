/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file es8311_codec.c
 * @brief ES8311 音频编解码器 I2C 驱动实现（最小化 DAC 路径）。
 *
 * 本实现基于 ES8311 datasheet 和 espressif/esp-bsp 中 ES8311 组件的
 * 公开初始化序列，仅配置 DAC 播放路径。
 *
 * 寄存器地址参考 ES8311 datasheet:
 *  0x01: Reset / Clock manager reset
 *  0x02: Clock select
 *  0x03: Clock divider (SDM)
 *  0x04: Clock divider (ADC)
 *  0x05: Clock divider (DAC)
 *  0x06-0x08: PLL factor
 *  0x09: BCLK divider
 *  0x0A: LRCK divider
 *  0x0B-0x0C: Reserved
 *  0x0D: PLL enable / clock source
 *  0x12: System power
 *  0x14: HP driver / output
 *  0x1A: Analog control
 *  0x1B: Headphone out
 *  0x1C: DAC system / clock
 *  0x1E: DAC mute
 *  0x20: Mic1 bias
 *  0x21: Mic2 bias
 *  0x22: Mic PGA
 *  0x23: DAC volume LSB
 *  0x24: DAC volume MSB
 *  0x25: DAC mixer
 *  0x26: DAC gain
 *  0x2D: GPIO select
 *  0x32: ADC system
 *  0x33: ADC mute
 *  0x37: ADC gain
 *  0x3B: ADC loop
 */

#include "es8311_codec.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"

static const char *TAG = "es8311";

/* ES8311 register addresses */
#define ES8311_CLK_REG_RST          0x01
#define ES8311_CLK_REG_CLK_SEL      0x02
#define ES8311_CLK_REG_CLK_DIV     0x03
#define ES8311_CLK_REG_CLK_ADC     0x04
#define ES8311_CLK_REG_CLK_DAC     0x05
#define ES8311_CLK_REG_PLL_FA      0x06
#define ES8311_CLK_REG_PLL_FB      0x07
#define ES8311_CLK_REG_PLL_FC      0x08
#define ES8311_CLK_REG_BCLK_DIV    0x09
#define ES8311_CLK_REG_LRCK_DIV    0x0A
#define ES8311_CLK_REG_PLL_EN      0x0D

#define ES8311_SYS_REG_PWR         0x12
#define ES8311_SYS_REG_HP          0x14
#define ES8311_SYS_REG_ANA_CTRL   0x1A
#define ES8311_SYS_REG_HP_CTRL     0x1B
#define ES8311_SYS_REG_DAC_CLK    0x1C
#define ES8311_SYS_REG_DAC_MUTE   0x1E

#define ES8311_SYS_REG_MIC1        0x20
#define ES8311_SYS_REG_MIC2        0x21
#define ES8311_SYS_REG_MIC_PGA    0x22
#define ES8311_SYS_REG_DAC_VOL_L   0x23
#define ES8311_SYS_REG_DAC_VOL_R   0x24
#define ES8311_SYS_REG_DAC_MIX    0x25
#define ES8311_SYS_REG_DAC_GAIN   0x26

#define ES8311_GPIO_REG_SEL       0x2D

#define ES8311_ADC_REG_SYS         0x32
#define ES8311_ADC_REG_MUTE         0x33
#define ES8311_ADC_REG_GAIN         0x37
#define ES8311_ADC_REG_LOOP        0x3B

/* I2C 超时 */
#define ES8311_I2C_TIMEOUT_MS      100

/* -------------------------------------------------------------------------- */
/* I2C 读写原语                                                                */
/* -------------------------------------------------------------------------- */

static esp_err_t es8311_write_reg(i2c_port_t port,
                                    uint8_t addr,
                                    uint8_t reg,
                                    uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port,
                                          cmd,
                                          pdMS_TO_TICKS(ES8311_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2c write reg 0x%02x val 0x%02x failed: %s",
                 reg, val, esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t es8311_read_reg(i2c_port_t port,
                                   uint8_t addr,
                                   uint8_t reg,
                                   uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port,
                                          cmd,
                                          pdMS_TO_TICKS(ES8311_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t es8311_codec_init(i2c_port_t i2c_port,
                             uint8_t i2c_addr,
                             uint32_t sample_rate)
{
    esp_err_t ret;

    ESP_LOGI(TAG,
             "init ES8311, addr=0x%02x, sample_rate=%lu",
             i2c_addr,
             (unsigned long)sample_rate);

    /*
     * Step 1: 复位 ES8311。
     */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_RST, 0x80);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_RST, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * Step 2: 时钟管理器配置。
     * 使用外部 MCLK 输入，不使用内部 PLL。
     */
    /* 时钟选择：使用 MCLK 来自外部（I2S 主机） */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_CLK_SEL, 0x00);
    if (ret != ESP_OK) return ret;

    /* 时钟分频：正常工作 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_CLK_DIV, 0x01);
    if (ret != ESP_OK) return ret;

    /* ADC 时钟分频 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_CLK_ADC, 0x10);
    if (ret != ESP_OK) return ret;

    /* DAC 时钟分频 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_CLK_DAC, 0x10);
    if (ret != ESP_OK) return ret;

    /* PLL 配置：禁用内部 PLL，使用外部 MCLK */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_PLL_FA, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_PLL_FB, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_PLL_FC, 0x00);
    if (ret != ESP_OK) return ret;

    /* BCLK 分频器：从模式不需要配置 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_BCLK_DIV, 0x00);
    if (ret != ESP_OK) return ret;

    /* LRCK 分频器：从模式不需要配置 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_LRCK_DIV, 0x00);
    if (ret != ESP_OK) return ret;

    /* PLL 禁用 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_CLK_REG_PLL_EN, 0x00);
    if (ret != ESP_OK) return ret;

    /*
     * Step 3: 系统电源管理。
     */
    /* 启动所有电源 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_PWR, 0x00);
    if (ret != ESP_OK) return ret;

    /* HP 驱动器使能 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_HP, 0x1F);
    if (ret != ESP_OK) return ret;

    /* 模拟控制 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_ANA_CTRL, 0x00);
    if (ret != ESP_OK) return ret;

    /* 耳机输出控制 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_HP_CTRL, 0x00);
    if (ret != ESP_OK) return ret;

    /*
     * Step 4: DAC 路径配置。
     */
    /* DAC 时钟配置 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_CLK, 0x0C);
    if (ret != ESP_OK) return ret;

    /* DAC 初始不静音 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_MUTE, 0x00);
    if (ret != ESP_OK) return ret;

    /*
     * Step 5: Mic/ADC 路径配置（本板暂不使用，关闭以省电）。
     */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_MIC1, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_MIC2, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_MIC_PGA, 0x00);
    if (ret != ESP_OK) return ret;

    /*
     * Step 6: DAC 音量配置。
     * 0x00 = 0dB（最大音量）。
     */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_VOL_L, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_VOL_R, 0x00);
    if (ret != ESP_OK) return ret;

    /* DAC 混音器 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_MIX, 0x00);
    if (ret != ESP_OK) return ret;

    /* DAC 增益 */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_GAIN, 0x1F);
    if (ret != ESP_OK) return ret;

    /*
     * Step 7: GPIO 配置。
     */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_GPIO_REG_SEL, 0x00);
    if (ret != ESP_OK) return ret;

    /*
     * Step 8: ADC 路径（关闭）。
     */
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_ADC_REG_SYS, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_ADC_REG_MUTE, 0x01);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_ADC_REG_GAIN, 0x00);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_ADC_REG_LOOP, 0x00);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "ES8311 codec initialized, sample_rate=%lu",
             (unsigned long)sample_rate);

    return ESP_OK;
}

esp_err_t es8311_codec_set_volume(i2c_port_t i2c_port,
                                   uint8_t i2c_addr,
                                   uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }

    /*
     * ES8311 DAC 音量寄存器：
     * 0x00 = 0dB (最大)
     * 0xFF = -95.5dB (最小)
     * 0xFF = 静音
     *
     * 把 0-100 的用户音量映射为 0x00-0xFF。
     * volume=0 -> 静音 (0xFF)
     * volume=100 -> 最大 (0x00)
     */
    uint8_t reg_val;

    if (volume == 0) {
        reg_val = 0xFF;
    } else {
        reg_val = (uint8_t)((100 - volume) * 255 / 100);
    }

    esp_err_t ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_VOL_L, reg_val);
    if (ret != ESP_OK) return ret;
    ret = es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_VOL_R, reg_val);
    return ret;
}

esp_err_t es8311_codec_set_dac_mute(i2c_port_t i2c_port,
                                     uint8_t i2c_addr,
                                     bool enable)
{
    uint8_t val = enable ? 0x01 : 0x00;
    return es8311_write_reg(i2c_port, i2c_addr, ES8311_SYS_REG_DAC_MUTE, val);
}
