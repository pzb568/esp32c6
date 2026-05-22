#pragma once
/**
 * ir_control.h — NEC 红外全设备控制
 * 支持: 空调 / 电视 / 机顶盒 / 风扇 / 灯光
 *
 * 红外发射电路 (ESP32-C6 GPIO2):
 *   GPIO2 → 33Ω → S8050(B极)
 *   S8050(C极) → 红外LED(阳极)
 *   S8050(E极) + LED(阴极) → GND
 *   LED需串联 10Ω 限流电阻
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* ── NEC 红外码 (按品牌分区) ──────────────────────────────────
 * 重要: 以下为示例码，需用逻辑分析仪/IR学习APP采集真实遥控器码值
 * 采集方法: 对准INMP441旁的IR接收头，调用 ir_learn_start() 后按遥控器
 * ─────────────────────────────────────────────────────────── */

/* ════ 空调 (美的/通用，协议为8位地址+8位命令) ════ */
#define IR_AC_ADDR           0xB2   /* 美的空调地址 */
#define IR_AC_POWER          0x48
#define IR_AC_TEMP_UP        0x10
#define IR_AC_TEMP_DN        0x11
#define IR_AC_MODE_COOL      0x18   /* 制冷 */
#define IR_AC_MODE_HEAT      0x58   /* 制热 */
#define IR_AC_MODE_AUTO      0x98   /* 自动 */
#define IR_AC_MODE_DRY       0xD8   /* 除湿 */
#define IR_AC_MODE_FAN       0xE8   /* 送风 */
#define IR_AC_WIND_LOW       0x02
#define IR_AC_WIND_MID       0x42
#define IR_AC_WIND_HIGH      0x22
#define IR_AC_WIND_AUTO      0x62
#define IR_AC_SWING          0x1A   /* 扫风 */
#define IR_AC_SLEEP          0x4C   /* 睡眠 */
#define IR_AC_TIMER          0x28   /* 定时 */

/* ════ 电视 (三星/通用) ════ */
#define IR_TV_ADDR           0x07
#define IR_TV_POWER          0x02
#define IR_TV_VOL_UP         0x07
#define IR_TV_VOL_DN         0x0B
#define IR_TV_MUTE           0x0F
#define IR_TV_CH_UP          0x12
#define IR_TV_CH_DN          0x13
#define IR_TV_CH_PREV        0x14   /* 上一频道 */
#define IR_TV_MENU           0x1A
#define IR_TV_OK             0x68
#define IR_TV_BACK           0x58
#define IR_TV_UP             0x60
#define IR_TV_DOWN           0x61
#define IR_TV_LEFT           0x65
#define IR_TV_RIGHT          0x62
#define IR_TV_HOME           0x79
#define IR_TV_INPUT          0x0E   /* 切换输入源 */
#define IR_TV_NUM(n)         (0x04 + (n))  /* 数字键0-9 */

/* ════ 机顶盒 (华为/中兴通用) ════ */
#define IR_STB_ADDR          0xFB
#define IR_STB_POWER         0x08
#define IR_STB_CH_UP         0x40
#define IR_STB_CH_DN         0x41
#define IR_STB_VOL_UP        0x00
#define IR_STB_VOL_DN        0x01
#define IR_STB_OK            0x44
#define IR_STB_UP            0x46
#define IR_STB_DOWN          0x47
#define IR_STB_LEFT          0x48
#define IR_STB_RIGHT         0x49
#define IR_STB_MENU          0x54
#define IR_STB_BACK          0x55
#define IR_STB_HOME          0x03
#define IR_STB_PLAY          0x0C
#define IR_STB_PAUSE         0x0D
#define IR_STB_STOP          0x0E
#define IR_STB_REC           0x17

/* ════ 风扇 (通用遥控) ════ */
#define IR_FAN_ADDR          0xFE
#define IR_FAN_POWER         0x02
#define IR_FAN_WIND_LOW      0x04
#define IR_FAN_WIND_MID      0x05
#define IR_FAN_WIND_HIGH     0x06
#define IR_FAN_WIND_NATURAL  0x07   /* 自然风 */
#define IR_FAN_TIMER         0x09
#define IR_FAN_SWING         0x0A   /* 摇头 */
#define IR_FAN_SLEEP         0x0B   /* 睡眠风 */

/* ════ 灯光/射灯 (通用) ════ */
#define IR_LIGHT_ADDR        0xEF
#define IR_LIGHT_ON          0x00
#define IR_LIGHT_OFF         0x01
#define IR_LIGHT_BRIGHT_UP   0x02
#define IR_LIGHT_BRIGHT_DN   0x03
#define IR_LIGHT_WARM        0x10   /* 暖白 */
#define IR_LIGHT_COOL        0x11   /* 冷白 */
#define IR_LIGHT_NEUTRAL     0x12   /* 自然光 */
#define IR_LIGHT_SCENE1      0x20   /* 场景1: 阅读 */
#define IR_LIGHT_SCENE2      0x21   /* 场景2: 影院 */
#define IR_LIGHT_SCENE3      0x22   /* 场景3: 浪漫 */

/* ── 动作枚举 (AI→红外 映射) ──────────────────── */
typedef enum {
    IR_ACT_POWER_ON = 1,  IR_ACT_POWER_OFF,  IR_ACT_POWER_TOGGLE,
    IR_ACT_VOL_UP,        IR_ACT_VOL_DOWN,   IR_ACT_VOL_MUTE,
    IR_ACT_VOL_SET,       /* param = 目标音量 (重复按键实现) */
    IR_ACT_TEMP_UP,       IR_ACT_TEMP_DOWN,  IR_ACT_TEMP_SET,
    IR_ACT_MODE_COOL,     IR_ACT_MODE_HEAT,  IR_ACT_MODE_AUTO,
    IR_ACT_MODE_DRY,      IR_ACT_MODE_FAN,
    IR_ACT_WIND_LOW,      IR_ACT_WIND_MID,   IR_ACT_WIND_HIGH,
    IR_ACT_WIND_AUTO,     IR_ACT_WIND_NATURAL,
    IR_ACT_SWING,         IR_ACT_SLEEP,
    IR_ACT_CH_UP,         IR_ACT_CH_DOWN,    IR_ACT_CH_SET,
    IR_ACT_CH_PREV,
    IR_ACT_BRIGHT_UP,     IR_ACT_BRIGHT_DOWN,
    IR_ACT_COLOR_WARM,    IR_ACT_COLOR_COOL, IR_ACT_COLOR_NEUTRAL,
    IR_ACT_SCENE,         /* param = 场景编号 */
    IR_ACT_NAV_OK,        IR_ACT_NAV_UP,     IR_ACT_NAV_DOWN,
    IR_ACT_NAV_LEFT,      IR_ACT_NAV_RIGHT,
    IR_ACT_MENU,          IR_ACT_BACK,       IR_ACT_HOME,
    IR_ACT_PLAY,          IR_ACT_PAUSE,      IR_ACT_STOP,
    IR_ACT_REC,           IR_ACT_INPUT,
} ir_action_t;

/* ── 命令结构体 ────────────────────────────────── */
typedef struct {
    device_type_t device;
    ir_action_t   action;
    int32_t       param;   /* -1 = 不适用 */
    uint8_t       repeat;  /* 发射次数 */
} ir_cmd_t;

/* ── NEC 原始码 ────────────────────────────────── */
typedef struct {
    uint8_t addr;
    uint8_t cmd;
} nec_t;

/* ── API ───────────────────────────────────────── */
esp_err_t  ir_control_init(void);
esp_err_t  ir_send_command(const ir_cmd_t *cmd);
esp_err_t  ir_send_nec(nec_t code, uint8_t repeat);
esp_err_t  ir_parse_json(const char *json, ir_cmd_t *out);

/* 学习模式 */
esp_err_t  ir_learn_start(device_type_t dev, ir_action_t act);
nec_t      ir_learn_get_result(void);
esp_err_t  ir_learn_save(device_type_t dev, ir_action_t act, nec_t code);
esp_err_t  ir_learn_load_all(void);  /* 从NVS加载自定义码 */
