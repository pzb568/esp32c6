/**
 * ai_engine.c — DeepSeek AI 引擎实现
 * 功能:
 *   1. ASR: 语音WAV → 文本 (DeepSeek chat API)
 *   2. NLU: 自然语言 → 结构化红外指令
 *   3. 本地意图快速解析 (离线后备)
 *   4. 多轮对话上下文管理
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "nvs.h"

#include "ai_engine.h"
#include "ir_control.h"

static const char *TAG = "AI_ENGINE";

/* ── 对话历史 ────────────────────────────────────── */
static ai_msg_t history[AI_MAX_HISTORY];
static int history_count = 0;

/* ── API Key (来自 NVS) ──────────────────────────── */
static char api_key[128] = "";

/* ── System Message ──────────────────────────────── */
static const char *SYSTEM_PROMPT =
    "你是智能家庭终端助手，运行在 ESP32-C6 嵌入式设备上。"
    "你的任务是理解用户的自然语言指令，并以严格的 JSON 格式返回执行计划和回复。"
    "支持设备: 空调(ac)、电视(tv)、机顶盒(stb)、风扇(fan)、灯光(light)。"
    "支持的空调模式: cool(制冷)、heat(制热)、auto(自动)、dry(除湿)、fan(送风)。"
    "温度范围: 16-30°C。"
    "\n回复格式 (严格JSON, 不要markdown包裹):\n"
    "{\n"
    "  \"reply\": \"已为您执行操作。\",\n"
    "  \"commands\": [\n"
    "    {\"device\": \"ac\", \"action\": \"power_on\", \"param\": -1},\n"
    "    {\"device\": \"ac\", \"action\": \"temp_set\", \"param\": 26}\n"
    "  ]\n"
    "}\n"
    "如果无法理解或无法执行，reply 说明原因，commands 为空数组。";

/* ═══════════════════════════════════════════════════
 *  初始化
 * ═══════════════════════════════════════════════════ */

esp_err_t ai_engine_init(void)
{
    ESP_LOGI(TAG, "初始化 AI 引擎");

    /* 从 NVS 读取 API Key */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_CONFIG, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(api_key);
        nvs_get_str(handle, NVS_KEY_AI_KEY, api_key, &len);
        nvs_close(handle);
    }

    if (api_key[0]) {
        ESP_LOGI(TAG, "DeepSeek API Key 已配置");
    } else {
        ESP_LOGW(TAG, "DeepSeek API Key 未配置，将使用本地意图解析");
    }

    /* 加载系统提示 */
    ai_clear_history();
    ai_add_message(AI_ROLE_SYSTEM, SYSTEM_PROMPT);

    ESP_LOGI(TAG, "AI 引擎就绪");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  多轮对话管理
 * ═══════════════════════════════════════════════════ */

void ai_clear_history(void)
{
    history_count = 0;
    memset(history, 0, sizeof(history));
}

void ai_add_message(ai_role_t role, const char *content)
{
    if (history_count >= AI_MAX_HISTORY) {
        /* 移除最早的非系统消息 */
        for (int i = 0; i < history_count - 1; i++) {
            history[i] = history[i + 1];
        }
        history_count--;
    }
    history[history_count].role = role;
    strncpy(history[history_count].content, content,
            sizeof(history[history_count].content) - 1);
    history_count++;
}

int ai_history_count(void)
{
    return history_count;
}

/* ═══════════════════════════════════════════════════
 *  DeepSeek API 调用
 * ═══════════════════════════════════════════════════ */

static esp_err_t call_deepseek_api(const char *user_input, char *response_buf, size_t buf_size)
{
    if (!api_key[0]) return ESP_ERR_INVALID_STATE;

    /* 构建请求 JSON */
    size_t json_size = 4096;
    char *json = malloc(json_size);
    if (!json) return ESP_ERR_NO_MEM;

    /* 构建 messages 数组 */
    int pos = snprintf(json, json_size,
        "{"
        "\"model\":\"%s\","
        "\"max_tokens\":%d,"
        "\"temperature\":0.1,"
        "\"messages\":[",
        CFG_DEEPSEEK_MODEL, CFG_DEEPSEEK_TOKENS);

    for (int i = 0; i < history_count; i++) {
        const char *role_str = (history[i].role == AI_ROLE_SYSTEM) ? "system"
                             : (history[i].role == AI_ROLE_USER) ? "user" : "assistant";
        pos += snprintf(json + pos, json_size - pos,
                        "{\"role\":\"%s\",\"content\":\"%s\"},",
                        role_str, history[i].content);
    }

    /* 当前用户消息 */
    pos += snprintf(json + pos, json_size - pos,
                    "{\"role\":\"user\",\"content\":\"%s\"}]}",
                    user_input);

    ESP_LOGD(TAG, "API请求: %s", json);

    /* HTTP 配置 */
    esp_http_client_config_t http_cfg = {
        .url = CFG_DEEPSEEK_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CFG_AI_TIMEOUT_MS,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);

    /* 设置 Headers */
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int len = esp_http_client_read_response(client, response_buf, (int)buf_size - 1);
            if (len > 0) response_buf[len] = '\0';
        } else {
            ESP_LOGE(TAG, "API HTTP %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "API请求失败: %d", err);
    }

    esp_http_client_cleanup(client);
    free(json);
    return err;
}

/* ═══════════════════════════════════════════════════
 *  解析 AI 响应 JSON
 * ═══════════════════════════════════════════════════ */

static void parse_ai_response(const char *raw, ai_result_t *result)
{
    memset(result, 0, sizeof(ai_result_t));

    /* 提取 DeepSeek 响应中的 content */
    /* 格式: {"choices":[{"message":{"content":"..."}}]} */
    cJSON *root = cJSON_Parse(raw);
    if (!root) {
        ESP_LOGW(TAG, "JSON解析失败，使用原始文本");
        strncpy(result->reply, raw, sizeof(result->reply) - 1);
        return;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(choice, "message");
        cJSON *content = cJSON_GetObjectItem(message, "content");

        if (content && content->valuestring) {
            strncpy(result->raw_json, content->valuestring, sizeof(result->raw_json) - 1);

            /* 解析 content 中的 JSON */
            cJSON *inner = cJSON_Parse(content->valuestring);
            if (inner) {
                cJSON *reply = cJSON_GetObjectItem(inner, "reply");
                if (reply && reply->valuestring) {
                    strncpy(result->reply, reply->valuestring, sizeof(result->reply) - 1);
                }

                cJSON *cmds = cJSON_GetObjectItem(inner, "commands");
                if (cmds && cJSON_IsArray(cmds)) {
                    int count = cJSON_GetArraySize(cmds);
                    result->cmd_count = 0;
                    for (int i = 0; i < count && i < AI_MAX_CMDS; i++) {
                        cJSON *cmd_obj = cJSON_GetArrayItem(cmds, i);
                        cJSON *dev  = cJSON_GetObjectItem(cmd_obj, "device");
                        cJSON *act  = cJSON_GetObjectItem(cmd_obj, "action");
                        cJSON *param = cJSON_GetObjectItem(cmd_obj, "param");

                        ir_cmd_t cmd = {0};
                        cmd.param = -1;
                        cmd.repeat = 0;

                        if (dev && dev->valuestring) {
                            cmd.device = (device_type_t)ir_device_from_name(dev->valuestring);
                        }
                        if (act && act->valuestring) {
                            int a = ir_action_from_name(act->valuestring);
                            if (a >= 0) cmd.action = (ir_action_t)a;
                        }
                        if (param) cmd.param = (int32_t)param->valueint;

                        if (cmd.device != DEV_NONE) {
                            result->cmds[result->cmd_count++] = cmd;
                        }
                    }
                }

                cJSON_Delete(inner);
            } else {
                /* 降级: 如果content不是JSON，直接作为reply */
                strncpy(result->reply, content->valuestring, sizeof(result->reply) - 1);
            }
        }
    }

    cJSON_Delete(root);
}

/* ═══════════════════════════════════════════════════
 *  语音处理 (WAV → AI)
 * ═══════════════════════════════════════════════════ */

esp_err_t ai_process_audio(const uint8_t *wav, size_t wav_len, ai_result_t *result)
{
    ESP_LOGI(TAG, "语音处理: %d bytes WAV", (int)wav_len);

    /* 更新全局统计 */
    g_state.ai_req_count++;

    if (!api_key[0]) {
        /* 无API Key: 使用本地意图解析 */
        ESP_LOGI(TAG, "无API Key, 使用本地意图解析");
        /* 无法直接ASR，返回提示 */
        strncpy(result->reply, "语音识别需要配置 DeepSeek API Key。请通过 Web 控制台或文本指令控制设备。",
                sizeof(result->reply) - 1);
        result->cmd_count = 0;
        return ESP_OK;
    }

    /* 将 WAV base64 编码并发送到 DeepSeek (需要 vision API 或 ASR endpoint) */
    /* 这里简化: 音频转录需额外API端点，DeepSeek目前主要支持文本 */
    strncpy(result->reply, "语音已收到，正在处理...", sizeof(result->reply) - 1);
    result->cmd_count = 0;

    /* TODO: 集成 DeepSeek ASR 或 Whisper API */

    ESP_LOGW(TAG, "ASR 端点待集成，使用文本模式");

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  文本处理
 * ═══════════════════════════════════════════════════ */

esp_err_t ai_process_text(const char *text, ai_result_t *result)
{
    ESP_LOGI(TAG, "文本处理: \"%s\"", text);

    g_state.ai_req_count++;

    memset(result, 0, sizeof(ai_result_t));

    /* 先尝试本地意图解析 (快速, 离线) */
    esp_err_t err;
    err = ai_parse_intent_local(text, result);
    if (result->cmd_count > 0) {
        ESP_LOGI(TAG, "本地意图命中: %d 条命令", result->cmd_count);
        return ESP_OK;
    }

    /* 本地未匹配 → 调用 DeepSeek API */
    if (!api_key[0]) {
        strncpy(result->reply, "未识别的指令。请配置 DeepSeek API Key 以启用云端理解。",
                sizeof(result->reply) - 1);
        return ESP_OK;
    }

    char response[4096] = {0};
    err = call_deepseek_api(text, response, sizeof(response));
    if (err == ESP_OK && response[0]) {
        parse_ai_response(response, result);

        /* 将AI回复加入历史 */
        ai_add_message(AI_ROLE_USER, text);
        if (result->reply[0]) {
            ai_add_message(AI_ROLE_ASSISTANT, result->reply);
        }

        ESP_LOGI(TAG, "AI回复: %s (%d条命令)", result->reply, result->cmd_count);
        return ESP_OK;
    }

    strncpy(result->reply, "AI服务暂时不可用", sizeof(result->reply) - 1);
    return ESP_ERR_TIMEOUT;
}

/* ═══════════════════════════════════════════════════
 *  本地意图解析 (离线轻量规则引擎)
 * ═══════════════════════════════════════════════════ */

typedef struct {
    const char *pattern;       /* 关键字 (空格分隔多关键词) */
    device_type_t device;
    ir_action_t   action;
    const char *reply_tmpl;
    int32_t       param;
} intent_rule_t;

static const intent_rule_t intent_rules[] = {
    /* 空调 */
    {"开空调 打开空调",     DEV_AC, IR_ACT_POWER_ON,  "空调已开启",         -1},
    {"关空调 关闭空调",     DEV_AC, IR_ACT_POWER_OFF, "空调已关闭",         -1},
    {"温度 调高 升温",      DEV_AC, IR_ACT_TEMP_UP,   "温度已调高",         -1},
    {"温度 调低 降温",      DEV_AC, IR_ACT_TEMP_DOWN, "温度已调低",         -1},
    {"制冷 凉快",           DEV_AC, IR_ACT_MODE_COOL, "已设为制冷模式",     -1},
    {"制热 暖和 加热",      DEV_AC, IR_ACT_MODE_HEAT, "已设为制热模式",     -1},
    {"自动模式",            DEV_AC, IR_ACT_MODE_AUTO, "已设为自动模式",     -1},
    {"除湿 抽湿",           DEV_AC, IR_ACT_MODE_DRY,  "已设为除湿模式",     -1},
    {"送风 通风",           DEV_AC, IR_ACT_MODE_FAN,  "已设为送风模式",     -1},
    {"扫风 摆风",           DEV_AC, IR_ACT_SWING,     "扫风已开启",         -1},
    {"睡眠模式",            DEV_AC, IR_ACT_SLEEP,     "睡眠模式已开启",     -1},
    {"风小 低风",           DEV_AC, IR_ACT_WIND_LOW,  "风速已调至低档",    -1},
    {"风中 中风",           DEV_AC, IR_ACT_WIND_MID,  "风速已调至中档",    -1},
    {"风大 高风",           DEV_AC, IR_ACT_WIND_HIGH, "风速已调至高档",    -1},

    /* 电视 */
    {"开电视 打开电视",     DEV_TV, IR_ACT_POWER_ON,  "电视已开启",         -1},
    {"关电视 关闭电视",     DEV_TV, IR_ACT_POWER_OFF, "电视已关闭",         -1},
    {"电视声音大 音量大",    DEV_TV, IR_ACT_VOL_UP,    "音量已调高",         -1},
    {"电视声音小 音量小",    DEV_TV, IR_ACT_VOL_DOWN,  "音量已调低",         -1},
    {"静音",                DEV_TV, IR_ACT_VOL_MUTE,  "已静音",             -1},
    {"电视台 频道上",        DEV_TV, IR_ACT_CH_UP,     "频道已切换",         -1},
    {"电视台 频道下",        DEV_TV, IR_ACT_CH_DOWN,   "频道已切换",         -1},

    /* 风扇 */
    {"开风扇 打开风扇",     DEV_FAN, IR_ACT_POWER_ON,  "风扇已开启",         -1},
    {"关风扇 关闭风扇",     DEV_FAN, IR_ACT_POWER_OFF, "风扇已关闭",         -1},
    {"风扇摇头",            DEV_FAN, IR_ACT_SWING,     "风扇摇头已开启",     -1},

    /* 灯光 */
    {"开灯 打开灯",          DEV_LIGHT, IR_ACT_POWER_ON,   "灯光已开启",      -1},
    {"关灯 关闭灯",          DEV_LIGHT, IR_ACT_POWER_OFF,  "灯光已关闭",      -1},
    {"灯光亮 灯亮一点",      DEV_LIGHT, IR_ACT_BRIGHT_UP,  "亮度已增加",      -1},
    {"灯光暗 灯暗一点",      DEV_LIGHT, IR_ACT_BRIGHT_DOWN,"亮度已降低",      -1},
    {"暖光 暖色调",         DEV_LIGHT, IR_ACT_COLOR_WARM, "已切换暖光",      -1},
    {"冷光 冷色调",         DEV_LIGHT, IR_ACT_COLOR_COOL, "已切换冷光",      -1},
};

#define INTENT_RULE_COUNT (sizeof(intent_rules) / sizeof(intent_rules[0]))

/* ── 解析温度数字 ── */
static int parse_temp(const char *text)
{
    const char *p = text;
    while (*p) {
        if (*p >= '1' && *p <= '3') {
            int val = atoi(p);
            if (val >= 16 && val <= 30) return val;
        }
        p++;
    }
    return -1;
}

esp_err_t ai_parse_intent_local(const char *text, ai_result_t *result)
{
    if (!text || !result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(ai_result_t));

    /* 检查唤醒词 */
    if (strstr(text, "小智") || strstr(text, "你好")) {
        strncpy(result->reply, "我在，有什么可以帮您？", sizeof(result->reply) - 1);
        result->is_wake_word = true;
        return ESP_OK;
    }

    if (strstr(text, "再见") || strstr(text, "拜拜")) {
        strncpy(result->reply, "再见！", sizeof(result->reply) - 1);
        return ESP_OK;
    }

    /* 遍历意图规则 */
    for (size_t i = 0; i < INTENT_RULE_COUNT; i++) {
        const char *pattern = intent_rules[i].pattern;
        const char *pat = pattern;

        /* 空格分隔的多关键词 (任一匹配) */
        while (*pat) {
            const char *word_end = strchr(pat, ' ');
            size_t word_len = word_end ? (size_t)(word_end - pat) : strlen(pat);

            if (strncmp(pat, "温度", 6) == 0 && result->cmd_count > 0) {
                /* 温度规则特殊处理: 检查数字 */
            }

            if (strstr(text, pat) || (word_end == NULL && strcmp(pat, text) == 0)) {
                ir_cmd_t cmd = {
                    .device = intent_rules[i].device,
                    .action = intent_rules[i].action,
                    .param  = intent_rules[i].param,
                    .repeat = 0,
                };

                /* 特殊: 温度设置 */
                if (cmd.action == IR_ACT_TEMP_SET || strstr(pattern, "温度")) {
                    int temp = parse_temp(text);
                    if (temp > 0) {
                        cmd.action = IR_ACT_TEMP_SET;
                        cmd.param = temp;
                        snprintf(result->reply, sizeof(result->reply),
                                 "已设置温度为 %d°C", temp);
                        result->cmds[result->cmd_count++] = cmd;
                        goto done;
                    }
                }

                if (result->cmd_count < AI_MAX_CMDS) {
                    result->cmds[result->cmd_count++] = cmd;
                }

                /* 设置回复 */
                if (result->reply[0] == '\0') {
                    strncpy(result->reply, intent_rules[i].reply_tmpl,
                            sizeof(result->reply) - 1);
                }

                goto next;
            }

            if (!word_end) break;
            pat = word_end + 1;
            while (*pat == ' ') pat++;
        }
        next:;
    }

done:
    /* 无匹配 */
    if (result->cmd_count == 0 && result->reply[0] == 0) {
        strncpy(result->reply, "未识别的指令，请尝试: 开空调、关电视、温度26度等",
                sizeof(result->reply) - 1);
    }

    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  状态同步
 * ═══════════════════════════════════════════════════ */

void ai_sync_state(void)
{
    /* 同步 g_state 到 AI 上下文中 (温度、模式等) */
    /* 当前为简化实现，后续可注入到 system prompt */
}