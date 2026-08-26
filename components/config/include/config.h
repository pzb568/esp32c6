#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_check.h"
/**
 * ================================================================
 *  config.h — 全局配置中心
 *  智能家庭终端 v2.1
 *  硬件: 微雪 ESP32-C6-DEV-KIT-N16 (16MB Flash)
 *  SoC: ESP32-C6-WROOM-1-N16, 160MHz RISC-V, Wi-Fi 6, BLE 5,
 *       IEEE 802.15.4 (Zigbee/Thread)
 * ================================================================
 */

/* ── WiFi ───────────────────────────────────────────────── */
#define CFG_WIFI_SSID        "YourWiFi"
#define CFG_WIFI_PASS        "YourPassword"
#define CFG_WIFI_MAX_RETRY   10

/* ── MQTT Broker (Mosquitto) ────────────────────────────── */
#define CFG_MQTT_URI         "mqtt://192.168.1.10"
#define CFG_MQTT_PORT        1883
#define CFG_MQTT_USER        ""
#define CFG_MQTT_PASS        ""
#define CFG_MQTT_CLIENT_ID   "esp32c6_n16_smarthome"
#define CFG_MQTT_KEEPALIVE   60

#define TOPIC_CMD_IR         "home/ir/cmd"
#define TOPIC_CMD_VOICE      "home/voice/cmd"
#define TOPIC_STATUS         "home/terminal/status"
#define TOPIC_IR_RESULT      "home/ir/result"
#define TOPIC_AI_REPLY       "home/ai/reply"
#define TOPIC_SENSOR         "home/sensor/data"
#define TOPIC_LWT            "home/terminal/lwt"

/* ── DeepSeek AI ────────────────────────────────────────── */
#define CFG_DEEPSEEK_URL     "https://api.deepseek.com/v1/chat/completions"
#define CFG_DEEPSEEK_MODEL   "deepseek-chat"
#define CFG_DEEPSEEK_TOKENS  512
#define CFG_AI_TIMEOUT_MS    12000

/* ── ESP32-C6-DEV-KIT-N16 GPIO ───────────────────────────
 * GPIO8  = 板载 RGB LED，GPIO9 = BOOT，12/13 = USB D-/D+，
 * 16/17 = 板载 UART0，均不作为外设默认引脚。
 * 18~23 在该开发板上作为扩展 GPIO，适合音频外设。
 */

/* INMP441：I2S0 RX simplex */
#define GPIO_MIC_SCK         20
#define GPIO_MIC_WS          22
#define GPIO_MIC_DATA        23

/* MAX98357：I2S0 TX simplex（与 RX 共用 I2S0 控制器） */
#define GPIO_SPK_BCLK        19
#define GPIO_SPK_LRC         21
#define GPIO_SPK_DIN         18
#define GPIO_SPK_SD          10

/* 红外：GPIO2/3 为普通可用 GPIO */
#define GPIO_IR_TX           2
#define GPIO_IR_RX           3

/* 状态指示：避免占用板载 RGB/BOOT/UART/USB */
#define GPIO_LED_WIFI        0
#define GPIO_LED_VOICE       1
#define GPIO_LED_IR          11

/* ── 音频参数 ───────────────────────────────────────────── */
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_BITS           32
#define AUDIO_VAD_THRESHOLD  600
#define AUDIO_VAD_WINDOW_MS  80
#define AUDIO_RECORD_MAX_MS  5000
#define AUDIO_PRE_SILENCE_MS 300

/* ── NVS ────────────────────────────────────────────────── */
#define NVS_NS_CONFIG        "smarthome"
#define NVS_KEY_AI_KEY       "ai_key"
#define NVS_KEY_MQTT_URI     "mqtt_uri"
#define NVS_KEY_IR_CODES     "ir_codes"

/* ── 任务优先级 ─────────────────────────────────────────── */
#define TASK_PRIO_WIFI       5
#define TASK_PRIO_MQTT       4
#define TASK_PRIO_AUDIO      6
#define TASK_PRIO_AI         3
#define TASK_PRIO_IR         5
#define TASK_PRIO_WEB        2

typedef enum {
    DEV_NONE  = 0,
    DEV_AC    = 1,
    DEV_TV    = 2,
    DEV_STB   = 3,
    DEV_FAN   = 4,
    DEV_LIGHT = 5,
    DEV_MAX
} device_type_t;

typedef enum {
    SYS_MODE_IDLE    = 0,
    SYS_MODE_LISTEN  = 1,
    SYS_MODE_RECORD  = 2,
    SYS_MODE_AI_PROC = 3,
    SYS_MODE_IR_SEND = 4,
    SYS_MODE_CONFIG  = 5,
} sys_mode_t;

typedef struct {
    sys_mode_t  mode;
    bool        wifi_ok;
    bool        mqtt_ok;
    bool        ai_key_set;
    uint32_t    ir_tx_count;
    uint32_t    ai_req_count;
    int32_t     ac_temp;
    char        ac_mode[16];
    char        last_cmd[128];
    char        last_reply[256];
    int64_t     boot_time_us;
} sys_state_t;

extern sys_state_t g_state;
