#pragma once
/**
 * wifi_sta.h — WiFi Station 模式
 */
#include "esp_err.h"
#include "freertos/event_groups.h"

esp_err_t wifi_init(EventGroupHandle_t events, EventBits_t ok_bit);