#pragma once
/**
 * ai_engine.h — DeepSeek AI 引擎
 * 功能: ASR语音识别 + NLU意图解析 + 多轮对话
 */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "ir_control.h"

/* ── AI 处理结果 ─────────────────────────────────── */
#define AI_MAX_CMDS     8
#define AI_MAX_REPLY    256
#define AI_MAX_HISTORY  20   /* 多轮对话历史条数 */

typedef struct {
    ir_cmd_t cmds[AI_MAX_CMDS];
    int      cmd_count;
    char     reply[AI_MAX_REPLY];
    char     raw_json[1024];   /* AI原始返回 */
    bool     is_wake_word;     /* 是否是唤醒词响应 */
} ai_result_t;

/* ── 对话角色 ────────────────────────────────────── */
typedef enum {
    AI_ROLE_SYSTEM = 0,
    AI_ROLE_USER,
    AI_ROLE_ASSISTANT,
} ai_role_t;

/* ── 对话消息 ────────────────────────────────────── */
typedef struct {
    ai_role_t role;
    char      content[512];
} ai_msg_t;

/* ── API ────────────────────────────────────────── */

/* 初始化 */
esp_err_t ai_engine_init(void);

/* 语音处理 (WAV → 结构化指令) */
esp_err_t ai_process_audio(const uint8_t *wav, size_t wav_len, ai_result_t *result);

/* 文本处理 (文字指令 → 结构化指令) */
esp_err_t ai_process_text(const char *text, ai_result_t *result);

/* 多轮对话管理 */
void      ai_clear_history(void);
void      ai_add_message(ai_role_t role, const char *content);
int       ai_history_count(void);

/* 意图解析 (本地快速解析，无需云端) */
esp_err_t ai_parse_intent_local(const char *text, ai_result_t *result);

/* 状态更新 (同步内部状态与 g_state) */
void      ai_sync_state(void);