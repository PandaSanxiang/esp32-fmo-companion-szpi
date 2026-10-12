/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file es8311_codec.h
 * @brief ES8311 音频编解码器 I2C 驱动接口（最小化）。
 *
 * 本模块通过 I2C 配置 ES8311 编解码器，使其工作在 I2S 从模式、
 * 16-bit、双声道，接收 ESP32-S3 I2S 接口输出的 16-bit PCM 数据。
 *
 * 仅实现 DAC 播放路径，不包含 ADC 录音路径。
 */

#ifndef ES8311_CODEC_H
#define ES8311_CODEC_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ES8311 编解码器。
 *
 * 本函数假设 I2C 总线已经由外部初始化完成。
 * 函数内部通过 I2C 向 ES8311 写入配置寄存器序列。
 *
 * @param i2c_port I2C 总线编号
 * @param i2c_addr ES8311 的 7-bit I2C 地址（通常为 0x18）
 * @param sample_rate 期望的采样率（Hz），例如 16000 / 32000 / 44100
 * @return ESP_OK 成功，其他值表示 I2C 通信失败
 */
esp_err_t es8311_codec_init(i2c_port_t i2c_port,
                             uint8_t i2c_addr,
                             uint32_t sample_rate);

/**
 * @brief 设置 ES8311 DAC 输出音量。
 *
 * @param i2c_port I2C 总线编号
 * @param i2c_addr ES8311 的 7-bit I2C 地址
 * @param volume 0-100，0 为静音，100 为最大音量
 * @return ESP_OK 成功
 */
esp_err_t es8311_codec_set_volume(i2c_port_t i2c_port,
                                   uint8_t i2c_addr,
                                   uint8_t volume);

/**
 * @brief 使能/静音 ES8311 DAC 输出。
 *
 * @param i2c_port I2C 总线编号
 * @param i2c_addr ES8311 的 7-bit I2C 地址
 * @param enable true 使能输出，false 静音
 * @return ESP_OK 成功
 */
esp_err_t es8311_codec_set_dac_mute(i2c_port_t i2c_port,
                                     uint8_t i2c_addr,
                                     bool enable);

#ifdef __cplusplus
}
#endif

#endif /* ES8311_CODEC_H */
