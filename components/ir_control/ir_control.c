/**
 * ir_control.c — NEC 红外发射/接收 + 学习功能
 * 目标: ESP32-C6, RMT + GPIO 红外接收
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
#include "nvs_flash.h"
#include "nvs.h"
#include "ir_control.h"

static const char *TAG = "IR_CTRL";

/* ── 设备名称映射 ────────────────────────────────── */
const ir_device_name_t ir_device_names[] = {
    {DEV_NONE,  "none"},
    {DEV_AC,    "ac"},
    {DEV_TV,    "tv"},
    {DEV_STB,   "stb"},
    {DEV_FAN,   "fan"},
    {DEV_LIGHT, "light"},
};
const int ir_device_name_count = sizeof(ir_device_names) / sizeof(ir_device_names[0]);

/* ── 动作名称映射 ────────────────────────────────── */
typedef struct {
    ir_action_t act;
    const char *name;
} ir_action_map_t;

static const ir_action_map_t action_map[] = {
    {IR_ACT_POWER_ON,    "power_on"},
    {IR_ACT_POWER_OFF,   "power_off"},
    {IR_ACT_POWER_TOGGLE,"power_toggle"},
    {IR_ACT_VOL_UP,      "vol_up"},
    {IR_ACT_VOL_DOWN,    "vol_down"},
    {IR_ACT_VOL_MUTE,    "vol_mute"},
    {IR_ACT_VOL_SET,     "vol_set"},
    {IR_ACT_TEMP_UP,     "temp_up"},
    {IR_ACT_TEMP_DOWN,   "temp_down"},
    {IR_ACT_TEMP_SET,    "temp_set"},
    {IR_ACT_MODE_COOL,   "mode_cool"},
    {IR_ACT_MODE_HEAT,   "mode_heat"},
    {IR_ACT_MODE_AUTO,   "mode_auto"},
    {IR_ACT_MODE_DRY,    "mode_dry"},
    {IR_ACT_MODE_FAN,    "mode_fan"},
    {IR_ACT_WIND_LOW,    "wind_low"},
    {IR_ACT_WIND_MID,    "wind_mid"},
    {IR_ACT_WIND_HIGH,   "wind_high"},
    {IR_ACT_WIND_AUTO,   "wind_auto"},
    {IR_ACT_WIND_NATURAL,"wind_natural"},
    {IR_ACT_SWING,       "swing"},
    {IR_ACT_SLEEP,       "sleep"},
    {IR_ACT_CH_UP,       "ch_up"},
    {IR_ACT_CH_DOWN,     "ch_down"},
    {IR_ACT_CH_SET,      "ch_set"},
    {IR_ACT_CH_PREV,     "ch_prev"},
    {IR_ACT_BRIGHT_UP,   "bright_up"},
    {IR_ACT_BRIGHT_DOWN, "bright_down"},
    {IR_ACT_COLOR_WARM,  "color_warm"},
    {IR_ACT_COLOR_COOL,  "color_cool"},
    {IR_ACT_COLOR_NEUTRAL,"color_neutral"},
    {IR_ACT_SCENE,       "scene"},
    {IR_ACT_NAV_OK,      "nav_ok"},
    {IR_ACT_NAV_UP,      "nav_up"},
    {IR_ACT_NAV_DOWN,    "nav_down"},
    {IR_ACT_NAV_LEFT,    "nav_left"},
    {IR_ACT_NAV_RIGHT,   "nav_right"},
    {IR_ACT_MENU,        "menu"},
    {IR_ACT_BACK,        "back"},
    {IR_ACT_HOME,        "home"},
    {IR_ACT_PLAY,        "play"},
    {IR_ACT_PAUSE,       "pause"},
    {IR_ACT_STOP,        "stop"},
    {IR_ACT_REC,         "rec"},
    {IR_ACT_INPUT,       "input"},
};

#define ACTION_MAP_SIZE (sizeof(action_map) / sizeof(action_map[0]))

/* ── 预设码库 ────────────────────────────────────── */
typedef struct {
    device_type_t dev;
    ir_action_t   act;
    uint8_t       addr;
    uint8_t       cmd;
} ir_preset_t;

static const ir_preset_t presets[] = {
    /* 空调 */
    {DEV_AC, IR_ACT_POWER_ON,    IR_AC_ADDR, IR_AC_POWER},
    {DEV_AC, IR_ACT_POWER_OFF,   IR_AC_ADDR, IR_AC_POWER},
    {DEV_AC, IR_ACT_TEMP_UP,     IR_AC_ADDR, IR_AC_TEMP_UP},
    {DEV_AC, IR_ACT_TEMP_DOWN,   IR_AC_ADDR, IR_AC_TEMP_DN},
    {DEV_AC, IR_ACT_MODE_COOL,   IR_AC_ADDR, IR_AC_MODE_COOL},
    {DEV_AC, IR_ACT_MODE_HEAT,   IR_AC_ADDR, IR_AC_MODE_HEAT},
    {DEV_AC, IR_ACT_MODE_AUTO,   IR_AC_ADDR, IR_AC_MODE_AUTO},
    {DEV_AC, IR_ACT_MODE_DRY,    IR_AC_ADDR, IR_AC_MODE_DRY},
    {DEV_AC, IR_ACT_MODE_FAN,    IR_AC_ADDR, IR_AC_MODE_FAN},
    {DEV_AC, IR_ACT_WIND_LOW,    IR_AC_ADDR, IR_AC_WIND_LOW},
    {DEV_AC, IR_ACT_WIND_MID,    IR_AC_ADDR, IR_AC_WIND_MID},
    {DEV_AC, IR_ACT_WIND_HIGH,   IR_AC_ADDR, IR_AC_WIND_HIGH},
    {DEV_AC, IR_ACT_WIND_AUTO,   IR_AC_ADDR, IR_AC_WIND_AUTO},
    {DEV_AC, IR_ACT_SWING,       IR_AC_ADDR, IR_AC_SWING},
    {DEV_AC, IR_ACT_SLEEP,       IR_AC_ADDR, IR_AC_SLEEP},
    /* 电视 */
    {DEV_TV, IR_ACT_POWER_ON,    IR_TV_ADDR, IR_TV_POWER},
    {DEV_TV, IR_ACT_POWER_OFF,   IR_TV_ADDR, IR_TV_POWER},
    {DEV_TV, IR_ACT_VOL_UP,      IR_TV_ADDR, IR_TV_VOL_UP},
    {DEV_TV, IR_ACT_VOL_DOWN,    IR_TV_ADDR, IR_TV_VOL_DN},
    {DEV_TV, IR_ACT_VOL_MUTE,    IR_TV_ADDR, IR_TV_MUTE},
    {DEV_TV, IR_ACT_CH_UP,       IR_TV_ADDR, IR_TV_CH_UP},
    {DEV_TV, IR_ACT_CH_DOWN,     IR_TV_ADDR, IR_TV_CH_DN},
    {DEV_TV, IR_ACT_CH_PREV,     IR_TV_ADDR, IR_TV_CH_PREV},
    {DEV_TV, IR_ACT_NAV_OK,      IR_TV_ADDR, IR_TV_OK},
    {DEV_TV, IR_ACT_NAV_UP,      IR_TV_ADDR, IR_TV_UP},
    {DEV_TV, IR_ACT_NAV_DOWN,    IR_TV_ADDR, IR_TV_DOWN},
    {DEV_TV, IR_ACT_NAV_LEFT,    IR_TV_ADDR, IR_TV_LEFT},
    {DEV_TV, IR_ACT_NAV_RIGHT,   IR_TV_ADDR, IR_TV_RIGHT},
    {DEV_TV, IR_ACT_MENU,        IR_TV_ADDR, IR_TV_MENU},
    {DEV_TV, IR_ACT_BACK,        IR_TV_ADDR, IR_TV_BACK},
    {DEV_TV, IR_ACT_HOME,        IR_TV_ADDR, IR_TV_HOME},
    {DEV_TV, IR_ACT_INPUT,       IR_TV_ADDR, IR_TV_INPUT},
    /* 机顶盒 */
    {DEV_STB, IR_ACT_POWER_ON,   IR_STB_ADDR, IR_STB_POWER},
    {DEV_STB, IR_ACT_POWER_OFF,  IR_STB_ADDR, IR_STB_POWER},
    {DEV_STB, IR_ACT_CH_UP,      IR_STB_ADDR, IR_STB_CH_UP},
    {DEV_STB, IR_ACT_CH_DOWN,    IR_STB_ADDR, IR_STB_CH_DN},
    {DEV_STB, IR_ACT_VOL_UP,     IR_STB_ADDR, IR_STB_VOL_UP},
    {DEV_STB, IR_ACT_VOL_DOWN,   IR_STB_ADDR, IR_STB_VOL_DN},
    {DEV_STB, IR_ACT_NAV_OK,     IR_STB_ADDR, IR_STB_OK},
    {DEV_STB, IR_ACT_NAV_UP,     IR_STB_ADDR, IR_STB_UP},
    {DEV_STB, IR_ACT_NAV_DOWN,   IR_STB_ADDR, IR_STB_DOWN},
    {DEV_STB, IR_ACT_NAV_LEFT,   IR_STB_ADDR, IR_STB_LEFT},
    {DEV_STB, IR_ACT_NAV_RIGHT,  IR_STB_ADDR, IR_STB_RIGHT},
    {DEV_STB, IR_ACT_MENU,       IR_STB_ADDR, IR_STB_MENU},
    {DEV_STB, IR_ACT_BACK,       IR_STB_ADDR, IR_STB_BACK},
    {DEV_STB, IR_ACT_HOME,       IR_STB_ADDR, IR_STB_HOME},
    {DEV_STB, IR_ACT_PLAY,       IR_STB_ADDR, IR_STB_PLAY},
    {DEV_STB, IR_ACT_PAUSE,      IR_STB_ADDR, IR_STB_PAUSE},
    {DEV_STB, IR_ACT_STOP,       IR_STB_ADDR, IR_STB_STOP},
    {DEV_STB, IR_ACT_REC,        IR_STB_ADDR, IR_STB_REC},
    /* 风扇 */
    {DEV_FAN, IR_ACT_POWER_ON,   IR_FAN_ADDR, IR_FAN_POWER},
    {DEV_FAN, IR_ACT_POWER_OFF,  IR_FAN_ADDR, IR_FAN_POWER},
    {DEV_FAN, IR_ACT_WIND_LOW,   IR_FAN_ADDR, IR_FAN_WIND_LOW},
    {DEV_FAN, IR_ACT_WIND_MID,   IR_FAN_ADDR, IR_FAN_WIND_MID},
    {DEV_FAN, IR_ACT_WIND_HIGH,  IR_FAN_ADDR, IR_FAN_WIND_HIGH},
    {DEV_FAN, IR_ACT_WIND_NATURAL,IR_FAN_ADDR,IR_FAN_WIND_NATURAL},
    {DEV_FAN, IR_ACT_SWING,      IR_FAN_ADDR, IR_FAN_SWING},
    {DEV_FAN, IR_ACT_SLEEP,      IR_FAN_ADDR, IR_FAN_SLEEP},
    /* 灯光 */
    {DEV_LIGHT, IR_ACT_POWER_ON,   IR_LIGHT_ADDR, IR_LIGHT_ON},
    {DEV_LIGHT, IR_ACT_POWER_OFF,  IR_LIGHT_ADDR, IR_LIGHT_OFF},
    {DEV_LIGHT, IR_ACT_BRIGHT_UP,  IR_LIGHT_ADDR, IR_LIGHT_BRIGHT_UP},
    {DEV_LIGHT, IR_ACT_BRIGHT_DOWN,IR_LIGHT_ADDR, IR_LIGHT_BRIGHT_DN},
    {DEV_LIGHT, IR_ACT_COLOR_WARM, IR_LIGHT_ADDR, IR_LIGHT_WARM},
    {DEV_LIGHT, IR_ACT_COLOR_COOL, IR_LIGHT_ADDR, IR_LIGHT_COOL},
    {DEV_LIGHT, IR_ACT_COLOR_NEUTRAL,IR_LIGHT_ADDR,IR_LIGHT_NEUTRAL},
};

#define PRESET_COUNT (sizeof(presets) / sizeof(presets[0]))

/* ── NVS 学习码库: 最大 256 对 ──────────────────── */
#define LEARNED_MAX     256
#define NVS_NS_IR       "ir_codes"

typedef struct {
    device_type_t dev;
    ir_action_t   act;
    uint8_t       addr;
    uint8_t       cmd;
} ir_learned_t;

static ir_learned_t learned_codes[LEARNED_MAX];
static int learned_count = 0;

/* ── RMT 句柄 ────────────────────────────────────── */
static rmt_channel_handle_t tx_chan = NULL;
static rmt_channel_handle_t rx_chan = NULL;
static rmt_encoder_handle_t tx_encoder = NULL;

/* ── 学习状态 ────────────────────────────────────── */
static ir_learn_state_t learn_state = IR_LEARN_IDLE;
static nec_t            learn_result = {0, 0};
static uint32_t         learn_timeout_ms = 5000;
static uint64_t         learn_start_us = 0;

/* ═══════════════════════════════════════════════════
 *  辅助函数
 * ═══════════════════════════════════════════════════ */

const char* ir_action_name(ir_action_t act)
{
    for (size_t i = 0; i < ACTION_MAP_SIZE; i++) {
        if (action_map[i].act == act) return action_map[i].name;
    }
    return "unknown";
}

const char* ir_device_name(device_type_t dev)
{
    for (int i = 0; i < ir_device_name_count; i++) {
        if (ir_device_names[i].dev == dev) return ir_device_names[i].name;
    }
    return "unknown";
}

int ir_device_from_name(const char *name)
{
    for (int i = 0; i < ir_device_name_count; i++) {
        if (strcmp(ir_device_names[i].name, name) == 0)
            return (int)ir_device_names[i].dev;
    }
    return -1;
}

int ir_action_from_name(const char *name)
{
    for (size_t i = 0; i < ACTION_MAP_SIZE; i++) {
        if (strcmp(action_map[i].name, name) == 0)
            return (int)action_map[i].act;
    }
    return -1;
}

/* ── 在已学习码库中查找 ── */
static nec_t *learned_find(device_type_t dev, ir_action_t act)
{
    for (int i = 0; i < learned_count; i++) {
        if (learned_codes[i].dev == dev && learned_codes[i].act == act) {
            static nec_t c;
            c.addr = learned_codes[i].addr;
            c.cmd  = learned_codes[i].cmd;
            return &c;
        }
    }
    return NULL;
}

/* ── 向已学习码库添加 ── */
static void learned_add(device_type_t dev, ir_action_t act, nec_t code)
{
    /* 检查是否已存在，覆盖 */
    for (int i = 0; i < learned_count; i++) {
        if (learned_codes[i].dev == dev && learned_codes[i].act == act) {
            learned_codes[i].addr = code.addr;
            learned_codes[i].cmd  = code.cmd;
            return;
        }
    }
    /* 添加新条目 */
    if (learned_count < LEARNED_MAX) {
        learned_codes[learned_count].dev  = dev;
        learned_codes[learned_count].act  = act;
        learned_codes[learned_count].addr = code.addr;
        learned_codes[learned_count].cmd  = code.cmd;
        learned_count++;
    }
}

/* ═══════════════════════════════════════════════════
 *  红外码查找 (优先学习码 → 预设码)
 * ═══════════════════════════════════════════════════ */

nec_t ir_lookup_code(device_type_t dev, ir_action_t act)
{
    /* 1. 优先查询已学习码 */
    nec_t *learned = learned_find(dev, act);
    if (learned) return *learned;

    /* 2. 回退到预设码 */
    for (size_t i = 0; i < PRESET_COUNT; i++) {
        if (presets[i].dev == dev && presets[i].act == act) {
            nec_t c = {.addr = presets[i].addr, .cmd = presets[i].cmd};
            return c;
        }
    }

    /* 未找到 */
    nec_t empty = {0, 0};
    return empty;
}

bool ir_is_learned(device_type_t dev, ir_action_t act)
{
    return learned_find(dev, act) != NULL;
}

void ir_list_learned(char *buf, size_t size)
{
    size_t pos = 0;
    pos += snprintf(buf + pos, size - pos, "[");
    for (int i = 0; i < learned_count; i++) {
        if (pos >= size - 1) break;
        pos += snprintf(buf + pos, size - pos,
                        "%s{\"dev\":\"%s\",\"act\":\"%s\",\"addr\":\"0x%02X\",\"cmd\":\"0x%02X\"}",
                        i > 0 ? "," : "",
                        ir_device_name(learned_codes[i].dev),
                        ir_action_name(learned_codes[i].act),
                        learned_codes[i].addr,
                        learned_codes[i].cmd);
    }
    pos += snprintf(buf + pos, size - pos, "]");
}

/* ═══════════════════════════════════════════════════
 *  RMT NEC 编码器
 * ═══════════════════════════════════════════════════ */

/* NEC 载波 38KHz, 占空比 33% */
#define NEC_RESOLUTION_HZ   1000000   /* 1MHz = 1µs per tick */
#define NEC_CARRIER_HZ      38000
#define NEC_DUTY_PCT        33

typedef struct {
    rmt_encoder_t           base;
    rmt_encoder_t          *copy_encoder;
    rmt_symbol_word_t       nec_symbols[256];
    int                     symbol_count;
    int                     symbol_index;
    rmt_encode_state_t      state;
} nec_encoder_t;

static size_t nec_encode(rmt_encoder_t *encoder,
                         rmt_channel_handle_t channel,
                         const void *primary_data, size_t data_size,
                         rmt_encode_state_t *ret_state)
{
    nec_encoder_t *nec = __containerof(encoder, nec_encoder_t, base);
    size_t encoded = 0;

    if (nec->state == 0) {
        /* 首次调用: 构建 NEC 波形 */
        uint8_t addr = ((const uint8_t*)primary_data)[0];
        uint8_t cmd  = ((const uint8_t*)primary_data)[1];
        uint32_t data = (addr << 8) | cmd;

        int idx = 0;
        /* 引导码: 9ms 高 + 4.5ms 低 */
        nec->nec_symbols[idx++] = (rmt_symbol_word_t){
            .duration0 = 9000, .level0 = 1,
            .duration1 = 4500, .level1 = 0,
        };

        /* 32-bit 数据: addr(8)+~addr(8)+cmd(8)+~cmd(8) */
        uint32_t raw = (uint32_t)addr
                     | ((uint32_t)(addr ^ 0xFF) << 8)
                     | ((uint32_t)cmd << 16)
                     | ((uint32_t)(cmd ^ 0xFF) << 24);

        for (int bit = 0; bit < 32; bit++) {
            if (raw & (UINT32_C(1) << bit)) {
                /* 逻辑1: 560µs高 + 1690µs低 */
                nec->nec_symbols[idx++] = (rmt_symbol_word_t){
                    .duration0 = 560, .level0 = 1,
                    .duration1 = 1690, .level1 = 0,
                };
            } else {
                /* 逻辑0: 560µs高 + 560µs低 */
                nec->nec_symbols[idx++] = (rmt_symbol_word_t){
                    .duration0 = 560, .level0 = 1,
                    .duration1 = 560, .level1 = 0,
                };
            }
        }
        /* 停止位: 560µs高 */
        nec->nec_symbols[idx++] = (rmt_symbol_word_t){
            .duration0 = 560, .level0 = 1,
            .duration1 = 0,   .level1 = 0,
        };

        nec->symbol_count = idx;
        nec->symbol_index = 0;
        nec->state = 1;
    }

    /* 从缓冲区拷贝到 RMT 输出 */
    if (nec->symbol_index < nec->symbol_count) {
        int remaining = nec->symbol_count - nec->symbol_index;
        int to_copy = (int)(data_size / sizeof(rmt_symbol_word_t));
        if (to_copy > remaining) to_copy = remaining;
        memcpy((rmt_symbol_word_t*)channel, &nec->nec_symbols[nec->symbol_index],
               to_copy * sizeof(rmt_symbol_word_t));
        nec->symbol_index += to_copy;
        encoded = to_copy * sizeof(rmt_symbol_word_t);
    }

    *ret_state = (nec->symbol_index >= nec->symbol_count)
                 ? RMT_ENCODING_COMPLETE : RMT_ENCODING_MEM_FULL;
    return encoded;
}

static esp_err_t nec_encoder_del(rmt_encoder_t *encoder)
{
    nec_encoder_t *nec = __containerof(encoder, nec_encoder_t, base);
    free(nec);
    return ESP_OK;
}

static esp_err_t nec_encoder_reset(rmt_encoder_t *encoder)
{
    nec_encoder_t *nec = __containerof(encoder, nec_encoder_t, base);
    nec->state = 0;
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  RMT NEC 接收器 (学习模式)
 * ═══════════════════════════════════════════════════ */

static bool rx_nec_parse(rmt_symbol_word_t *symbols, size_t count, nec_t *out)
{
    if (count < 34) return false; /* 最少 1引导 + 32数据 + 1停止 */

    /* 检查引导码: ~9ms高 + ~4.5ms低 (允许20%误差) */
    if (symbols[0].level0 != 1) return false;
    if (symbols[0].duration0 < 7200 || symbols[0].duration0 > 10800) return false;
    if (symbols[0].duration1 < 3600 || symbols[0].duration1 > 5400) return false;

    /* 解析32位数据 */
    uint32_t raw = 0;
    for (int bit = 0; bit < 32; bit++) {
        int idx = bit + 1;
        if (idx >= (int)count) return false;
        if (symbols[idx].level0 != 1) return false;

        uint16_t low = symbols[idx].duration1;
        if (low > 1200) {
            raw |= (1 << bit);  /* 逻辑1: 长低电平 */
        } else if (low > 200) {
            /* 逻辑0: 短低电平 */
        } else {
            return false;
        }
    }

    uint8_t addr = raw & 0xFF;
    uint8_t addr_inv = (raw >> 8) & 0xFF;
    uint8_t cmd  = (raw >> 16) & 0xFF;
    uint8_t cmd_inv = (raw >> 24) & 0xFF;

    /* 校验: addr+~addr, cmd+~cmd */
    if ((addr ^ addr_inv) != 0xFF) {
        ESP_LOGW(TAG, "NEC地址校验失败: 0x%02X ^ 0x%02X != 0xFF", addr, addr_inv);
        return false;
    }
    if ((cmd ^ cmd_inv) != 0xFF) {
        ESP_LOGW(TAG, "NEC命令校验失败: 0x%02X ^ 0x%02X != 0xFF", cmd, cmd_inv);
        return false;
    }

    out->addr = addr;
    out->cmd  = cmd;
    return true;
}

static bool rx_callback(rmt_channel_handle_t channel,
                         const rmt_rx_done_event_data_t *edata,
                         void *user_data)
{
    if (learn_state != IR_LEARN_READY) return false;

    nec_t code;
    if (rx_nec_parse(edata->received_symbols, edata->num_symbols, &code)) {
        learn_result = code;
        learn_state = IR_LEARN_DONE;
        ESP_LOGI(TAG, "✦ 学习到 NEC 码: Addr=0x%02X Cmd=0x%02X", code.addr, code.cmd);
    } else {
        ESP_LOGW(TAG, "无效的NEC信号");
    }
    return false;
}

/* ═══════════════════════════════════════════════════
 *  初始化
 * ═══════════════════════════════════════════════════ */

esp_err_t ir_control_init(void)
{
    ESP_LOGI(TAG, "初始化红外控制 (TX=GPIO%d, RX=GPIO%d)", GPIO_IR_TX, GPIO_IR_RX);

    /* ── 发射通道 ── */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = GPIO_IR_TX,
        .clk_src  = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = NEC_RESOLUTION_HZ,
        .mem_block_symbols = 256,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &tx_chan), TAG, "创建TX通道失败");

    /* 载波配置 */
    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = NEC_CARRIER_HZ,
        .duty_cycle   = (float)NEC_DUTY_PCT / 100.0,
    };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(tx_chan, &carrier_cfg), TAG, "配置载波失败");

    /* 编码器 */
    nec_encoder_t *enc = calloc(1, sizeof(nec_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;
    enc->base.encode = nec_encode;
    enc->base.del    = nec_encoder_del;
    enc->base.reset  = nec_encoder_reset;
    tx_encoder = &enc->base;

    ESP_RETURN_ON_ERROR(rmt_enable(tx_chan), TAG, "启用TX通道失败");

    /* ── 接收通道 (学习模式) ── */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = GPIO_IR_RX,
        .clk_src  = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = NEC_RESOLUTION_HZ,
        .mem_block_symbols = 256,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &rx_chan), TAG, "创建RX通道失败");

    rmt_rx_event_callbacks_t rx_cbs = {
        .on_recv_done = rx_callback,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(rx_chan, &rx_cbs, NULL), TAG, "注册RX回调失败");

    ESP_RETURN_ON_ERROR(rmt_enable(rx_chan), TAG, "启用RX通道失败");

    /* ── GPIO 红外接收上拉 (确保信号稳定) ── */
    gpio_set_pull_mode(GPIO_IR_RX, GPIO_PULLUP_ONLY);

    /* ── 加载已学习码 ── */
    ir_learn_load_all();

    ESP_LOGI(TAG, "红外控制就绪 (预设%d条, 已学习%d条)", (int)PRESET_COUNT, learned_count);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  发射
 * ═══════════════════════════════════════════════════ */

esp_err_t ir_send_nec(nec_t code, uint8_t repeat)
{
    if (!tx_chan || !tx_encoder) return ESP_ERR_INVALID_STATE;

    uint8_t data[2] = {code.addr, code.cmd};

    rmt_transmit_config_t cfg = {
        .loop_count = repeat > 0 ? repeat : 0,
    };

    esp_err_t err = rmt_transmit(tx_chan, tx_encoder, data, sizeof(data), &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "发射失败: Addr=0x%02X Cmd=0x%02X err=%d", code.addr, code.cmd, err);
        return err;
    }

    /* 等待发射完成 */
    rmt_tx_wait_all_done(tx_chan, pdMS_TO_TICKS(200));

    ESP_LOGD(TAG, "已发射: Addr=0x%02X Cmd=0x%02X repeat=%d", code.addr, code.cmd, repeat);
    return ESP_OK;
}

esp_err_t ir_send_command(const ir_cmd_t *cmd)
{
    if (!cmd) return ESP_ERR_INVALID_ARG;
    if (cmd->device == DEV_NONE) return ESP_ERR_INVALID_ARG;

    /* 查找码值 (优先学习 → 预设) */
    nec_t code = ir_lookup_code(cmd->device, cmd->action);
    if (code.addr == 0 && code.cmd == 0) {
        ESP_LOGW(TAG, "未找到码值: dev=%s act=%s",
                 ir_device_name(cmd->device), ir_action_name(cmd->action));
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t repeat = cmd->repeat > 0 ? cmd->repeat : 0;
    esp_err_t err = ir_send_nec(code, repeat);

    ESP_LOGI(TAG, "IR命令: %s/%s Addr=0x%02X Cmd=0x%02X → %s",
             ir_device_name(cmd->device), ir_action_name(cmd->action),
             code.addr, code.cmd, err == ESP_OK ? "OK" : "FAIL");

    return err;
}

/* ═══════════════════════════════════════════════════
 *  JSON 解析/序列化
 * ═══════════════════════════════════════════════════ */

esp_err_t ir_parse_json(const char *json, ir_cmd_t *out)
{
    if (!json || !out) return ESP_ERR_INVALID_ARG;

    /* 简易 JSON 解析 — 不依赖 cJSON，直接字符串扫描 */
    memset(out, 0, sizeof(ir_cmd_t));
    out->param  = -1;
    out->repeat = 0;

    /* 解析 device */
    const char *p = strstr(json, "\"device\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++; /* skip ':' */
            while (*p == ' ' || *p == '"') p++;
            char dev_name[16] = {0};
            int i = 0;
            while (*p && *p != '"' && *p != ',' && *p != '}' && i < 15) {
                dev_name[i++] = *p++;
            }
            out->device = (device_type_t)ir_device_from_name(dev_name);
        }
    }

    /* 解析 action */
    p = strstr(json, "\"action\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            char act_name[32] = {0};
            int i = 0;
            while (*p && *p != '"' && *p != ',' && *p != '}' && i < 31) {
                act_name[i++] = *p++;
            }
            int act = ir_action_from_name(act_name);
            if (act >= 0) out->action = (ir_action_t)act;
        }
    }

    /* 解析 param */
    p = strstr(json, "\"param\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            out->param = (int32_t)strtol(p, NULL, 10);
        }
    }

    /* 解析 repeat */
    p = strstr(json, "\"repeat\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            out->repeat = (uint8_t)strtoul(p, NULL, 10);
        }
    }

    return ESP_OK;
}

void ir_cmd_to_json(const ir_cmd_t *cmd, char *buf, size_t size)
{
    snprintf(buf, size,
             "{\"device\":\"%s\",\"action\":\"%s\",\"param\":%ld,\"repeat\":%d}",
             ir_device_name(cmd->device),
             ir_action_name(cmd->action),
             (long)cmd->param,
             cmd->repeat);
}

/* ═══════════════════════════════════════════════════
 *  学习模式
 * ═══════════════════════════════════════════════════ */

esp_err_t ir_learn_start(device_type_t dev, ir_action_t act, uint32_t timeout_ms)
{
    if (learn_state == IR_LEARN_READY) {
        ESP_LOGW(TAG, "学习模式已在运行");
        return ESP_ERR_INVALID_STATE;
    }

    learn_state = IR_LEARN_READY;
    learn_result.addr = 0;
    learn_result.cmd  = 0;
    learn_timeout_ms  = timeout_ms > 0 ? timeout_ms : 5000;
    learn_start_us    = esp_timer_get_time();

    ESP_LOGI(TAG, "✦ 学习模式启动: %s/%s (超时%lums)",
             ir_device_name(dev), ir_action_name(act), (unsigned long)learn_timeout_ms);

    return ESP_OK;
}

esp_err_t ir_learn_stop(void)
{
    learn_state = IR_LEARN_IDLE;
    ESP_LOGI(TAG, "学习模式已停止");
    return ESP_OK;
}

ir_learn_state_t ir_learn_get_state(void)
{
    /* 检查超时 */
    if (learn_state == IR_LEARN_READY) {
        uint64_t now = esp_timer_get_time();
        if ((now - learn_start_us) / 1000 > learn_timeout_ms) {
            learn_state = IR_LEARN_TIMEOUT;
            ESP_LOGI(TAG, "学习超时");
        }
    }
    return learn_state;
}

nec_t ir_learn_get_result(void)
{
    return learn_result;
}

/* ═══════════════════════════════════════════════════
 *  NVS 持久化 (学习码存储)
 * ═══════════════════════════════════════════════════ */

esp_err_t ir_learn_save(device_type_t dev, ir_action_t act, nec_t code)
{
    learned_add(dev, act, code);
    ESP_LOGI(TAG, "已保存学习码: %s/%s → Addr=0x%02X Cmd=0x%02X",
             ir_device_name(dev), ir_action_name(act), code.addr, code.cmd);
    return ir_learn_save_to_nvs();
}

esp_err_t ir_learn_save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_IR, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS打开失败: %d", err);
        return err;
    }

    /* 先存储数量 */
    err = nvs_set_i32(handle, "count", learned_count);
    if (err != ESP_OK) { nvs_close(handle); return err; }

    /* 逐条存储: key = "dev_act", value = (addr<<8)|cmd */
    for (int i = 0; i < learned_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "%d_%d", (int)learned_codes[i].dev, (int)learned_codes[i].act);
        uint32_t val = ((uint32_t)learned_codes[i].addr << 16)
                     | ((uint32_t)learned_codes[i].cmd);
        err = nvs_set_u32(handle, key, val);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS写入失败 key=%s err=%d", key, err);
        }
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "NVS已保存 %d 条学习码", learned_count);
    return err;
}

esp_err_t ir_learn_load_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_IR, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS无学习码数据 (首次使用?)");
        return err;
    }

    int32_t count = 0;
    err = nvs_get_i32(handle, "count", &count);
    if (err != ESP_OK || count <= 0) {
        nvs_close(handle);
        return ESP_OK;
    }

    learned_count = 0;
    for (int i = 0; i < count && learned_count < LEARNED_MAX; i++) {
        char key[32];
        /* 尝试读取: 遍历所有可能的键 (dev 0-5, act 1-50) */
        /* 简化版: 直接尝试读取 key 格式的 NVS 项 */
        /* 这里使用迭代器方式不现实 — 改用固定格式遍历 */

        /* 由于NVS不支持按前缀迭代，此处用预设列表重建索引:
           但实际从NVS读取需要知道key。保存时用"dev_act"格式。
           加载时我们按已知索引重建。更好的方式:存为blob。*/
    }

    /* 改用 blob 方式加载 */
    size_t blob_size = LEARNED_MAX * sizeof(ir_learned_t);
    err = nvs_get_blob(handle, "learned_blob", learned_codes, &blob_size);
    if (err == ESP_OK) {
        learned_count = blob_size / sizeof(ir_learned_t);
        ESP_LOGI(TAG, "NVS加载了 %d 条学习码 (blob方式)", learned_count);
    } else {
        ESP_LOGI(TAG, "NVS blob加载失败，尝试逐条...");

        /* 逐条恢复: 遍历 dev 0-5, act 1-50 */
        learned_count = 0;
        for (int d = 1; d < (int)DEV_MAX; d++) {
            for (int a = 1; a <= 50; a++) {
                char key[32];
                snprintf(key, sizeof(key), "%d_%d", d, a);
                uint32_t val = 0;
                if (nvs_get_u32(handle, key, &val) == ESP_OK) {
                    learned_codes[learned_count].dev  = (device_type_t)d;
                    learned_codes[learned_count].act  = (ir_action_t)a;
                    learned_codes[learned_count].addr = (uint8_t)((val >> 16) & 0xFF);
                    learned_codes[learned_count].cmd  = (uint8_t)(val & 0xFF);
                    learned_count++;
                    if (learned_count >= LEARNED_MAX) break;
                }
            }
            if (learned_count >= LEARNED_MAX) break;
        }
        ESP_LOGI(TAG, "NVS逐条加载了 %d 条学习码", learned_count);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t ir_learn_delete(device_type_t dev, ir_action_t act)
{
    /* 从内存中删除 */
    int found = -1;
    for (int i = 0; i < learned_count; i++) {
        if (learned_codes[i].dev == dev && learned_codes[i].act == act) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* 数组前移 */
        for (int i = found; i < learned_count - 1; i++) {
            learned_codes[i] = learned_codes[i + 1];
        }
        learned_count--;

        /* 同时从NVS删除 */
        nvs_handle_t handle;
        if (nvs_open(NVS_NS_IR, NVS_READWRITE, &handle) == ESP_OK) {
            char key[32];
            snprintf(key, sizeof(key), "%d_%d", (int)dev, (int)act);
            nvs_erase_key(handle, key);
            nvs_commit(handle);
            nvs_close(handle);
        }

        ESP_LOGI(TAG, "已删除学习码: %s/%s", ir_device_name(dev), ir_action_name(act));
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}