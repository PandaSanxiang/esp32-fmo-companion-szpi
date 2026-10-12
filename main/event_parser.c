/*
 * Copyright (c) 2026 BI8SIG
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file event_parser.c
 * @brief Event WebSocket JSON 解析实现。
 *
 * 本模块解析来自 Event WebSocket 或 Station WebSocket 转发的 qso JSON。
 *
 * 当前处理逻辑：
 * - qso/callsign：
 *   - isSpeaking=true：更新当前通联呼号、唤醒待机页、打开音频门控；
 *   - isSpeaking=false：结束当前通联，将当前呼号更新为上次通联；
 *
 * - qso/history：
 *   - 当前忽略，不再用 history 更新“上次通联”；
 *
 * - qso/getListResponse：
 *   - 解析 QSO 列表页码、数量、latest log id 和 log id 列表；
 *   - 转交 audio_ws_qso_count_handle_response() 处理同步状态机。
 *
 * - QTH查询：
 *   - qso/callsign data.grid 存在时；
 *   - 如果设置中开启 QTH查询，则异步请求 API 并显示地址。
 */

#include "event_parser.h"

/* Standard library headers ------------------------------------------------- */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Third-party headers ------------------------------------------------------ */
#include "cJSON.h"

/* ESP-IDF headers ---------------------------------------------------------- */
#include "esp_log.h"

/* Project headers ---------------------------------------------------------- */
#include "audio_ws.h"
#include "ui_async.h"
#include "qth_lookup.h"
#include "gpio_ctrl.h"

/* -------------------------------------------------------------------------- */
/* Log tag                                                                    */
/* -------------------------------------------------------------------------- */

static const char *TAG = "event_parser";

/* -------------------------------------------------------------------------- */
/* Private macros                                                             */
/* -------------------------------------------------------------------------- */

#define EVENT_CURRENT_CALLSIGN_MAX_LEN  32

#define EVENT_QSO_LOG_ID_MAX_COUNT      24

/* -------------------------------------------------------------------------- */
/* Private variables                                                          */
/* -------------------------------------------------------------------------- */

static char s_current_qso_callsign[EVENT_CURRENT_CALLSIGN_MAX_LEN] = {0};

static bool s_current_qso_active = false;

/* -------------------------------------------------------------------------- */
/* Private function declarations                                              */
/* -------------------------------------------------------------------------- */

static const char *json_get_string(cJSON *obj, const char *key);
static bool json_get_bool(cJSON *obj, const char *key, bool def_val);

static void event_parser_cache_current_callsign(const char *callsign);
static void event_parser_finish_current_qso(const char *fallback_callsign);

static void event_parser_handle_callsign_event(const char *callsign,
                                               const char *grid,
                                               bool is_speaking);

static void event_parser_handle_qso_get_list_response(cJSON *root);

/* -------------------------------------------------------------------------- */
/* JSON helpers                                                               */
/* -------------------------------------------------------------------------- */

static const char *json_get_string(cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);

    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }

    return NULL;
}

static bool json_get_bool(cJSON *obj, const char *key, bool def_val)
{
    if (!obj || !key) {
        return def_val;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);

    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }

    return def_val;
}

/* -------------------------------------------------------------------------- */
/* Current QSO callsign cache                                                 */
/* -------------------------------------------------------------------------- */

static void event_parser_cache_current_callsign(const char *callsign)
{
    if (!callsign || callsign[0] == '\0') {
        return;
    }

    snprintf(s_current_qso_callsign,
             sizeof(s_current_qso_callsign),
             "%s",
             callsign);
}

static void event_parser_finish_current_qso(const char *fallback_callsign)
{
    const char *last_call = NULL;

    if (s_current_qso_callsign[0]) {
        last_call = s_current_qso_callsign;
    } else if (fallback_callsign && fallback_callsign[0]) {
        last_call = fallback_callsign;
    }

    if (last_call && last_call[0]) {
        ui_async_update_last_call(last_call);

        ESP_LOGI(TAG, "last callsign from current qso=%s", last_call);

        ui_async_update_talker_state(last_call, false);
    } else {
        ui_async_update_talker_state(NULL, false);
    }

    s_current_qso_active = false;
    //ui_async_update_qth_address(NULL);
}

/* -------------------------------------------------------------------------- */
/* qso/callsign handling                                                      */
/* -------------------------------------------------------------------------- */

static void event_parser_handle_callsign_event(const char *callsign,
                                               const char *grid,
                                               bool is_speaking)
{
    if (is_speaking) {
        if (!callsign || callsign[0] == '\0') {
            ESP_LOGW(TAG, "speaking event without callsign");
            return;
        }

        if (s_current_qso_active &&
            s_current_qso_callsign[0] &&
            strcmp(s_current_qso_callsign, callsign) != 0) {

            ui_async_update_last_call(s_current_qso_callsign);

            ESP_LOGW(TAG,
                     "new speaking before previous end, previous=%s, new=%s",
                     s_current_qso_callsign,
                     callsign);
        }

        event_parser_cache_current_callsign(callsign);
        s_current_qso_active = true;

        ui_async_wake_from_idle();

        audio_ws_set_speaking(true);
        gpio_ctrl_beep_once();

        ui_async_update_talker_state(s_current_qso_callsign, true);
        ui_async_update_status("通联中");

        /*
         * QTH查询：
         * callsign事件中 data.grid 存在时触发。
         *
         * qth_lookup_request_grid() 内部会判断设置开关；
         * 如果 QTH查询关闭，则不会请求网络。
         */

        if (grid && grid[0]) {
            ESP_LOGI(TAG, "callsign grid=%s", grid);
            qth_lookup_request_grid(grid);
        }

        ESP_LOGD(TAG,
                 "speaking callsign=%s grid=%s",
                 callsign,
                 grid ? grid : "");

        return;
    }

    audio_ws_set_speaking(false);

    event_parser_finish_current_qso(callsign);

    ui_async_update_status("空闲");
}

/* -------------------------------------------------------------------------- */
/* Public JSON entry                                                          */
/* -------------------------------------------------------------------------- */

/*
 * WebSocket 收到的数据在一些情况下可能不是“纯单个 JSON 对象”：
 * - 前面带少量协议前缀，例如 Socket.IO 风格的 42[...]；
 * - 一个 payload 中拼了多个 JSON 对象；
 * - 末尾带换行、NUL 或其他非 JSON 字节；
 * - 返回的是 JSON 数组，数组中包含真正的事件对象。
 *
 * 这里先用一个轻量扫描器找出完整 JSON 值的边界，再逐个交给 cJSON。
 * 这样不会因为尾部多余字节或多个对象拼在一起而整包解析失败。
 */
static bool event_parser_find_json_value(const char *buf,
                                         int len,
                                         int from,
                                         int *out_start,
                                         int *out_end)
{
    if (!buf || len <= 0 || from >= len || !out_start || !out_end) {
        return false;
    }

    int start = -1;

    for (int i = from; i < len; i++) {
        if (buf[i] == '{' || buf[i] == '[') {
            start = i;
            break;
        }
    }

    if (start < 0) {
        return false;
    }

    char open_ch = buf[start];
    char close_ch = (open_ch == '{') ? '}' : ']';

    int depth = 0;
    bool in_string = false;
    bool escape = false;

    for (int i = start; i < len; i++) {
        char c = buf[i];

        if (in_string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }

            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }

        if (c == open_ch) {
            depth++;
        } else if (c == close_ch) {
            depth--;

            if (depth == 0) {
                *out_start = start;
                *out_end = i + 1;
                return true;
            }
        }
    }

    /*
     * 没找到完整闭合，说明 payload 可能被截断。
     */
    return false;
}

static void event_parser_log_json_parse_error(const char *json, int len)
{
    if (!json || len <= 0) {
        ESP_LOGW(TAG, "JSON parse failed, empty input");
        return;
    }

    const char *err = cJSON_GetErrorPtr();
    int err_off = -1;

    if (err && err >= json && err < json + len) {
        err_off = (int)(err - json);
    }

    char head[129];
    char tail[129];

    int head_len = len < 128 ? len : 128;
    memcpy(head, json, head_len);
    head[head_len] = '\0';

    for (int i = 0; i < head_len; i++) {
        if (head[i] == '\r' || head[i] == '\n') {
            head[i] = ' ';
        }
    }

    int tail_len = len < 128 ? len : 128;
    memcpy(tail, json + len - tail_len, tail_len);
    tail[tail_len] = '\0';

    for (int i = 0; i < tail_len; i++) {
        if (tail[i] == '\r' || tail[i] == '\n') {
            tail[i] = ' ';
        }
    }

    ESP_LOGW(TAG,
             "JSON parse failed, len=%d, err_off=%d, head=%s",
             len,
             err_off,
             head);
    ESP_LOGW(TAG, "JSON tail=%s", tail);
}

static void event_parser_handle_root(cJSON *root)
{
    if (!root) {
        return;
    }

    /*
     * 兼容数组封装，例如 ["event", {...}] 或 [{...}, {...}]。
     * 只处理其中的对象项。
     */
    if (cJSON_IsArray(root)) {
        int size = cJSON_GetArraySize(root);

        for (int i = 0; i < size; i++) {
            cJSON *item = cJSON_GetArrayItem(root, i);
            if (cJSON_IsObject(item)) {
                event_parser_handle_root(item);
            }
        }

        return;
    }

    if (!cJSON_IsObject(root)) {
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *subType = cJSON_GetObjectItem(root, "subType");
    cJSON *data = cJSON_GetObjectItem(root, "data");

    /*
     * 如果外层是 {"event":{...}} / {"payload":{...}} 这类包装，
     * 尝试向下找一层。
     */
    if (!cJSON_IsString(type) || !cJSON_IsString(subType) || !data) {
        cJSON *nested = cJSON_GetObjectItem(root, "event");
        if (cJSON_IsObject(nested)) {
            event_parser_handle_root(nested);
            return;
        }

        nested = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(nested)) {
            event_parser_handle_root(nested);
            return;
        }

        nested = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsObject(nested)) {
            event_parser_handle_root(nested);
            return;
        }

        return;
    }

    if (strcmp(type->valuestring, "qso") == 0) {
        if (strcmp(subType->valuestring, "callsign") == 0 &&
            cJSON_IsObject(data)) {

            const char *callsign = json_get_string(data, "callsign");
            const char *grid = json_get_string(data, "grid");
            bool is_speaking = json_get_bool(data, "isSpeaking", false);

            event_parser_handle_callsign_event(callsign,
                                               grid,
                                               is_speaking);
        } else if (strcmp(subType->valuestring, "history") == 0) {
            ESP_LOGD(TAG, "ignore qso history");
        } else if (strcmp(subType->valuestring, "getListResponse") == 0) {
            event_parser_handle_qso_get_list_response(root);
        }
    }
}

void event_parser_handle_json(const char *json, int len)
{
    if (!json || len <= 0) {
        return;
    }

    int pos = 0;
    int handled = 0;

    while (pos < len) {
        int start = 0;
        int end = 0;

        if (!event_parser_find_json_value(json, len, pos, &start, &end)) {
            break;
        }

        if (start > pos) {
            ESP_LOGD(TAG,
                     "skip non-json prefix, offset=%d size=%d",
                     pos,
                     start - pos);
        }

        cJSON *root = cJSON_ParseWithLength(json + start, end - start);
        if (!root) {
            event_parser_log_json_parse_error(json + start, end - start);
            break;
        }

        event_parser_handle_root(root);
        cJSON_Delete(root);

        handled++;
        pos = end;
    }

    if (handled == 0) {
        event_parser_log_json_parse_error(json, len);
    }
}

/* -------------------------------------------------------------------------- */
/* qso/getListResponse handling                                               */
/* -------------------------------------------------------------------------- */

static void event_parser_handle_qso_get_list_response(cJSON *root)
{
    if (!root) {
        return;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        ESP_LOGW(TAG,
                 "qso getListResponse code=%d",
                 code->valueint);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        ESP_LOGW(TAG, "qso getListResponse data missing/not object");
        return;
    }

    cJSON *page_item = cJSON_GetObjectItem(data, "page");
    cJSON *page_size_item = cJSON_GetObjectItem(data, "pageSize");
    cJSON *count_item = cJSON_GetObjectItem(data, "count");
    cJSON *list = cJSON_GetObjectItem(data, "list");

    int page = cJSON_IsNumber(page_item) ? page_item->valueint : 0;
    int page_size =
        cJSON_IsNumber(page_size_item) ? page_size_item->valueint : 20;

    int count = -1;

    if (cJSON_IsNumber(count_item)) {
        count = count_item->valueint;
    } else if (cJSON_IsArray(list)) {
        count = cJSON_GetArraySize(list);
    }

    if (count < 0) {
        count = 0;
    }

    int latest_log_id = -1;

    int log_ids[EVENT_QSO_LOG_ID_MAX_COUNT];
    int log_id_count = 0;

    if (cJSON_IsArray(list)) {
        int arr_size = cJSON_GetArraySize(list);

        if (arr_size > EVENT_QSO_LOG_ID_MAX_COUNT) {
            arr_size = EVENT_QSO_LOG_ID_MAX_COUNT;
        }

        for (int i = 0; i < arr_size; i++) {
            cJSON *item = cJSON_GetArrayItem(list, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }

            cJSON *log_id = cJSON_GetObjectItem(item, "logId");
            if (!cJSON_IsNumber(log_id)) {
                continue;
            }

            int id = log_id->valueint;

            if (i == 0) {
                latest_log_id = id;
            }

            log_ids[log_id_count++] = id;
        }
    }

    ESP_LOGD(TAG,
             "qso getListResponse page=%d pageSize=%d count=%d latest=%d ids=%d",
             page,
             page_size,
             count,
             latest_log_id,
             log_id_count);

    audio_ws_qso_count_handle_response(page,
                                       page_size,
                                       count,
                                       latest_log_id,
                                       log_ids,
                                       log_id_count);
}
