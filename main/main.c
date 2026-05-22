/**
 * main.c — 智能家庭终端 v2.0 主程序
 *
 * 系统架构:
 *   WiFi连接 → MQTT订阅 → 语音VAD → 录音
 *       ↓
 *   DeepSeek AI (ASR+NLU+意图识别)
 *       ↓
 *   NEC红外发射 + MQTT状态上报 + TTS播报
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "config.h"
#include "wifi_sta.h"
#include "mqtt_client_mgr.h"
#include "audio_pipeline.h"
#include "ai_engine.h"
#include "ir_control.h"
#include "web_server.h"

static const char *TAG = "MAIN";

/* ── 全局状态 ──────────────────────────────────── */
sys_state_t g_state = {
    .mode        = SYS_MODE_IDLE,
    .ac_temp     = 26,
    .ac_mode     = "cool",
};

/* ── 事件组 ────────────────────────────────────── */
EventGroupHandle_t g_events;
#define EV_WIFI_OK    BIT0
#define EV_MQTT_OK    BIT1
#define EV_VAD_TRIG   BIT2
#define EV_AI_DONE    BIT3

/* ── 命令队列 (MQTT/Web → IR驱动) ─────────────── */
QueueHandle_t g_ir_queue;   /* ir_cmd_t, 深度10 */
QueueHandle_t g_tts_queue;  /* char[256], 深度5 */

/* ── LED 辅助 ──────────────────────────────────── */
static void led_init(void)
{
    uint64_t mask = (1ULL<<GPIO_LED_WIFI)|(1ULL<<GPIO_LED_VOICE)|(1ULL<<GPIO_LED_IR);
    gpio_config_t c = {.pin_bit_mask=mask,.mode=GPIO_MODE_OUTPUT};
    gpio_config(&c);
}
static inline void led_set(int gpio, bool on) { gpio_set_level(gpio, on ? 1 : 0); }

/* ── 语音处理任务 ──────────────────────────────── */
static void voice_task(void *arg)
{
    ESP_LOGI(TAG, "语音任务启动");
    while (1) {
        /* 等待WiFi就绪 */
        xEventGroupWaitBits(g_events, EV_WIFI_OK, pdFALSE, pdTRUE, portMAX_DELAY);

        /* VAD 检测 */
        if (g_state.mode == SYS_MODE_IDLE && audio_detect_vad()) {
            g_state.mode = SYS_MODE_RECORD;
            led_set(GPIO_LED_VOICE, true);
            ESP_LOGI(TAG, "✦ VAD触发，开始录音");

            /* MQTT 通知录音开始 */
            mqtt_publish(TOPIC_STATUS, "{\"event\":\"recording\"}", 0);

            /* 录音 */
            uint8_t *wav = NULL;
            size_t   wav_len = 0;
            esp_err_t err = audio_record(&wav, &wav_len, AUDIO_RECORD_MAX_MS);

            if (err == ESP_OK && wav_len > 0) {
                g_state.mode = SYS_MODE_AI_PROC;
                mqtt_publish(TOPIC_STATUS, "{\"event\":\"ai_processing\"}", 0);

                /* AI处理: 语音 → JSON指令 */
                ai_result_t result = {0};
                ai_process_audio(wav, wav_len, &result);
                free(wav);

                if (result.reply[0]) {
                    /* 保存最后回复 */
                    strncpy(g_state.last_reply, result.reply, sizeof(g_state.last_reply)-1);

                    /* MQTT 上报AI回复 */
                    char pub[320];
                    snprintf(pub, sizeof(pub),
                             "{\"reply\":\"%s\",\"cmd_count\":%d}",
                             result.reply, result.cmd_count);
                    mqtt_publish(TOPIC_AI_REPLY, pub, 0);

                    /* TTS播报 */
                    char tts[256];
                    strncpy(tts, result.reply, sizeof(tts)-1);
                    xQueueSend(g_tts_queue, tts, 0);

                    /* 推送红外命令 */
                    for (int i = 0; i < result.cmd_count; i++) {
                        xQueueSend(g_ir_queue, &result.cmds[i], pdMS_TO_TICKS(100));
                    }
                }
            } else {
                free(wav);
            }

            g_state.mode = SYS_MODE_IDLE;
            led_set(GPIO_LED_VOICE, false);
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* ── 红外发射任务 ──────────────────────────────── */
static void ir_task(void *arg)
{
    ESP_LOGI(TAG, "红外任务启动");
    ir_cmd_t cmd;
    while (1) {
        if (xQueueReceive(g_ir_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            g_state.mode = SYS_MODE_IR_SEND;
            led_set(GPIO_LED_IR, true);

            esp_err_t err = ir_send_command(&cmd);

            /* MQTT 上报执行结果 */
            char result[128];
            snprintf(result, sizeof(result),
                     "{\"dev\":%d,\"act\":%d,\"param\":%d,\"ok\":%s}",
                     cmd.device, cmd.action, cmd.param,
                     err == ESP_OK ? "true" : "false");
            mqtt_publish(TOPIC_IR_RESULT, result, 0);

            g_state.ir_tx_count++;
            led_set(GPIO_LED_IR, false);

            if (g_state.mode == SYS_MODE_IR_SEND)
                g_state.mode = SYS_MODE_IDLE;

            vTaskDelay(pdMS_TO_TICKS(80)); /* 命令间隔 */
        }
    }
}

/* ── TTS 播报任务 ──────────────────────────────── */
static void tts_task(void *arg)
{
    char text[256];
    while (1) {
        if (xQueueReceive(g_tts_queue, text, portMAX_DELAY) == pdTRUE) {
            audio_play_tts(text);
        }
    }
}

/* ── MQTT 消息回调 ─────────────────────────────── */
void on_mqtt_message(const char *topic, const char *payload, int len)
{
    ESP_LOGI(TAG, "MQTT← [%s] %.*s", topic, len, payload);

    if (strcmp(topic, TOPIC_CMD_IR) == 0) {
        /* 直接红外命令: {"device":"ac","action":"power_on","param":-1} */
        ir_cmd_t cmd;
        if (ir_parse_json(payload, &cmd) == ESP_OK) {
            xQueueSend(g_ir_queue, &cmd, pdMS_TO_TICKS(200));
        }
    } else if (strcmp(topic, TOPIC_CMD_VOICE) == 0) {
        /* 文字指令走AI */
        ai_result_t result = {0};
        ai_process_text(payload, &result);

        if (result.reply[0]) {
            mqtt_publish(TOPIC_AI_REPLY, result.reply, 0);
            char tts[256];
            strncpy(tts, result.reply, sizeof(tts)-1);
            xQueueSend(g_tts_queue, tts, 0);
        }
        for (int i = 0; i < result.cmd_count; i++) {
            xQueueSend(g_ir_queue, &result.cmds[i], pdMS_TO_TICKS(100));
        }
    }
}

/* ── 状态上报任务 (每30秒) ─────────────────────── */
static void status_report_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!g_state.mqtt_ok) continue;

        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"uptime\":%lld,\"free_heap\":%lu,"
            "\"ir_tx\":%lu,\"ai_req\":%lu,"
            "\"wifi\":%s,\"mqtt\":%s,"
            "\"ac_temp\":%ld,\"ac_mode\":\"%s\"}",
            esp_timer_get_time()/1000000,
            esp_get_free_heap_size(),
            g_state.ir_tx_count,
            g_state.ai_req_count,
            g_state.wifi_ok ? "true" : "false",
            g_state.mqtt_ok ? "true" : "false",
            g_state.ac_temp,
            g_state.ac_mode);
        mqtt_publish(TOPIC_STATUS, buf, 1); /* retain=1 */
    }
}

/* ── app_main ──────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  智能家庭终端 v2.0  ESP32-C6          ║");
    ESP_LOGI(TAG, "║  DeepSeek AI · MQTT · NEC-IR          ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");

    g_state.boot_time_us = esp_timer_get_time();

    /* NVS 初始化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 事件组 & 队列 */
    g_events   = xEventGroupCreate();
    g_ir_queue = xQueueCreate(10, sizeof(ir_cmd_t));
    g_tts_queue= xQueueCreate(5,  256);

    /* LED */
    led_init();

    /* ── 子系统初始化 ── */
    ESP_LOGI(TAG, "[1/6] WiFi...");
    wifi_init(g_events, EV_WIFI_OK);

    ESP_LOGI(TAG, "[2/6] 等待WiFi...");
    xEventGroupWaitBits(g_events, EV_WIFI_OK, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    led_set(GPIO_LED_WIFI, true);

    ESP_LOGI(TAG, "[3/6] MQTT...");
    mqtt_init(g_events, EV_MQTT_OK, on_mqtt_message);
    /* 订阅控制主题 */
    mqtt_subscribe(TOPIC_CMD_IR,    1);
    mqtt_subscribe(TOPIC_CMD_VOICE, 1);

    ESP_LOGI(TAG, "[4/6] 音频管道...");
    audio_pipeline_init();

    ESP_LOGI(TAG, "[5/6] 红外控制...");
    ir_control_init();

    ESP_LOGI(TAG, "[6/6] AI引擎 & Web服务器...");
    ai_engine_init();
    web_server_start();

    /* ── 创建任务 ── */
    xTaskCreate(voice_task,         "voice",   8192, NULL, TASK_PRIO_AUDIO, NULL);
    xTaskCreate(ir_task,            "ir_tx",   4096, NULL, TASK_PRIO_IR,    NULL);
    xTaskCreate(tts_task,           "tts",     4096, NULL, TASK_PRIO_AUDIO, NULL);
    xTaskCreate(status_report_task, "status",  3072, NULL, TASK_PRIO_MQTT,  NULL);

    /* 启动提示 */
    vTaskDelay(pdMS_TO_TICKS(500));
    xQueueSend(g_tts_queue, "智能家庭终端已就绪", 0);
    mqtt_publish(TOPIC_STATUS, "{\"event\":\"boot_ok\"}", 1);

    ESP_LOGI(TAG, "✅ 系统就绪");
}
