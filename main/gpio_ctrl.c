#include "gpio_ctrl.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

/* ESP-IDF headers ---------------------------------------------------------- */
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

/* FreeRTOS headers --------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

/* Project headers ---------------------------------------------------------- */
#include "board_config.h"
#include "app_settings.h"

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

/*
 * 蜂鸣器硬件类型选择：
 *
 * 0 = 有源蜂鸣器
 *     GPIO 输出固定高/低电平控制响停。
 *     一般“直接接 3.3V 就响”的是有源蜂鸣器。
 *
 * 1 = 无源蜂鸣器
 *     GPIO 通过 LEDC PWM 输出方波控制发声。
 *     可以通过改变 BUZZER_PASSIVE_FREQ_HZ 调整默认音调。
 *
 * 注意：
 * 这是硬件适配宏，不放到 UI 设置中。
 */
#define BUZZER_PASSIVE              1

/*
 * 有源蜂鸣器有效电平。
 *
 * 如果你的有源蜂鸣器模块是高电平响，保持 1。
 * 如果是低电平响，改成 0。
 */
#define BUZZER_ACTIVE_LEVEL         1

/*
 * 普通 beep 时长。
 */
#define BUZZER_BEEP_MS              120

/*
 * 无源蜂鸣器默认频率。
 *
 * 音调调节：
 * 1000~1500 Hz：偏低、柔和
 * 1800~2500 Hz：常用提示音
 * 3000~4000 Hz：偏尖锐
 */
#define BUZZER_PASSIVE_FREQ_HZ      950

/*
 * 无源蜂鸣器 LEDC PWM 配置。
 *
 * 注意：
 * 不要和屏幕背光使用同一个 LEDC timer/channel。
 * 背光常见使用 TIMER_0 / CHANNEL_0。
 *
 * 这里使用 TIMER_3 / CHANNEL_7 避免冲突。
 */
#define BUZZER_LEDC_MODE            LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER           LEDC_TIMER_3
#define BUZZER_LEDC_CHANNEL         LEDC_CHANNEL_7
#define BUZZER_LEDC_DUTY_RES        LEDC_TIMER_8_BIT   /* 改为8-bit，避免频率错误 */

/*
 * 8-bit 分辨率下，占空比范围 0~255。
 * 128 约等于 50% 占空比，适合无源蜂鸣器输出方波。
 */
#define BUZZER_LEDC_DUTY_ON         128
#define BUZZER_LEDC_DUTY_OFF        0

/*
 * CW 参数。
 *
 * 当前 CW 内容：DE
 *
 * D = -..
 * E = .
 */
#define CW_DOT_MS                   60
#define CW_DASH_MS                  (CW_DOT_MS * 3)
#define CW_INTRA_GAP_MS             CW_DOT_MS
#define CW_CHAR_GAP_MS              (CW_DOT_MS * 3)

/*
 * CW 播放任务配置。
 *
 * 优先级不要太高，避免和音频任务抢 CPU。
 */
#define BUZZER_CW_TASK_STACK_SIZE   2048
#define BUZZER_CW_TASK_PRIORITY     3

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

static const char *TAG = "gpio_ctrl";

static bool s_gpio_ctrl_inited = false;
static TimerHandle_t s_buzzer_timer = NULL;
static TaskHandle_t s_cw_task_handle = NULL;

static void gpio_ctrl_buzzer_mario_sound(void);

/* -------------------------------------------------------------------------- */
/* Private functions: buzzer basic control                                    */
/* -------------------------------------------------------------------------- */

static void gpio_ctrl_buzzer_set_off(void)
{
    if (BUZZER_PASSIVE) {
        ledc_set_duty(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_CHANNEL,
                      BUZZER_LEDC_DUTY_OFF);

        ledc_update_duty(BUZZER_LEDC_MODE,
                         BUZZER_LEDC_CHANNEL);
    } else {
        gpio_set_level(BUZZER_GPIO, !BUZZER_ACTIVE_LEVEL);
    }
}

static void gpio_ctrl_buzzer_set_on(void)
{
    if (BUZZER_PASSIVE) {
        ledc_set_freq(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_TIMER,
                      BUZZER_PASSIVE_FREQ_HZ);

        ledc_set_duty(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_CHANNEL,
                      BUZZER_LEDC_DUTY_ON);

        ledc_update_duty(BUZZER_LEDC_MODE,
                         BUZZER_LEDC_CHANNEL);
    } else {
        gpio_set_level(BUZZER_GPIO, BUZZER_ACTIVE_LEVEL);
    }
}

static void gpio_ctrl_buzzer_off_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    gpio_ctrl_buzzer_set_off();
}

/* -------------------------------------------------------------------------- */
/* Private functions: buzzer init                                             */
/* -------------------------------------------------------------------------- */

static esp_err_t gpio_ctrl_buzzer_active_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUZZER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "gpio_config failed, gpio=%d err=%s",
                 BUZZER_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    gpio_ctrl_buzzer_set_off();

    return ESP_OK;
}

static esp_err_t gpio_ctrl_buzzer_passive_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode       = BUZZER_LEDC_MODE,
        .timer_num        = BUZZER_LEDC_TIMER,
        .duty_resolution  = BUZZER_LEDC_DUTY_RES,
        .freq_hz          = BUZZER_PASSIVE_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };

    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ledc_timer_config failed, gpio=%d err=%s",
                 BUZZER_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num   = BUZZER_GPIO,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel    = BUZZER_LEDC_CHANNEL,
        .timer_sel  = BUZZER_LEDC_TIMER,
        .duty       = BUZZER_LEDC_DUTY_OFF,
        .hpoint     = 0,
    };

    err = ledc_channel_config(&channel_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ledc_channel_config failed, gpio=%d err=%s",
                 BUZZER_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    gpio_ctrl_buzzer_set_off();

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* 新版：优雅开机上电音效（适配无源蜂鸣器，无LEDC错误）                     */
/* -------------------------------------------------------------------------- */
static void gpio_ctrl_buzzer_power_on_chime(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing power-on chime sound (Nokia classic style)");

    typedef struct {
        int freq;
        int duration_ms;
    } buzzer_note_t;

    /*
     * Nokia classic tune 近似音符：
     * E6 D6 F#5 G#5 C#6 B5 D5 E5 B5 A5 C#5 E5 A5
     */
    static const buzzer_note_t nokia_tune[] = {
        { 1319, 120 },  // E6
        { 1175, 120 },  // D6
        {  740, 240 },  // F#5
        {  831, 240 },  // G#5

        { 1109, 120 },  // C#6
        {  988, 120 },  // B5
        {  587, 240 },  // D5
        {  659, 240 },  // E5

        {  988, 120 },  // B5
        {  880, 120 },  // A5
        {  554, 240 },  // C#5
        {  659, 240 },  // E5
        {  880, 520 },  // A5
    };

    const int note_gap_ms = 25;

    for (int i = 0; i < (int)(sizeof(nokia_tune) / sizeof(nokia_tune[0])); i++) {
        ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, nokia_tune[i].freq);

        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_LEDC_DUTY_ON);
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

        vTaskDelay(pdMS_TO_TICKS(nokia_tune[i].duration_ms));

        gpio_ctrl_buzzer_set_off();
        vTaskDelay(pdMS_TO_TICKS(note_gap_ms));
    }

    gpio_ctrl_buzzer_set_off();

    ESP_LOGI(TAG, "power-on chime finished");
}

/* -------------------------------------------------------------------------- */
/* 优化版：CS C4 炸弹倒计时音效（单个音调由慢到快 + 最后长音）             */
/* -------------------------------------------------------------------------- */

static void gpio_ctrl_buzzer_c4_countdown_sound(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing C4 countdown sound");

    const int freq = 2450;
    const int beep_ms = 55;

    for (int i = 0; i < 11; i++) {
        int interval = 420 - i * 32;
        if (interval < 80) {
            interval = 80;
        }

        ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq);
        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_LEDC_DUTY_ON);
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(beep_ms));

        gpio_ctrl_buzzer_set_off();
        vTaskDelay(pdMS_TO_TICKS(interval));
    }

    /*
     * 最后一声是真正的长音。
     */
    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, BUZZER_LEDC_DUTY_ON);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(420));

    gpio_ctrl_buzzer_set_off();

    ESP_LOGI(TAG, "C4 countdown sound finished");
}


/* -------------------------------------------------------------------------- */
/* Private functions: normal beep                                             */
/* -------------------------------------------------------------------------- */

static void gpio_ctrl_buzzer_beep_once_start(void)
{
    if (!s_buzzer_timer) {
        return;
    }

    /*
     * 如果 CW 正在响，先通知层面不强制终止任务，
     * 但先关一下蜂鸣器，避免两个声音重叠。
     */
    gpio_ctrl_buzzer_set_off();

    gpio_ctrl_buzzer_set_on();

    /*
     * 连续触发时重新计时，避免蜂鸣器提前关闭或任务堆积。
     */
    xTimerStop(s_buzzer_timer, 0);

    xTimerChangePeriod(s_buzzer_timer,
                       pdMS_TO_TICKS(BUZZER_BEEP_MS),
                       0);

    xTimerStart(s_buzzer_timer, 0);
}

/* -------------------------------------------------------------------------- */
/* Private functions: CW DE                                                   */
/* -------------------------------------------------------------------------- */

static void gpio_ctrl_cw_mark(uint32_t on_ms, uint32_t off_ms)
{
    gpio_ctrl_buzzer_set_on();
    vTaskDelay(pdMS_TO_TICKS(on_ms));

    gpio_ctrl_buzzer_set_off();

    if (off_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

static void gpio_ctrl_cw_dot(uint32_t off_ms)
{
    gpio_ctrl_cw_mark(CW_DOT_MS, off_ms);
}

static void gpio_ctrl_cw_dash(uint32_t off_ms)
{
    gpio_ctrl_cw_mark(CW_DASH_MS, off_ms);
}

/*
 * CW: DE
 *
 * D = -..
 * E = .
 */
static void gpio_ctrl_buzzer_cw_de_play(void)
{
    /*
     * D: -..
     */
    gpio_ctrl_cw_dash(CW_INTRA_GAP_MS);
    gpio_ctrl_cw_dot(CW_INTRA_GAP_MS);
    gpio_ctrl_cw_dot(CW_CHAR_GAP_MS);

    /*
     * E: .
     */
    gpio_ctrl_cw_dot(0);

    gpio_ctrl_buzzer_set_off();
}

static void gpio_ctrl_cw_task(void *arg)
{
    (void)arg;

    while (1) {
        /*
         * 等待 gpio_ctrl_buzzer_cw_sound() 触发。
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * 播放 DE。
         */
        gpio_ctrl_buzzer_cw_de_play();

        /*
         * 如果播放过程中又触发了多次，这里清掉积累的通知。
         * 避免一直重复播放导致堆积。
         */
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            /* clear pending notifications */
        }
    }
}

static esp_err_t gpio_ctrl_cw_task_init(void)
{
    if (s_cw_task_handle) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(gpio_ctrl_cw_task,
                                 "buzzer_cw",
                                 BUZZER_CW_TASK_STACK_SIZE,
                                 NULL,
                                 BUZZER_CW_TASK_PRIORITY,
                                 &s_cw_task_handle);

    if (ret != pdPASS) {
        s_cw_task_handle = NULL;
        ESP_LOGE(TAG, "create buzzer cw task failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void gpio_ctrl_buzzer_cw_de_start(void)
{
    /*
     * 防止普通 beep 定时器还没关闭时切到 CW。
     */
    if (s_buzzer_timer) {
        xTimerStop(s_buzzer_timer, 0);
    }

    gpio_ctrl_buzzer_set_off();

    if (s_cw_task_handle) {
        xTaskNotifyGive(s_cw_task_handle);
    }
}

/* -------------------------------------------------------------------------- */
/* Public functions                                                           */
/* -------------------------------------------------------------------------- */

void gpio_ctrl_init(void)
{
    if (s_gpio_ctrl_inited) {
        return;
    }

    /*
     * 立创实战派 ESP32-S3 无独立蜂鸣器，BUZZER_GPIO 为 NC。
     * 跳过蜂鸣器初始化，CW 任务也不创建。
     */
    if (BUZZER_GPIO == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "buzzer GPIO is NC, skip buzzer init");
        s_gpio_ctrl_inited = true;
        return;
    }

    esp_err_t err;

    if (BUZZER_PASSIVE) {
        err = gpio_ctrl_buzzer_passive_init();
    } else {
        err = gpio_ctrl_buzzer_active_init();
    }

    if (err != ESP_OK) {
        gpio_ctrl_buzzer_set_off();
        return;
    }

    s_buzzer_timer = xTimerCreate("buzzer_off",
                                  pdMS_TO_TICKS(BUZZER_BEEP_MS),
                                  pdFALSE,
                                  NULL,
                                  gpio_ctrl_buzzer_off_timer_cb);

    if (!s_buzzer_timer) {
        ESP_LOGE(TAG, "create buzzer timer failed");
        gpio_ctrl_buzzer_set_off();
        return;
    }

    /*
     * CW 任务始终创建。
     *
     * 原来是根据 BUZZER_SOUND_MODE 编译期选择是否创建。
     * 现在声音模式由设置页运行时选择，所以必须始终准备好。
     */
    err = gpio_ctrl_cw_task_init();
    if (err != ESP_OK) {
        gpio_ctrl_buzzer_set_off();
        return;
    }

    s_gpio_ctrl_inited = true;

    ESP_LOGI(TAG,
             "gpio ctrl init ok, gpio=%d type=%s",
             BUZZER_GPIO,
             BUZZER_PASSIVE ? "passive" : "active");
}

void gpio_ctrl_buzzer_beep_sound(void)
{
    if (!s_gpio_ctrl_inited) {
        gpio_ctrl_init();
    }

    if (!s_gpio_ctrl_inited || !s_buzzer_timer) {
        return;
    }

    gpio_ctrl_buzzer_beep_once_start();
}

void gpio_ctrl_buzzer_cw_sound(void)
{
    if (!s_gpio_ctrl_inited) {
        gpio_ctrl_init();
    }

    if (!s_gpio_ctrl_inited || !s_cw_task_handle) {
        return;
    }

    gpio_ctrl_buzzer_cw_de_start();
}

void gpio_ctrl_buzzer_notify(void)
{
    const app_settings_t *cfg = app_settings_get();

    app_buzzer_mode_t mode =
        cfg ? cfg->buzzer_mode : APP_BUZZER_MODE_OFF;

    switch (mode) {
    case APP_BUZZER_MODE_OFF:
        /*
         * 设置为关闭时确保蜂鸣器停止。
         */
        gpio_ctrl_buzzer_set_off();
        return;

    case APP_BUZZER_MODE_BEEP:
        gpio_ctrl_buzzer_beep_sound();
        break;

    case APP_BUZZER_MODE_CW:
        gpio_ctrl_buzzer_cw_sound();
        break;

    default:
        gpio_ctrl_buzzer_set_off();
        break;
    }
}

void gpio_ctrl_beep_once(void)
{
    /*
     * 兼容旧接口。
     *
     * 现在统一走设置项：
     * - 关：不响
     * - 提示音：普通 beep
     * - CW音：CW DE
     */
    gpio_ctrl_buzzer_notify();
}

/* -------------------------------------------------------------------------- */
/* FC 风格：超级玛丽短铃声模拟（仅无源蜂鸣器）                                */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint16_t freq;
    uint16_t ms;
} buzzer_note_t;

static void gpio_ctrl_buzzer_play_note(uint16_t freq, uint16_t ms)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    if (freq == 0) {
        gpio_ctrl_buzzer_set_off();
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }

    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, freq);
    ledc_set_duty(BUZZER_LEDC_MODE,
                  BUZZER_LEDC_CHANNEL,
                  BUZZER_LEDC_DUTY_ON);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(ms));

    gpio_ctrl_buzzer_set_off();

    /*
     * 每个音符之间留一点极短空隙，
     * 会更像 FC 方波音色，不会糊成一片。
     */
    vTaskDelay(pdMS_TO_TICKS(18));
}


static void gpio_ctrl_buzzer_mario_sound(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing mario slow sound");

    static const buzzer_note_t melody[] = {
        {2637, 130},  /* E7 */
        {2637, 130},
        {0,    130},
        {2637, 130},
        {0,    130},
        {2093, 130},  /* C7 */
        {2637, 140},
        {0,    130},
        {3136, 190},  /* G7 */
        {0,    300},
        {1568, 190},  /* G6 */
        {0,    300},

        {2093, 170},  /* C7 */
        {0,    110},
        {1568, 170},  /* G6 */
        {0,    110},
        {1319, 170},  /* E6 */
        {0,    110},
        {1760, 140},  /* A6 */
        {1976, 140},  /* B6 */
        {1865, 130},  /* AS6 */
        {1760, 140},  /* A6 */
        {1568, 160},  /* G6 */

        {2637, 145},  /* E7 */
        {3136, 145},  /* G7 */
        {3520, 170},  /* A7 */
        {2794, 145},  /* F7 */
        {3136, 145},  /* G7 */
        {0,    140},
        {2637, 145},  /* E7 */
        {2093, 145},  /* C7 */
        {2349, 145},  /* D7 */
        {1976, 230},  /* B6 */
    };

    for (size_t i = 0; i < sizeof(melody) / sizeof(melody[0]); i++) {
        gpio_ctrl_buzzer_play_note(melody[i].freq, melody[i].ms);
    }

    gpio_ctrl_buzzer_set_off();
}
/* -------------------------------------------------------------------------- */
/* FC 风格：超级玛丽开始音（仅无源蜂鸣器）                                */
/* -------------------------------------------------------------------------- */
static void gpio_ctrl_buzzer_mario_start_sound(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing mario start sound");

    static const buzzer_note_t melody[] = {
        {2637, 90},   /* E7 */
        {2637, 90},   /* E7 */
        {0,    80},
        {2637, 90},   /* E7 */
        {0,    90},
        {2093, 90},   /* C7 */
        {2637, 100},  /* E7 */
        {0,    90},
        {3136, 150},  /* G7 */
        {0,    220},
        {1568, 150},  /* G6 */
    };

    for (size_t i = 0; i < sizeof(melody) / sizeof(melody[0]); i++) {
        gpio_ctrl_buzzer_play_note(melody[i].freq, melody[i].ms);
    }

    gpio_ctrl_buzzer_set_off();
}

/* -------------------------------------------------------------------------- */
/* 超级玛丽失败音                                                              */
/* -------------------------------------------------------------------------- */
static void gpio_ctrl_buzzer_mario_fail_sound(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing mario fail sound");

    static const buzzer_note_t melody[] = {
        {2093, 120},  /* C7 */
        {1976, 120},  /* B6 */
        {1865, 120},  /* AS6 */
        {1760, 150},  /* A6 */
        {0,    60},
        {1661, 140},  /* GS6 */
        {1568, 140},  /* G6 */
        {1480, 150},  /* FS6 */
        {1397, 180},  /* F6 */
        {0,    80},
        {1319, 220},  /* E6 */
        {1047, 260},  /* C6 */
        {784,  360},  /* G5 */
    };

    for (size_t i = 0; i < sizeof(melody) / sizeof(melody[0]); i++) {
        gpio_ctrl_buzzer_play_note(melody[i].freq, melody[i].ms);
    }

    gpio_ctrl_buzzer_set_off();
}
/* -------------------------------------------------------------------------- */
/* 魂斗罗                                                             */
/* -------------------------------------------------------------------------- */
static void gpio_ctrl_buzzer_contra_jungle_sound(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing contra jungle rhythm sound");

    static const buzzer_note_t melody[] = {
        {988,  70},   /* B5 */
        {0,    35},
        {988,  70},
        {0,    35},
        {1175, 85},   /* D6 */
        {988,  70},
        {0,    40},

        {1319, 90},   /* E6 */
        {1175, 70},
        {988,  70},
        {880,  90},   /* A5 */
        {0,    45},

        {988,  70},
        {0,    35},
        {1175, 70},
        {0,    35},
        {1319, 85},
        {1568, 90},   /* G6 */
        {0,    50},

        {1480, 80},   /* FS6 */
        {1319, 80},
        {1175, 80},
        {988,  130},
        {0,    80},

        /* 第二小节，强化循环感 */
        {784,  70},   /* G5 */
        {988,  70},
        {1175, 70},
        {988,  70},
        {784,  90},
        {0,    40},

        {880,  70},
        {988,  70},
        {1175, 90},
        {1319, 90},
        {1175, 70},
        {988,  150},
    };

    for (size_t i = 0; i < sizeof(melody) / sizeof(melody[0]); i++) {
        gpio_ctrl_buzzer_play_note(melody[i].freq, melody[i].ms);
    }

    gpio_ctrl_buzzer_set_off();
}

/* -------------------------------------------------------------------------- */
/* 盖革计数器辐射检测滴答声（仅无源蜂鸣器）                                  */
/* -------------------------------------------------------------------------- */
static void gpio_ctrl_buzzer_geiger_counter_sound(void)
{
    if (!BUZZER_PASSIVE) {
        return;
    }

    ESP_LOGI(TAG, "playing geiger counter ramp sound");

    const uint16_t freqs[] = {
        3000, 3300, 2800, 3500, 3200, 3600, 3100, 3400
    };

    /*
     * 间隔逐渐缩短，模拟辐射强度增强。
     */
    const uint16_t gaps[] = {
        280, 220, 180, 150, 120, 95, 75, 60,
        50, 42, 36, 32, 28, 24, 20, 18
    };

    for (int i = 0; i < 16; i++) {
        uint16_t freq = freqs[i % (sizeof(freqs) / sizeof(freqs[0]))];

        ledc_set_freq(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_TIMER,
                      freq);

        ledc_set_duty(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_CHANNEL,
                      BUZZER_LEDC_DUTY_ON);

        ledc_update_duty(BUZZER_LEDC_MODE,
                         BUZZER_LEDC_CHANNEL);

        /*
         * 滴答声很短。
         */
        vTaskDelay(pdMS_TO_TICKS(12 + (i % 3) * 3));

        gpio_ctrl_buzzer_set_off();

        vTaskDelay(pdMS_TO_TICKS(gaps[i]));
    }

    /*
     * 最后密集几声。
     */
    for (int i = 0; i < 8; i++) {
        ledc_set_freq(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_TIMER,
                      3400 + (i % 2) * 250);

        ledc_set_duty(BUZZER_LEDC_MODE,
                      BUZZER_LEDC_CHANNEL,
                      BUZZER_LEDC_DUTY_ON);

        ledc_update_duty(BUZZER_LEDC_MODE,
                         BUZZER_LEDC_CHANNEL);

        vTaskDelay(pdMS_TO_TICKS(10));

        gpio_ctrl_buzzer_set_off();

        vTaskDelay(pdMS_TO_TICKS(22));
    }

    gpio_ctrl_buzzer_set_off();

    ESP_LOGI(TAG, "geiger counter ramp sound finished");
}


void gpio_ctrl_buzzer_play_effect(app_sound_effect_t effect)
{
    if (!s_gpio_ctrl_inited) {
        gpio_ctrl_init();
    }

    if (!s_gpio_ctrl_inited) {
        return;
    }

    switch (effect) {
    case APP_SOUND_EFFECT_OFF:
        gpio_ctrl_buzzer_set_off();
        break;

    case APP_SOUND_EFFECT_BEEP:
        gpio_ctrl_buzzer_beep_sound();
        break;

    case APP_SOUND_EFFECT_CW_DE:
        gpio_ctrl_buzzer_cw_sound();
        break;

    case APP_SOUND_EFFECT_POWER_ON:
        gpio_ctrl_buzzer_power_on_chime();
        break;

    case APP_SOUND_EFFECT_C4_COUNTDOWN:
        gpio_ctrl_buzzer_c4_countdown_sound();
        break;

    case APP_SOUND_EFFECT_MARIO:
        gpio_ctrl_buzzer_mario_sound();
        break;

    case APP_SOUND_EFFECT_MARIO_START:
        gpio_ctrl_buzzer_mario_start_sound();
        break;

    case APP_SOUND_EFFECT_MARIO_FAIL:
        gpio_ctrl_buzzer_mario_fail_sound();
        break;

    case APP_SOUND_EFFECT_CONTRA_JUNGLE:
        gpio_ctrl_buzzer_contra_jungle_sound();
        break;
    case APP_SOUND_EFFECT_GEIGER:
        gpio_ctrl_buzzer_geiger_counter_sound();
        break;
    default:
        gpio_ctrl_buzzer_set_off();
        break;
    }
}
/* -------------------------------------------------------------------------- */
/* 新增公开接口（兼容旧调用 + 新功能）                                       */
/* -------------------------------------------------------------------------- */

/* 兼容 main.c 中旧函数名 */
void gpio_ctrl_buzzer_gun_load(void)
{
    if (!s_gpio_ctrl_inited) gpio_ctrl_init();
    gpio_ctrl_buzzer_power_on_chime();
}

/* 新推荐名称（功能相同） */
void gpio_ctrl_buzzer_power_on(void)
{
    gpio_ctrl_buzzer_gun_load();
}

void gpio_ctrl_buzzer_c4_countdown(void)
{
    if (!s_gpio_ctrl_inited) gpio_ctrl_init();
    gpio_ctrl_buzzer_c4_countdown_sound();
}
