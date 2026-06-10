#pragma once
/**
 * ir_control.h — NEC 红外全设备控制 + 学习功能
 * 支持: 空调 / 电视 / 机顶盒 / 风扇 / 灯光
 *
 * 红外发射电路 (ESP32-C6 GPIO2):
 *   GPIO2 → 33Ω → S8050(B极)
 *   S8050(C极) → 红外LED(阳极)
 *   S8050(E极) + LED(阴极) → GND
 *   LED需串联 10Ω 限流电阻
 *
 * 红外接收电路 (ESP32-C6 GPIO3):
 *   IR接收头 OUT → GPIO3 (内部上拉)
 *   IR接收头 VCC → 3.3V
 *   IR接收头 GND → GND
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* ── NEC 红外码 (按品牌分区) ──────────────────────────────────
 * 以下为预设码值，可通过学习模式动态替换
 * 采集方法: 对准IR接收头，调用 ir_learn_start() 后按遥控器
 * ─────────────────────────────────────────────────────────── */

/* ════ 空调 (美的/通用) ════ */
#define IR_AC_ADDR           0xB2
#define IR_AC_POWER          0x48
#define IR_AC_TEMP_UP        0x10
#define IR_AC_TEMP_DN        0x11
#define IR_AC_MODE_COOL      0x18
#define IR_AC_MODE_HEAT      0x58
#define IR_AC_MODE_AUTO      0x98
#define IR_AC_MODE_DRY       0xD8
#define IR_AC_MODE_FAN       0xE8
#define IR_AC_WIND_LOW       0x02
#define IR_AC_WIND_MID       0x42
#define IR_AC_WIND_HIGH      0x22
#define IR_AC_WIND_AUTO      0x62
#define IR_AC_SWING          0x1A
#define IR_AC_SLEEP          0x4C
#define IR_AC_TIMER          0x28

/* ════ 电视 (三星/通用) ════ */
#define IR_TV_ADDR           0x07
#define IR_TV_POWER          0x02
#define IR_TV_VOL_UP         0x07
#define IR_TV_VOL_DN         0x0B
#define IR_TV_MUTE           0x0F
#define IR_TV_CH_UP          0x12
#define IR_TV_CH_DN          0x13
#define IR_TV_CH_PREV        0x14
#define IR_TV_MENU           0x1A
#define IR_TV_OK             0x68
#define IR_TV_BACK           0x58
#define IR_TV_UP             0x60
#define IR_TV_DOWN           0x61
#define IR_TV_LEFT           0x65
#define IR_TV_RIGHT          0x62
#define IR_TV_HOME           0x79
#define IR_TV_INPUT          0x0E
#define IR_TV_NUM(n)         (0x04 + (n))

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
#define IR_FAN_WIND_NATURAL  0x07
#define IR_FAN_TIMER         0x09
#define IR_FAN_SWING         0x0A
#define IR_FAN_SLEEP         0x0B

/* ════ 灯光/射灯 (通用) ════ */
#define IR_LIGHT_ADDR        0xEF
#define IR_LIGHT_ON          0x00
#define IR_LIGHT_OFF         0x01
#define IR_LIGHT_BRIGHT_UP   0x02
#define IR_LIGHT_BRIGHT_DN   0x03
#define IR_LIGHT_WARM        0x10
#define IR_LIGHT_COOL        0x11
#define IR_LIGHT_NEUTRAL     0x12
#define IR_LIGHT_SCENE1      0x20
#define IR_LIGHT_SCENE2      0x21
#define IR_LIGHT_SCENE3      0x22

/* ── 动作枚举 (AI→红外 映射) ──────────────────── */
typedef enum {
    IR_ACT_POWER_ON = 1,  IR_ACT_POWER_OFF,  IR_ACT_POWER_TOGGLE,
    IR_ACT_VOL_UP,        IR_ACT_VOL_DOWN,   IR_ACT_VOL_MUTE,
    IR_ACT_VOL_SET,
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
    IR_ACT_SCENE,
    IR_ACT_NAV_OK,        IR_ACT_NAV_UP,     IR_ACT_NAV_DOWN,
    IR_ACT_NAV_LEFT,      IR_ACT_NAV_RIGHT,
    IR_ACT_MENU,          IR_ACT_BACK,       IR_ACT_HOME,
    IR_ACT_PLAY,          IR_ACT_PAUSE,      IR_ACT_STOP,
    IR_ACT_REC,           IR_ACT_INPUT,
} ir_action_t;

/* ── 学习状态枚举 ──────────────────────────────── */
typedef enum {
    IR_LEARN_IDLE   = 0,  /* 空闲 */
    IR_LEARN_READY  = 1,  /* 等待接收 */
    IR_LEARN_DONE   = 2,  /* 已收到 */
    IR_LEARN_TIMEOUT= 3,  /* 超时 */
} ir_learn_state_t;

/* ── 命令结构体 ────────────────────────────────── */
typedef struct {
    device_type_t device;
    ir_action_t   action;
    int32_t       param;
    uint8_t       repeat;
} ir_cmd_t;

/* ── NEC 原始码 ────────────────────────────────── */
typedef struct {
    uint8_t addr;
    uint8_t cmd;
} nec_t;

/* ── 学习回调 ──────────────────────────────────── */
typedef void (*ir_learn_cb_t)(ir_learn_state_t state, nec_t code, void *user_data);

/* ── 设备名称映射 ──────────────────────────────── */
typedef struct {
    device_type_t dev;
    const char   *name;
} ir_device_name_t;

extern const ir_device_name_t ir_device_names[];
extern const int ir_device_name_count;

/* ── API ───────────────────────────────────────── */
esp_err_t  ir_control_init(void);
esp_err_t  ir_send_command(const ir_cmd_t *cmd);
esp_err_t  ir_send_nec(nec_t code, uint8_t repeat);

/* JSON 解析 */
esp_err_t  ir_parse_json(const char *json, ir_cmd_t *out);
void       ir_cmd_to_json(const ir_cmd_t *cmd, char *buf, size_t size);
const char* ir_action_name(ir_action_t act);
const char* ir_device_name(device_type_t dev);
int        ir_device_from_name(const char *name);
int        ir_action_from_name(const char *name);

/* 学习模式 */
esp_err_t  ir_learn_start(device_type_t dev, ir_action_t act, uint32_t timeout_ms);
esp_err_t  ir_learn_stop(void);
ir_learn_state_t ir_learn_get_state(void);
nec_t      ir_learn_get_result(void);
esp_err_t  ir_learn_save(device_type_t dev, ir_action_t act, nec_t code);
esp_err_t  ir_learn_save_to_nvs(void);
esp_err_t  ir_learn_load_all(void);
esp_err_t  ir_learn_delete(device_type_t dev, ir_action_t act);

/* 码库查询 */
nec_t      ir_lookup_code(device_type_t dev, ir_action_t act);
bool       ir_is_learned(device_type_t dev, ir_action_t act);
void       ir_list_learned(char *buf, size_t size);