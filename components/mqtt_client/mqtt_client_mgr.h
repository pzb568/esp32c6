#pragma once
/**
 * mqtt_client_mgr.h — MQTT 客户端管理器
 */
#include "esp_err.h"
#include "freertos/event_groups.h"
#include <stdint.h>

typedef void (*mqtt_msg_cb_t)(const char *topic, const char *payload, int len);

esp_err_t mqtt_init(EventGroupHandle_t events, EventBits_t ok_bit, mqtt_msg_cb_t cb);
esp_err_t mqtt_publish(const char *topic, const char *payload, int retain);
esp_err_t mqtt_subscribe(const char *topic, int qos);