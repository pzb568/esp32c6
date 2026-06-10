#pragma once
/**
 * web_server.h — HTTP + WebSocket 控制面板
 * 功能:
 *   - 静态网页: 设备控制面板 (SPIFFS)
 *   - REST API: GET/POST 设备状态、红外命令、学习、配置
 *   - WebSocket: 实时状态推送、日志流
 *   - 文件上传: OTA固件升级、红外码库导入
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* ── WebSocket 事件类型 ──────────────────────────── */
typedef enum {
    WS_EVT_CONNECT = 0,
    WS_EVT_DISCONNECT,
    WS_EVT_DATA,          /* 收到数据 */
} ws_event_t;

/* ── WebSocket 消息 ──────────────────────────────── */
typedef struct {
    ws_event_t event;
    const char *data;     /* 接收到的消息 (仅 EVT_DATA) */
    int         data_len;
    int         fd;       /* 连接 fd */
} ws_msg_t;

/* ── WebSocket 回调 ──────────────────────────────── */
typedef void (*ws_cb_t)(ws_msg_t *msg, void *user_data);

/* ── HTTP 路由处理器 ──────────────────────────────── */
typedef esp_err_t (*http_handler_t)(void *req, void *resp);

/* ── API ────────────────────────────────────────── */

/* 启动/停止 Web 服务器 */
esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

/* WebSocket 发送 (向所有连接的客户端) */
void      ws_broadcast(const char *message, int len);

/* WebSocket 发送 JSON 状态 */
void      ws_broadcast_state(const char *json_state);

/* WebSocket 注册消息回调 */
void      ws_register_cb(ws_cb_t cb, void *user_data);

/* 获取已连接的 WebSocket 客户端数量 */
int       ws_client_count(void);

/* HTTP REST API 端点注册 (用于扩展) */
esp_err_t web_register_api(const char *method, const char *uri, http_handler_t handler);