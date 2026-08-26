#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_check.h"
/**
 * ================================================================
 *  config.h — 全局配置中心
 *  智能家庭终端 v2.0
 *  硬件: 微雪 ESP32-C6-DEV-KIT-N16 (16MB Flash, WiFi6/BLE5)
 * ================================================================
 */

/* ── WiFi ───────────────────────────────────────────────── */
#define CFG_WIFI_SSID        "YourWiFi"
#define CFG_WIFI_PASS        "YourPassword"
#define CFG_WIFI_MAX_RETRY   10

/* ── MQTT Broker (Mosquitto) ────────────────────────────── */
#define CFG_MQTT_URI         "mqtt://192.168.1.10"   /* 本地Mosquitto地址 */
#define CFG_MQTT_PORT        1883
#define CFG_MQTT_USER        ""                       /* 若开启认证填写 */
#define CFG_MQTT_PASS        ""
#define CFG_MQTT_CLIENT_ID   "esp32c6_smarthome"
#define CFG_MQTT_KEEPALIVE   60

/* MQTT Topic 规划 */
#define TOPIC_CMD_IR         "home/ir/cmd"            /* 订阅: 接收红外指令 */
#define TOPIC_CMD_VOICE      "home/voice/cmd"         /* 订阅: 接收文字指令 */
#define TOPIC_STATUS         "home/terminal/status"   /* 发布: 设备状态 */
#define TOPIC_IR_RESULT      "home/ir/result"         /* 发布: 红外执行结果 */
#define TOPIC_AI_REPLY       "home/ai/reply"          /* 发布: AI回复文本 */
#define TOPIC_SENSOR         "home/sensor/data"       /* 发布: 传感器数据 */
#define TOPIC_LWT            "home/terminal/lwt"      /* Last Will Testament */

/* ── DeepSeek AI ────────────────────────────────────────── */
#define CFG_DEEPSEEK_URL     "https://api.deepseek.com/v1/chat/completions"
#define CFG_DEEPSEEK_MODEL   "deepseek-chat"
#define CFG_DEEPSEEK_TOKENS  512
#define CFG_AI_TIMEOUT_MS    12000

/* ── GPIO 分配 (ESP32-C6) ──────────────────────────────── */
/* INMP441 麦克风 I2S (I2S_NUM_0 RX) */
#define GPIO_MIC_SCK         16
#define GPIO_MIC_WS          17
#define GPIO_MIC_DATA        18

/* MAX98357 功放 I2S (ESP32-C6 I2S_NUM_0 TX simplex) */
#define GPIO_SPK_BCLK        6
#define GPIO_SPK_LRC         7
#define GPIO_SPK_DIN         15
#define GPIO_SPK_SD          5    /* 低=静音 */

/* 红外发射/接收 RMT */
#define GPIO_IR_TX           2    /* 经S8050三极管驱动 */
#define GPIO_IR_RX           3    /* NEC解码模块OUT */

/* 状态指示 LED */
#define GPIO_LED_WIFI        8    /* WiFi状态 */
#define GPIO_LED_VOICE       9    /* 语音活动 */
#define GPIO_LED_IR          10   /* 红外发射 */

/* ── 音频参数 ───────────────────────────────────────────── */
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_BITS           32            /* INMP441帧宽 */
#define AUDIO_VAD_THRESHOLD  600           /* 静音检测阈值 */
#define AUDIO_VAD_WINDOW_MS  80
#define AUDIO_RECORD_MAX_MS  5000          /* 最长录音5秒 */
#define AUDIO_PRE_SILENCE_MS 300           /* 前置静音去除 */

/* ── NVS 命名空间 ───────────────────────────────────────── */
#define NVS_NS_CONFIG        "smarthome"
#define NVS_KEY_AI_KEY       "ai_key"
#define NVS_KEY_MQTT_URI     "mqtt_uri"
#define NVS_KEY_IR_CODES     "ir_codes"    /* 学习到的红外码 */

/* ── 系统任务优先级 ─────────────────────────────────────── */
#define TASK_PRIO_WIFI       5
#define TASK_PRIO_MQTT       4
#define TASK_PRIO_AUDIO      6
#define TASK_PRIO_AI         3
#define TASK_PRIO_IR         5
#define TASK_PRIO_WEB        2

/* ── 设备类型枚举 ───────────────────────────────────────── */
typedef enum {
    DEV_NONE  = 0,
    DEV_AC    = 1,   /* 空调 */
    DEV_TV    = 2,   /* 电视 */
    DEV_STB   = 3,   /* 机顶盒 */
    DEV_FAN   = 4,   /* 风扇 */
    DEV_LIGHT = 5,   /* 灯光/射灯 */
    DEV_MAX
} device_type_t;

/* ── 系统运行模式 ───────────────────────────────────────── */
typedef enum {
    SYS_MODE_IDLE    = 0,
    SYS_MODE_LISTEN  = 1,   /* VAD检测 */
    SYS_MODE_RECORD  = 2,   /* 录音中 */
    SYS_MODE_AI_PROC = 3,   /* AI处理中 */
    SYS_MODE_IR_SEND = 4,   /* 红外发射中 */
    SYS_MODE_CONFIG  = 5,   /* 配置模式 */
} sys_mode_t;

/* ── 全局状态结构 ───────────────────────────────────────── */
typedef struct {
    sys_mode_t  mode;
    bool        wifi_ok;
    bool        mqtt_ok;
    bool        ai_key_set;
    uint32_t    ir_tx_count;    /* 红外发射总次数 */
    uint32_t    ai_req_count;   /* AI请求总次数 */
    int32_t     ac_temp;        /* 当前设定温度 */
    char        ac_mode[16];    /* 当前空调模式 */
    char        last_cmd[128];  /* 最后一条指令 */
    char        last_reply[256];/* 最后一条AI回复 */
    int64_t     boot_time_us;
} sys_state_t;

extern sys_state_t g_state;
