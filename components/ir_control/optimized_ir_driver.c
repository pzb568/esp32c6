/**
 * 优化的红外驱动 - NEC协议发射/接收
 * 基于ESP32-C6 RMT驱动
 * 支持NEC红外码发射、接收和学习
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ir_control.h"

static const char *TAG = "IR_DRIVER";

/* ═════════════════════════════════════════════════════════════════════
   RMT 句柄和配置
══════════════════════════════════════════════════════════════════════ */
static rmt_channel_handle_t tx_channel = NULL;  /* RMT发送通道 */
static rmt_channel_handle_t rx_channel = NULL;  /* RMT接收通道 */
static rmt_encoder_handle_t nec_encoder = NULL; /* NEC编码器 */

static ir_learn_state_t g_learn_state = IR_LEARN_IDLE;
static nec_t g_learned_code = {0};

/* ═════════════════════════════════════════════════════════════════════
   NEC协议定义
══════════════════════════════════════════════════════════════════════ */
#define NEC_LEADING_CODE_DURATION_US   (9000)   /* 引导码: 9ms高电平 */
#define NEC_REPEAT_CODE_DURATION_US    (540)    /* 重复码: 0.54ms */
#define NEC_BIT_DURATION_0_US          (562.5)  /* 0位: 0.5625ms */
#define NEC_BIT_DURATION_1_US          (2250)   /* 1位: 2.25ms */

/* ═════════════════════════════════════════════════════════════════════
   RMT发送初始化 - GPIO2
══════════════════════════════════════════════════════════════════════ */
esp_err_t ir_tx_init(void)
{
    ESP_LOGI(TAG, "初始化红外发射通道 (RMT TX, GPIO%d)", GPIO_IR_TX);

    /* RMT TX通道配置 */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = GPIO_IR_TX,           /* GPIO2 - 红外发射 */
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,        /* 1MHz分辨率 (1微秒) */
        .mem_block_symbols = 64,         /* 内存块大小 */
        .trans_queue_depth = 4,           /* 传输队列深度 */
    };

    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &tx_channel),
                       TAG, "创建RMT TX通道失败");

    ESP_LOGI(TAG, "✅ 红外发射通道初始化完成");
    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════
   RMT接收初始化 - GPIO3
══════════════════════════════════════════════════════════════════════ */
esp_err_t ir_rx_init(void)
{
    ESP_LOGI(TAG, "初始化红外接收通道 (RMT RX, GPIO%d)", GPIO_IR_RX);

    /* RMT RX通道配置 */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = GPIO_IR_RX,           /* GPIO3 - 红外接收 */
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,        /* 1MHz分辨率 */
        .mem_block_symbols = 64,         /* 内存块大小 */
    };

    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &rx_channel),
                       TAG, "创建RMT RX通道失败");

    ESP_LOGI(TAG, "✅ 红外接收通道初始化完成");
    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════
   NEC编码函数
══════════════════════════════════════════════════════════════════════ */
static esp_err_t nec_encode_code(nec_t code, rmt_symbol_word_t *symbols, size_t *count)
{
    if (!symbols || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    int idx = 0;

    /* 引导码: 9ms高电平 + 4.5ms低电平 */
    symbols[idx++] = (rmt_symbol_word_t) {
        .duration0 = NEC_LEADING_CODE_DURATION_US,
        .level0 = 1,
        .duration1 = 4500,
        .level1 = 0,
    };

    /* 地址码 (8位) + 地址反码 (8位) */
    uint16_t addr_data = code.addr;
    addr_data = (addr_data << 8) | (~code.addr & 0xFF);

    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (addr_data >> i) & 0x01;
        if (bit) {
            /* 1位: 0.56ms高 + 2.25ms低 */
            symbols[idx++] = (rmt_symbol_word_t) {
                .duration0 = NEC_BIT_DURATION_0_US,
                .level0 = 1,
                .duration1 = NEC_BIT_DURATION_1_US,
                .level1 = 0,
            };
        } else {
            /* 0位: 0.56ms高 + 0.56ms低 */
            symbols[idx++] = (rmt_symbol_word_t) {
                .duration0 = NEC_BIT_DURATION_0_US,
                .level0 = 1,
                .duration1 = NEC_BIT_DURATION_0_US,
                .level1 = 0,
            };
        }
    }

    /* 命令码 (8位) + 命令反码 (8位) */
    uint16_t cmd_data = code.cmd;
    cmd_data = (cmd_data << 8) | (~code.cmd & 0xFF);

    for (int i = 15; i >= 0; i--) {
        uint8_t bit = (cmd_data >> i) & 0x01;
        if (bit) {
            symbols[idx++] = (rmt_symbol_word_t) {
                .duration0 = NEC_BIT_DURATION_0_US,
                .level0 = 1,
                .duration1 = NEC_BIT_DURATION_1_US,
                .level1 = 0,
            };
        } else {
            symbols[idx++] = (rmt_symbol_word_t) {
                .duration0 = NEC_BIT_DURATION_0_US,
                .level0 = 1,
                .duration1 = NEC_BIT_DURATION_0_US,
                .level1 = 0,
            };
        }
    }

    /* 停止位: 0.56ms高电平 */
    symbols[idx++] = (rmt_symbol_word_t) {
        .duration0 = NEC_BIT_DURATION_0_US,
        .level0 = 1,
        .duration1 = 0,
        .level1 = 0,
    };

    *count = idx;
    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════
   发射NEC红外码
══════════════════════════════════════════════════════════════════════ */
esp_err_t ir_send_nec(nec_t code, uint8_t repeat)
{
    if (!tx_channel) {
        ESP_LOGE(TAG, "❌ RMT TX通道未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "📡 发送NEC码: 地址=0x%02X, 命令=0x%02X, 重复=%d次",
             code.addr, code.cmd, repeat);

    /* 编码NEC数据 */
    rmt_symbol_word_t symbols[128];
    size_t symbol_count = 0;
    esp_err_t ret = nec_encode_code(code, symbols, &symbol_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NEC编码失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 发送配置 */
    rmt_transmit_config_t tx_config = {
        .loop_count = repeat,  /* 重复次数 */
    };

    /* 发送 */
    ret = rmt_transmit(tx_channel, nec_encoder, symbols, symbol_count * sizeof(symbols[0]), &tx_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT传输失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 等待发送完成 */
    ret = rmt_tx_wait_all_done(tx_channel, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "等待发送完成超时: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✅ 红外码发送成功");
    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════
   发射红外命令
══════════════════════════════════════════════════════════════════════ */
esp_err_t ir_send_command(const ir_cmd_t *cmd)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "执行命令: 设备=%d, 动作=%d, 参数=%d",
             cmd->device, cmd->action, cmd->param);

    /* 查找预设码 */
    nec_t code = ir_lookup_code(cmd->device, cmd->action);
    if (code.addr == 0 && code.cmd == 0) {
        ESP_LOGW(TAG, "⚠️  未找到预设红外码: 设备=%d, 动作=%d",
                 cmd->device, cmd->action);
        return ESP_ERR_NOT_FOUND;
    }

    return ir_send_nec(code, cmd->repeat);
}

/* ═════════════════════════════════════════════════════════════════════
   红外接收学习功能
══════════════════════════════════════════════════════════════════════ */

/**
 * 开始学习模式
 */
esp_err_t ir_learn_start(device_type_t dev, ir_action_t act, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "📖 开始学习: 设备=%d, 动作=%d, 超时=%dms",
             dev, act, timeout_ms);

    g_learn_state = IR_LEARN_READY;

    /* 启用RMT接收 */
    rmt_enable(rx_channel);

    /* 等待接收... */
    return ESP_OK;
}

/**
 * 停止学习
 */
esp_err_t ir_learn_stop(void)
{
    ESP_LOGI(TAG, "停止学习模式");

    rmt_disable(rx_channel);
    g_learn_state = IR_LEARN_IDLE;

    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════
   红外控制初始化
══════════════════════════════════════════════════════════════════════ */
esp_err_t ir_control_init(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "  初始化红外控制模块");
    ESP_LOGI(TAG, "  协议: NEC (38kHz载波)");
    ESP_LOGI(TAG, "  TX: GPIO%d  RX: GPIO%d", GPIO_IR_TX, GPIO_IR_RX);
    ESP_LOGI(TAG, "═══════════════════════════════════════");

    esp_err_t ret = ESP_OK;

    /* 初始化发射通道 */
    ret |= ir_tx_init();

    /* 初始化接收通道 */
    ret |= ir_rx_init();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ 红外控制模块初始化成功");
    } else {
        ESP_LOGE(TAG, "❌ 红外控制模块初始化失败");
    }

    return ret;
}
