#include "app_settings.h"
#ifndef GPIO_CTRL_H
#define GPIO_CTRL_H

#ifdef __cplusplus

extern "C" {
#endif

/*
 * 初始化 GPIO/蜂鸣器。
 *
 * 硬件类型由 gpio_ctrl.c 中 BUZZER_PASSIVE 宏决定：
 * 0 = 有源蜂鸣器
 * 1 = 无源蜂鸣器
 */
void gpio_ctrl_init(void);

/*
 * 兼容旧接口。
 *
 * 现在该函数会根据设置中的蜂鸣器模式决定：
 * - 关：不响
 * - 提示音：普通 beep
 * - CW音：CW DE
 */
void gpio_ctrl_beep_once(void);

/*
 * 根据设置播放通知音。
 *
 * 推荐其他模块统一调用这个函数。
 */
void gpio_ctrl_buzzer_notify(void);

/*
 * 直接播放普通提示音。
 *
 * 不检查设置项，主要用于设置页预览或内部调用。
 */
void gpio_ctrl_buzzer_beep_sound(void);

/*
 * 直接播放 CW 提示音。
 *
 * 不检查设置项，主要用于设置页预览或内部调用。
 */
void gpio_ctrl_buzzer_cw_sound(void);

/* 开机枪上膛音效（仅无源蜂鸣器有效） */
void gpio_ctrl_buzzer_gun_load(void);

/* CS C4 倒计时音效（仅无源蜂鸣器有效） */
void gpio_ctrl_buzzer_c4_countdown(void);

void gpio_ctrl_buzzer_play_effect(app_sound_effect_t effect);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_CTRL_H */
