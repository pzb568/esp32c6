/**
 * mqtt_client_mgr.c — MQTT 客户端管理器
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt_client_mgr.h"
#include "config.h"
#include "esp_err.h"

static const char *TAG = "MQTT";
static esp_mqtt_client_handle_t client = NULL;
static EventGroupHandle_t mqtt_events;
static EventBits_t mqtt_ok_bit;
static mqtt_msg_cb_t user_cb = NULL;

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接");
        xEventGroupSetBits(mqtt_events, mqtt_ok_bit);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT 断开");
        xEventGroupClearBits(mqtt_events, mqtt_ok_bit);
        break;
    case MQTT_EVENT_DATA:
        if (user_cb) {
            char topic[128] = {0};
            char payload[512] = {0};
            int tlen = MIN((int)ev->topic_len, (int)sizeof(topic)-1);
            int plen = MIN((int)ev->data_len, (int)sizeof(payload)-1);
            strncpy(topic, ev->topic, tlen);
            strncpy(payload, ev->data, plen);
            user_cb(topic, payload, plen);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 错误");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_init(EventGroupHandle_t events, EventBits_t ok_bit, mqtt_msg_cb_t cb)
{
    mqtt_events = events;
    mqtt_ok_bit = ok_bit;
    user_cb = cb;

    char uri[64];
    snprintf(uri, sizeof(uri), "%s:%d", CFG_MQTT_URI, CFG_MQTT_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .broker.address.port = CFG_MQTT_PORT,
        .credentials.username = CFG_MQTT_USER[0] ? CFG_MQTT_USER : NULL,
        .credentials.client_id = CFG_MQTT_CLIENT_ID,
        .session.keepalive = CFG_MQTT_KEEPALIVE,
        .session.last_will.topic = TOPIC_LWT,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) return ESP_FAIL;

    ESP_RETURN_ON_ERROR(
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                        mqtt_event_handler, NULL),
        TAG, "MQTT事件注册失败");

    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(client), TAG, "MQTT启动失败");

    ESP_LOGI(TAG, "MQTT 初始化: %s", uri);
    return ESP_OK;
}

esp_err_t mqtt_publish(const char *topic, const char *payload, int retain)
{
    if (!client) return ESP_ERR_INVALID_STATE;

    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0,
                                          retain ? 1 : 0, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "MQTT 发布失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mqtt_subscribe(const char *topic, int qos)
{
    if (!client) return ESP_ERR_INVALID_STATE;

    int msg_id = esp_mqtt_client_subscribe(client, topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "MQTT 订阅失败: %s", topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT 订阅: %s (QoS=%d)", topic, qos);
    return ESP_OK;
}