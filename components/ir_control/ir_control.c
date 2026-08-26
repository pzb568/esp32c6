#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_check.h"
#include "ir_control.h"

static const char *TAG = "IR_CTRL";
#define IR_UART UART_NUM_1
#define IR_UART_BAUD 9600
#define IR_UART_RX_BUF 1024
#define IR_UART_TX_BUF 256
#define IR_UART_ADDR 0xA1
#define IR_OP_TX 0xF1
#define LEARNED_MAX 256
#define NVS_NS_IR "ir_codes"
#define NVS_KEY_BLOB "learned_blob"

typedef struct { device_type_t dev; ir_action_t act; nec_t code; } learned_item_t;
static learned_item_t learned[LEARNED_MAX];
static size_t learned_count;
static volatile ir_learn_state_t learn_state = IR_LEARN_IDLE;
static nec_t learn_result;
static uint64_t learn_deadline_us;
static TaskHandle_t rx_task_handle;
static bool uart_ready;

const ir_device_name_t ir_device_names[] = {
    {DEV_NONE,"none"},{DEV_AC,"ac"},{DEV_TV,"tv"},{DEV_STB,"stb"},{DEV_FAN,"fan"},{DEV_LIGHT,"light"},
};
const int ir_device_name_count = sizeof(ir_device_names)/sizeof(ir_device_names[0]);

typedef struct { ir_action_t act; const char *name; } action_map_t;
static const action_map_t action_map[] = {
    {IR_ACT_POWER_ON,"power_on"},{IR_ACT_POWER_OFF,"power_off"},{IR_ACT_POWER_TOGGLE,"power_toggle"},
    {IR_ACT_VOL_UP,"vol_up"},{IR_ACT_VOL_DOWN,"vol_down"},{IR_ACT_VOL_MUTE,"vol_mute"},{IR_ACT_VOL_SET,"vol_set"},
    {IR_ACT_TEMP_UP,"temp_up"},{IR_ACT_TEMP_DOWN,"temp_down"},{IR_ACT_TEMP_SET,"temp_set"},
    {IR_ACT_MODE_COOL,"mode_cool"},{IR_ACT_MODE_HEAT,"mode_heat"},{IR_ACT_MODE_AUTO,"mode_auto"},{IR_ACT_MODE_DRY,"mode_dry"},{IR_ACT_MODE_FAN,"mode_fan"},
    {IR_ACT_WIND_LOW,"wind_low"},{IR_ACT_WIND_MID,"wind_mid"},{IR_ACT_WIND_HIGH,"wind_high"},{IR_ACT_WIND_AUTO,"wind_auto"},{IR_ACT_WIND_NATURAL,"wind_natural"},
    {IR_ACT_SWING,"swing"},{IR_ACT_SLEEP,"sleep"},{IR_ACT_CH_UP,"ch_up"},{IR_ACT_CH_DOWN,"ch_down"},{IR_ACT_CH_SET,"ch_set"},{IR_ACT_CH_PREV,"ch_prev"},
    {IR_ACT_BRIGHT_UP,"bright_up"},{IR_ACT_BRIGHT_DOWN,"bright_down"},{IR_ACT_COLOR_WARM,"color_warm"},{IR_ACT_COLOR_COOL,"color_cool"},{IR_ACT_COLOR_NEUTRAL,"color_neutral"},
    {IR_ACT_SCENE,"scene"},{IR_ACT_NAV_OK,"nav_ok"},{IR_ACT_NAV_UP,"nav_up"},{IR_ACT_NAV_DOWN,"nav_down"},{IR_ACT_NAV_LEFT,"nav_left"},{IR_ACT_NAV_RIGHT,"nav_right"},
    {IR_ACT_MENU,"menu"},{IR_ACT_BACK,"back"},{IR_ACT_HOME,"home"},{IR_ACT_PLAY,"play"},{IR_ACT_PAUSE,"pause"},{IR_ACT_STOP,"stop"},{IR_ACT_REC,"rec"},{IR_ACT_INPUT,"input"},
};
#define ACTION_MAP_SIZE (sizeof(action_map)/sizeof(action_map[0]))

typedef struct { device_type_t dev; ir_action_t act; uint8_t user_hi; uint8_t cmd; } preset_t;
static const preset_t presets[] = {
    {DEV_AC,IR_ACT_POWER_ON,IR_AC_ADDR,IR_AC_POWER},{DEV_AC,IR_ACT_POWER_OFF,IR_AC_ADDR,IR_AC_POWER},{DEV_AC,IR_ACT_TEMP_UP,IR_AC_ADDR,IR_AC_TEMP_UP},{DEV_AC,IR_ACT_TEMP_DOWN,IR_AC_ADDR,IR_AC_TEMP_DN},
    {DEV_AC,IR_ACT_MODE_COOL,IR_AC_ADDR,IR_AC_MODE_COOL},{DEV_AC,IR_ACT_MODE_HEAT,IR_AC_ADDR,IR_AC_MODE_HEAT},{DEV_AC,IR_ACT_MODE_AUTO,IR_AC_ADDR,IR_AC_MODE_AUTO},{DEV_AC,IR_ACT_MODE_DRY,IR_AC_ADDR,IR_AC_MODE_DRY},{DEV_AC,IR_ACT_MODE_FAN,IR_AC_ADDR,IR_AC_MODE_FAN},
    {DEV_AC,IR_ACT_WIND_LOW,IR_AC_ADDR,IR_AC_WIND_LOW},{DEV_AC,IR_ACT_WIND_MID,IR_AC_ADDR,IR_AC_WIND_MID},{DEV_AC,IR_ACT_WIND_HIGH,IR_AC_ADDR,IR_AC_WIND_HIGH},{DEV_AC,IR_ACT_WIND_AUTO,IR_AC_ADDR,IR_AC_WIND_AUTO},{DEV_AC,IR_ACT_SWING,IR_AC_ADDR,IR_AC_SWING},{DEV_AC,IR_ACT_SLEEP,IR_AC_ADDR,IR_AC_SLEEP},
    {DEV_TV,IR_ACT_POWER_ON,IR_TV_ADDR,IR_TV_POWER},{DEV_TV,IR_ACT_POWER_OFF,IR_TV_ADDR,IR_TV_POWER},{DEV_TV,IR_ACT_VOL_UP,IR_TV_ADDR,IR_TV_VOL_UP},{DEV_TV,IR_ACT_VOL_DOWN,IR_TV_ADDR,IR_TV_VOL_DN},{DEV_TV,IR_ACT_VOL_MUTE,IR_TV_ADDR,IR_TV_MUTE},
    {DEV_TV,IR_ACT_CH_UP,IR_TV_ADDR,IR_TV_CH_UP},{DEV_TV,IR_ACT_CH_DOWN,IR_TV_ADDR,IR_TV_CH_DN},{DEV_TV,IR_ACT_CH_PREV,IR_TV_ADDR,IR_TV_CH_PREV},{DEV_TV,IR_ACT_NAV_OK,IR_TV_ADDR,IR_TV_OK},{DEV_TV,IR_ACT_NAV_UP,IR_TV_ADDR,IR_TV_UP},{DEV_TV,IR_ACT_NAV_DOWN,IR_TV_ADDR,IR_TV_DOWN},
    {DEV_TV,IR_ACT_NAV_LEFT,IR_TV_ADDR,IR_TV_LEFT},{DEV_TV,IR_ACT_NAV_RIGHT,IR_TV_ADDR,IR_TV_RIGHT},{DEV_TV,IR_ACT_MENU,IR_TV_ADDR,IR_TV_MENU},{DEV_TV,IR_ACT_BACK,IR_TV_ADDR,IR_TV_BACK},{DEV_TV,IR_ACT_HOME,IR_TV_ADDR,IR_TV_HOME},{DEV_TV,IR_ACT_INPUT,IR_TV_ADDR,IR_TV_INPUT},
};
#define PRESET_COUNT (sizeof(presets)/sizeof(presets[0]))

static int find_learned(device_type_t dev, ir_action_t act) {
    for(size_t i=0;i<learned_count;i++) if(learned[i].dev==dev && learned[i].act==act) return (int)i;
    return -1;
}
static void learned_add(device_type_t dev, ir_action_t act, nec_t code) {
    int i=find_learned(dev,act);
    if(i>=0){learned[i].code=code;return;}
    if(learned_count<LEARNED_MAX) learned[learned_count++]=(learned_item_t){dev,act,code};
}

const char *ir_action_name(ir_action_t act){for(size_t i=0;i<ACTION_MAP_SIZE;i++)if(action_map[i].act==act)return action_map[i].name;return "unknown";}
const char *ir_device_name(device_type_t dev){for(int i=0;i<ir_device_name_count;i++)if(ir_device_names[i].dev==dev)return ir_device_names[i].name;return "unknown";}
int ir_device_from_name(const char *name){if(!name)return -1;for(int i=0;i<ir_device_name_count;i++)if(!strcmp(name,ir_device_names[i].name))return ir_device_names[i].dev;return -1;}
int ir_action_from_name(const char *name){if(!name)return -1;for(size_t i=0;i<ACTION_MAP_SIZE;i++)if(!strcmp(name,action_map[i].name))return action_map[i].act;return -1;}

nec_t ir_lookup_code(device_type_t dev, ir_action_t act){
    int i=find_learned(dev,act); if(i>=0)return learned[i].code;
    for(size_t n=0;n<PRESET_COUNT;n++)if(presets[n].dev==dev&&presets[n].act==act)return (nec_t){presets[n].user_hi,0,presets[n].cmd};
    return (nec_t){0,0,0};
}
bool ir_is_learned(device_type_t dev, ir_action_t act){return find_learned(dev,act)>=0;}

void ir_list_learned(char *buf,size_t size){
    if(!buf||!size)return;size_t p=0;int n=snprintf(buf,size,"[");if(n<0)return;p=(size_t)n;
    for(size_t i=0;i<learned_count&&p<size;i++){n=snprintf(buf+p,size-p,"%s{\"dev\":\"%s\",\"act\":\"%s\",\"user_hi\":\"0x%02X\",\"user_lo\":\"0x%02X\",\"cmd\":\"0x%02X\"}",i?",":"",ir_device_name(learned[i].dev),ir_action_name(learned[i].act),learned[i].code.user_hi,learned[i].code.user_lo,learned[i].code.cmd);if(n<0)break;p+=(size_t)n;}
    if(p<size)snprintf(buf+p,size-p,"]");else buf[size-1]='\0';
}

esp_err_t ir_learn_save_to_nvs(void){
    nvs_handle_t h;esp_err_t e=nvs_open(NVS_NS_IR,NVS_READWRITE,&h);if(e!=ESP_OK)return e;
    e=nvs_set_blob(h,"learned_blob",learned,learned_count*sizeof(learned_item_t));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e;
}
esp_err_t ir_learn_load_all(void){
    learned_count=0;memset(learned,0,sizeof(learned));nvs_handle_t h;
    if(nvs_open(NVS_NS_IR,NVS_READONLY,&h)==ESP_OK){size_t s=sizeof(learned);if(nvs_get_blob(h,"learned_blob",learned,&s)==ESP_OK)learned_count=s/sizeof(learned_item_t);nvs_close(h);}
    if(learned_count>LEARNED_MAX)learned_count=LEARNED_MAX;return ESP_OK;
}

static void process_rx(nec_t code){
    if(learn_state==IR_LEARN_READY){learn_result=code;learn_state=IR_LEARN_DONE;ESP_LOGI(TAG,"learned NEC %02X %02X %02X",code.user_hi,code.user_lo,code.cmd);}
    else ESP_LOGI(TAG,"NEC RX %02X %02X %02X",code.user_hi,code.user_lo,code.cmd);
}
static void ir_uart_rx_task(void *arg){
    uint8_t b[3];size_t have=0;
    while(1){
        int n=uart_read_bytes(IR_UART,&b[have],1,pdMS_TO_TICKS(100));
        if(n==1){if(++have==3){process_rx((nec_t){b[0],b[1],b[2]});have=0;}}
        if(learn_state==IR_LEARN_READY&&esp_timer_get_time()>learn_deadline_us)learn_state=IR_LEARN_TIMEOUT;
    }
}

esp_err_t ir_control_init(void){
    const uart_config_t cfg={.baud_rate=IR_UART_BAUD,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_RETURN_ON_ERROR(uart_param_config(IR_UART,&cfg),TAG,"UART config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(IR_UART,GPIO_IR_TX,GPIO_IR_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE),TAG,"UART pin failed");
    esp_err_t e=uart_driver_install(IR_UART,IR_UART_RX_BUF,IR_UART_TX_BUF,0,NULL,0);if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE)return e;
    uart_ready=true;uart_flush_input(IR_UART);ir_learn_load_all();
    if(xTaskCreate(ir_uart_rx_task,"ir_uart_rx",4096,NULL,TASK_PRIO_IR,&rx_task_handle)!=pdPASS)return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG,"NEC UART ready: UART1 TX=GPIO%d RX=GPIO%d 9600 8N1",GPIO_IR_TX,GPIO_IR_RX);return ESP_OK;
}

static esp_err_t wait_ack(uint32_t timeout_ms){
    uint8_t b;int64_t end=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    while(esp_timer_get_time()<end){if(uart_read_bytes(IR_UART,&b,1,pdMS_TO_TICKS(20))==1&&b==IR_OP_TX)return ESP_OK;}return ESP_ERR_TIMEOUT;
}

esp_err_t ir_send_nec(nec_t code,uint8_t repeat){
    if(!uart_ready)return ESP_ERR_INVALID_STATE;
    uint8_t frame[5]={IR_UART_ADDR,IR_OP_TX,code.user_hi,code.user_lo,code.cmd};
    uint8_t count=(uint8_t)(repeat+1);
    for(uint8_t i=0;i<count;i++){
        uart_flush_input(IR_UART);if(uart_write_bytes(IR_UART,(const char*)frame,sizeof(frame))!=(int)sizeof(frame))return ESP_FAIL;
        esp_err_t e=wait_ack(250);if(e!=ESP_OK&&i==count-1)return e;vTaskDelay(pdMS_TO_TICKS(20));
    }return ESP_OK;
}

esp_err_t ir_send_command(const ir_cmd_t *cmd){
    if(!cmd||cmd->device==DEV_NONE)return ESP_ERR_INVALID_ARG;nec_t code=ir_lookup_code(cmd->device,cmd->action);
    if(code.user_hi==0&&code.user_lo==0&&code.cmd==0)return ESP_ERR_NOT_FOUND;return ir_send_nec(code,cmd->repeat);
}

static const char *json_value(const char *json,const char *key){char pat[32];snprintf(pat,sizeof(pat),"\"%s\"",key);const char*p=strstr(json,pat);if(!p)return NULL;p=strchr(p,':');if(!p)return NULL;p++;while(*p==' '||*p=='\t'||*p=='\"')p++;return p;}
esp_err_t ir_parse_json(const char *json,ir_cmd_t*out){
    if(!json||!out)return ESP_ERR_INVALID_ARG;memset(out,0,sizeof(*out));out->param=-1;const char*p;
    p=json_value(json,"device");if(p){char s[16]={0};sscanf(p,"%15[^\"]",s);int v=ir_device_from_name(s);if(v>=0)out->device=v;}
    p=json_value(json,"action");if(p){char s[32]={0};sscanf(p,"%31[^\"]",s);int v=ir_action_from_name(s);if(v>=0)out->action=v;}
    p=json_value(json,"param");if(p)out->param=strtol(p,NULL,10);p=json_value(json,"repeat");if(p)out->repeat=(uint8_t)strtoul(p,NULL,10);return ESP_OK;
}
void ir_cmd_to_json(const ir_cmd_t*cmd,char*buf,size_t size){if(cmd&&buf&&size)snprintf(buf,size,"{\"device\":\"%s\",\"action\":\"%s\",\"param\":%ld,\"repeat\":%u}",ir_device_name(cmd->device),ir_action_name(cmd->action),(long)cmd->param,cmd->repeat);}

esp_err_t ir_learn_start(device_type_t dev,ir_action_t act,uint32_t timeout_ms){
    (void)dev;(void)act;if(learn_state==IR_LEARN_READY||!uart_ready)return ESP_ERR_INVALID_STATE;learn_result=(nec_t){0,0,0};learn_state=IR_LEARN_READY;learn_deadline_us=esp_timer_get_time()+(uint64_t)(timeout_ms?timeout_ms:5000)*1000ULL;uart_flush_input(IR_UART);return ESP_OK;
}
esp_err_t ir_learn_stop(void){learn_state=IR_LEARN_IDLE;return ESP_OK;}
ir_learn_state_t ir_learn_get_state(void){if(learn_state==IR_LEARN_READY&&esp_timer_get_time()>learn_deadline_us)learn_state=IR_LEARN_TIMEOUT;return learn_state;}
nec_t ir_learn_get_result(void){return learn_result;}
esp_err_t ir_learn_save(device_type_t dev,ir_action_t act,nec_t code){learned_add(dev,act,code);return ir_learn_save_to_nvs();}
esp_err_t ir_learn_delete(device_type_t dev,ir_action_t act){int i=find_learned(dev,act);if(i<0)return ESP_ERR_NOT_FOUND;for(size_t n=i;n+1<learned_count;n++)learned[n]=learned[n+1];learned_count--;return ir_learn_save_to_nvs();}
