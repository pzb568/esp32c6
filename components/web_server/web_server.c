/**
 * web_server.c — HTTP + WebSocket 服务器
 * ESP32-C6 智能家庭终端网页控制台
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "web_server.h"
#include "config.h"

static const char *TAG = "WEB";

/* ── 全局句柄 ────────────────────────────────────── */
static httpd_handle_t server = NULL;

/* ── WebSocket 客户端列表 ──────────────────────────── */
#define WS_MAX_CLIENTS 8
static int ws_fds[WS_MAX_CLIENTS];
static int ws_fd_count = 0;

/* ── WebSocket 回调 ────────────────────────────────── */
static ws_cb_t ws_user_cb = NULL;
static void *ws_user_data = NULL;

/* ═══════════════════════════════════════════════════
 *  SPIFFS 静态文件服务
 * ═══════════════════════════════════════════════════ */

#define WEB_ROOT "/spiffs/www"

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "SPIFFS 挂载/格式化失败");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "SPIFFS 分区未找到");
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %lu/%lu KB已用", (unsigned long)(used/1024), (unsigned long)(total/1024));
    }

    return ESP_OK;
}

static esp_err_t serve_static_file(httpd_req_t *req, const char *uri)
{
    /* 默认页面 */
    if (strcmp(uri, "/") == 0) uri = "/index.html";

    char path[256];
    snprintf(path, sizeof(path), "%s%s", WEB_ROOT, uri);

    /* 检查文件是否存在 */
    FILE *f = fopen(path, "r");
    if (!f) {
        /* 文件不存在 → 返回内嵌 HTML (离线可用) */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<html><body><h1>ESP32-C6 Smart Home</h1><p>SPIFFS 未挂载网页文件。请通过 OTA 上传。</p></body></html>",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* MIME 类型 */
    const char *ext = strrchr(uri, '.');
    if (ext) {
        if (strcmp(ext, ".html") == 0) httpd_resp_set_type(req, "text/html");
        else if (strcmp(ext, ".css") == 0) httpd_resp_set_type(req, "text/css");
        else if (strcmp(ext, ".js") == 0) httpd_resp_set_type(req, "application/javascript");
        else if (strcmp(ext, ".json") == 0) httpd_resp_set_type(req, "application/json");
        else if (strcmp(ext, ".png") == 0) httpd_resp_set_type(req, "image/png");
        else if (strcmp(ext, ".ico") == 0) httpd_resp_set_type(req, "image/x-icon");
        else if (strcmp(ext, ".svg") == 0) httpd_resp_set_type(req, "image/svg+xml");
        else httpd_resp_set_type(req, "application/octet-stream");
    }

    /* 读取文件并发送 */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(fsize + 1);
    if (buf) {
        fread(buf, 1, fsize, f);
        buf[fsize] = '\0';
        httpd_resp_send(req, buf, fsize);
        free(buf);
    }
    fclose(f);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  内嵌 Web 控制面板 HTML (无SPIFFS时后备)
 * ═══════════════════════════════════════════════════ */

static const char *EMBEDDED_HTML =
"<!DOCTYPE html>"
"<html lang=\"zh-CN\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\">"
"<title>ESP32-C6 智能家庭终端</title>"
"<style>"
":root{--bg:#0f172a;--card:#1e293b;--accent:#3b82f6;--green:#22c55e;--red:#ef4444;--yellow:#eab308;--text:#e2e8f0;--muted:#94a3b8;--border:#334155}"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);min-height:100vh}"
".container{max-width:960px;margin:0 auto;padding:16px}"
"header{text-align:center;padding:24px 0 16px}"
"header h1{font-size:1.5rem;font-weight:700;background:linear-gradient(135deg,var(--accent),#8b5cf6);-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
"header p{color:var(--muted);font-size:.85rem;margin-top:4px}"
".status-bar{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:16px}"
".status-card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:14px;text-align:center}"
".status-card .icon{font-size:1.5rem}"
".status-card .label{color:var(--muted);font-size:.75rem;margin:4px 0}"
".status-card .value{font-size:1.1rem;font-weight:600}"
".status-card.on{border-color:var(--green)}"
".status-card.off{border-color:var(--red)}"
".devices{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:12px;margin-bottom:16px}"
".device{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px}"
".device h3{margin-bottom:12px;font-size:1rem;display:flex;align-items:center;gap:8px}"
".device h3 .dot{width:8px;height:8px;border-radius:50%;background:var(--green);display:inline-block}"
".btn-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(70px,1fr));gap:6px}"
".btn{background:var(--border);color:var(--text);border:none;border-radius:8px;padding:8px 10px;font-size:.75rem;cursor:pointer;transition:all .15s;white-space:nowrap}"
".btn:active{transform:scale(.95)}"
".btn:hover{background:#475569}"
".btn.power{background:var(--red);color:#fff}"
".btn.power:hover{background:#dc2626}"
".btn.active{background:var(--accent);color:#fff}"
".section{margin-bottom:20px}"
".section h2{font-size:1rem;margin-bottom:10px;padding-bottom:6px;border-bottom:2px solid var(--accent)}"
"#log{background:var(--card);border-radius:12px;max-height:200px;overflow-y:auto;font-family:'JetBrains Mono',monospace;font-size:.75rem;padding:12px;border:1px solid var(--border)}"
".log-entry{margin:2px 0;color:var(--muted)}"
".log-entry.warn{color:var(--yellow)}"
".log-entry.err{color:var(--red)}"
".log-entry.info{color:var(--green)}"
".temp-display{font-size:2rem;font-weight:700;text-align:center;margin:12px 0}"
".temp-control{display:flex;gap:8px;justify-content:center;align-items:center}"
".temp-control .btn{font-size:1.2rem;padding:6px 16px}"
".mode-btns{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}"
".mode-btn{flex:1;min-width:60px}"
"#learn-status{font-size:.75rem;color:var(--muted);text-align:center;margin-top:8px;min-height:20px}"
"@media(max-width:480px){.status-bar{grid-template-columns:1fr 1fr}.devices{grid-template-columns:1fr}}"
"#ws-indicator{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--red);margin-right:4px}"
"#ws-indicator.on{background:var(--green)}"
"</style>"
"</head>"
"<body>"
"<div class=\"container\">"
"<header>"
"<h1>ESP32-C6 智能家庭终端</h1>"
"<p><span id=\"ws-indicator\"></span>WebSocket <span id=\"ws-text\">断开</span> · DeepSeek AI · NEC红外 · MQTT</p>"
"</header>"
"<div class=\"status-bar\">"
"<div class=\"status-card\" id=\"card-wifi\"><div class=\"icon\">📶</div><div class=\"label\">WiFi</div><div class=\"value\">--</div></div>"
"<div class=\"status-card\" id=\"card-mqtt\"><div class=\"icon\">📡</div><div class=\"label\">MQTT</div><div class=\"value\">--</div></div>"
"<div class=\"status-card\"><div class=\"icon\">⏱️</div><div class=\"label\">运行时间</div><div class=\"value\" id=\"uptime\">--</div></div>"
"<div class=\"status-card\"><div class=\"icon\">💾</div><div class=\"label\">空闲内存</div><div class=\"value\" id=\"heap\">--</div></div>"
"</div>"
"<div class=\"devices\">"
"<div class=\"device\" id=\"dev-ac\">"
"<h3><span class=\"dot\"></span> 空调</h3>"
"<div class=\"temp-display\" id=\"ac-temp\">26°C</div>"
"<div class=\"temp-control\">"
"<button class=\"btn\" onclick=\"sendIR('ac','temp_down')\">−</button>"
"<span id=\"ac-mode-display\">制冷</span>"
"<button class=\"btn\" onclick=\"sendIR('ac','temp_up')\">+</button>"
"</div>"
"<div class=\"mode-btns\">"
"<button class=\"btn mode-btn active\" id=\"btn-cool\" onclick=\"sendIR('ac','mode_cool')\">❄️制冷</button>"
"<button class=\"btn mode-btn\" id=\"btn-heat\" onclick=\"sendIR('ac','mode_heat')\">🔥制热</button>"
"<button class=\"btn mode-btn\" id=\"btn-auto\" onclick=\"sendIR('ac','mode_auto')\">🔄自动</button>"
"<button class=\"btn mode-btn\" id=\"btn-dry\" onclick=\"sendIR('ac','mode_dry')\">💧除湿</button>"
"<button class=\"btn mode-btn\" id=\"btn-fan\" onclick=\"sendIR('ac','mode_fan')\">🌀送风</button>"
"</div>"
"<div class=\"btn-grid\" style=\"margin-top:8px\">"
"<button class=\"btn power\" onclick=\"sendIR('ac','power_on')\">开机</button>"
"<button class=\"btn\" onclick=\"sendIR('ac','power_off')\">关机</button>"
"<button class=\"btn\" onclick=\"sendIR('ac','wind_low')\">低风</button>"
"<button class=\"btn\" onclick=\"sendIR('ac','wind_mid')\">中风</button>"
"<button class=\"btn\" onclick=\"sendIR('ac','wind_high')\">高风</button>"
"<button class=\"btn\" onclick=\"sendIR('ac','swing')\">扫风</button>"
"<button class=\"btn\" onclick=\"sendIR('ac','sleep')\">睡眠</button>"
"</div>"
"</div>"
"<div class=\"device\">"
"<h3><span class=\"dot\"></span> 电视</h3>"
"<div class=\"btn-grid\">"
"<button class=\"btn power\" onclick=\"sendIR('tv','power_on')\">开机</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','power_off')\">关机</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','vol_up')\">音+</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','vol_down')\">音−</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','vol_mute')\">静音</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','ch_up')\">频+</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','ch_down')\">频−</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','ch_prev')\">回看</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','input')\">信号源</button>"
"</div>"
"<div class=\"btn-grid\" style=\"margin-top:6px\">"
"<button class=\"btn\" onclick=\"sendIR('tv','nav_ok')\">确定</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','nav_up')\">上</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','nav_down')\">下</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','nav_left')\">左</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','nav_right')\">右</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','menu')\">菜单</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','back')\">返回</button>"
"<button class=\"btn\" onclick=\"sendIR('tv','home')\">主页</button>"
"</div>"
"</div>"
"<div class=\"device\">"
"<h3><span class=\"dot\"></span> 机顶盒</h3>"
"<div class=\"btn-grid\">"
"<button class=\"btn power\" onclick=\"sendIR('stb','power_on')\">开机</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','power_off')\">关机</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','ch_up')\">频+</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','ch_down')\">频−</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','vol_up')\">音+</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','vol_down')\">音−</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','play')\">播放</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','pause')\">暂停</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','stop')\">停止</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','rec')\">录制</button>"
"</div>"
"<div class=\"btn-grid\" style=\"margin-top:6px\">"
"<button class=\"btn\" onclick=\"sendIR('stb','nav_ok')\">确定</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','nav_up')\">上</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','nav_down')\">下</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','back')\">返回</button>"
"<button class=\"btn\" onclick=\"sendIR('stb','home')\">主页</button>"
"</div>"
"</div>"
"<div class=\"device\">"
"<h3><span class=\"dot\"></span> 风扇</h3>"
"<div class=\"btn-grid\">"
"<button class=\"btn power\" onclick=\"sendIR('fan','power_on')\">开机</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','power_off')\">关机</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','wind_low')\">低</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','wind_mid')\">中</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','wind_high')\">高</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','wind_natural')\">自然风</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','swing')\">摇头</button>"
"<button class=\"btn\" onclick=\"sendIR('fan','sleep')\">睡眠</button>"
"</div>"
"</div>"
"<div class=\"device\">"
"<h3><span class=\"dot\"></span> 灯光</h3>"
"<div class=\"btn-grid\">"
"<button class=\"btn power\" onclick=\"sendIR('light','power_on')\">开灯</button>"
"<button class=\"btn\" onclick=\"sendIR('light','power_off')\">关灯</button>"
"<button class=\"btn\" onclick=\"sendIR('light','bright_up')\">增亮</button>"
"<button class=\"btn\" onclick=\"sendIR('light','bright_down')\">减暗</button>"
"<button class=\"btn\" onclick=\"sendIR('light','color_warm')\">暖白</button>"
"<button class=\"btn\" onclick=\"sendIR('light','color_cool')\">冷白</button>"
"<button class=\"btn\" onclick=\"sendIR('light','color_neutral')\">自然</button>"
"</div>"
"</div>"
"</div>"
"<div class=\"section\">"
"<h2>📟 红外学习</h2>"
"<div style=\"display:flex;gap:8px;flex-wrap:wrap;align-items:center\">"
"<select id=\"learn-dev\" style=\"background:var(--card);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:8px\">"
"<option value=\"ac\">空调</option><option value=\"tv\">电视</option><option value=\"stb\">机顶盒</option>"
"<option value=\"fan\">风扇</option><option value=\"light\">灯光</option>"
"</select>"
"<select id=\"learn-act\" style=\"background:var(--card);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:8px\">"
"<option value=\"power_on\">开机</option><option value=\"power_off\">关机</option>"
"<option value=\"temp_up\">温度+</option><option value=\"temp_down\">温度−</option>"
"<option value=\"wind_low\">低风</option><option value=\"wind_mid\">中风</option><option value=\"wind_high\">高风</option>"
"<option value=\"mode_cool\">制冷</option><option value=\"mode_heat\">制热</option>"
"<option value=\"vol_up\">音量+</option><option value=\"vol_down\">音量−</option>"
"</select>"
"<button class=\"btn active\" onclick=\"startLearn()\">开始学习</button>"
"<button class=\"btn\" onclick=\"stopLearn()\">停止</button>"
"<button class=\"btn\" onclick=\"listLearned()\">查看已学</button>"
"</div>"
"<div id=\"learn-status\">点击「开始学习」后对准IR接收头按下遥控器按钮</div>"
"<div id=\"learn-list\" style=\"margin-top:8px;font-size:.75rem;color:var(--muted)\"></div>"
"</div>"
"<div class=\"section\">"
"<h2>📋 事件日志</h2>"
"<div id=\"log\"><div class=\"log-entry info\">等待 WebSocket 连接...</div></div>"
"</div>"
"</div>"
"<script>"
"var ws, reconnectTimer;"
"function connectWS(){"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=function(){document.getElementById('ws-indicator').classList.add('on');document.getElementById('ws-text').textContent='已连接';addLog('WebSocket 已连接','info');clearTimeout(reconnectTimer)};"
"ws.onclose=function(){document.getElementById('ws-indicator').classList.remove('on');document.getElementById('ws-text').textContent='断开';addLog('WebSocket 断开','warn');reconnectTimer=setTimeout(connectWS,3000)};"
"ws.onerror=function(){addLog('WebSocket 错误','err')};"
"ws.onmessage=function(e){try{var m=JSON.parse(e.data);handleMsg(m)}catch(ex){addLog('WS: '+e.data,'warn')}}"
"}"
"function handleMsg(m){"
"if(m.type==='status'){"
"if(m.wifi_ok){document.getElementById('card-wifi').classList.add('on');document.getElementById('card-wifi').classList.remove('off')}"
"document.getElementById('card-wifi').querySelector('.value').textContent=m.wifi_ok?'在线':'离线';"
"document.getElementById('card-mqtt').querySelector('.value').textContent=m.mqtt_ok?'在线':'离线';"
"document.getElementById('uptime').textContent=m.uptime+'s';"
"document.getElementById('heap').textContent=(m.free_heap/1024).toFixed(0)+' KB';"
"document.getElementById('ac-temp').textContent=m.ac_temp+'°C';"
"document.getElementById('ac-mode-display').textContent=acModeName(m.ac_mode);"
"updateAcModeBtns(m.ac_mode)"
"}"
"else if(m.type==='ir_result'){addLog('IR '+m.dev+'/'+m.act+' → '+(m.ok?'✓':'✗'),m.ok?'info':'err')}"
"else if(m.type==='learn'){"
"if(m.state==='done'){document.getElementById('learn-status').textContent='✓ 学习成功: '+m.addr+'/'+m.cmd;addLog('学习成功: '+m.addr+'/'+m.cmd,'info')}"
"else if(m.state==='ready'){document.getElementById('learn-status').textContent='等待接收红外信号...'}"
"else if(m.state==='timeout'){document.getElementById('learn-status').textContent='✗ 学习超时，请重试';addLog('学习超时','warn')}"
"}"
"else if(m.type==='log'){addLog(m.msg,m.level)}"
"}"
"function sendIR(dev,act,param){"
"param=param||-1;"
"var cmd={device:dev,action:act,param:param};"
"if(ws&&ws.readyState===WebSocket.OPEN){ws.send(JSON.stringify(cmd));addLog('→ '+dev+'/'+act,'')}"
"}"
"function sendCmd(obj){if(ws&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify(obj))}"
"function startLearn(){"
"var dev=document.getElementById('learn-dev').value;"
"var act=document.getElementById('learn-act').value;"
"sendCmd({cmd:'learn_start',dev:dev,act:act,timeout:8000});"
"document.getElementById('learn-status').textContent='学习启动中...请对准IR接收头按下遥控器按钮';"
"}"
"function stopLearn(){sendCmd({cmd:'learn_stop'});document.getElementById('learn-status').textContent='学习已停止'}"
"function listLearned(){sendCmd({cmd:'learn_list'})}"
"function addLog(msg,level){"
"var l=document.getElementById('log');"
"level=level||'';"
"var t=new Date().toLocaleTimeString();"
"l.innerHTML+='<div class=\"log-entry '+level+'\">['+t+'] '+msg+'</div>';"
"if(l.children.length>50)l.removeChild(l.firstChild);"
"l.scrollTop=l.scrollHeight"
"}"
"function acModeName(m){return m==='cool'?'制冷':m==='heat'?'制热':m==='auto'?'自动':m==='dry'?'除湿':m==='fan'?'送风':m||'--'}"
"function updateAcModeBtns(mode){"
"['cool','heat','auto','dry','fan'].forEach(function(m){"
"var b=document.getElementById('btn-'+m);if(b)b.classList.toggle('active',m===mode)})"
"}"
"connectWS();"
"</script>"
"</body>"
"</html>";

/* ═══════════════════════════════════════════════════
 *  HTTP 处理器
 * ═══════════════════════════════════════════════════ */

static esp_err_t root_handler(httpd_req_t *req)
{
    /* 尝试 SPIFFS */
    char path[256];
    snprintf(path, sizeof(path), "%s/index.html", WEB_ROOT);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return serve_static_file(req, "/index.html");
    }

    /* 回退: 内嵌 HTML */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, EMBEDDED_HTML, strlen(EMBEDDED_HTML));
    return ESP_OK;
}

static esp_err_t static_handler(httpd_req_t *req)
{
    return serve_static_file(req, req->uri);
}

/* GET /api/status — 设备状态 */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    char json[512];

    /* 访问全局状态 (extern in config.h) */
    extern sys_state_t g_state;
    extern EventGroupHandle_t g_events;
    extern QueueHandle_t g_ir_queue, g_tts_queue;

    snprintf(json, sizeof(json),
        "{"
        "\"mode\":%d,"
        "\"wifi\":%s,"
        "\"mqtt\":%s,"
        "\"ir_tx\":%lu,"
        "\"ai_req\":%lu,"
        "\"ac_temp\":%ld,"
        "\"ac_mode\":\"%s\","
        "\"ws_clients\":%d,"
        "\"uptime\":%lld"
        "}",
        (int)g_state.mode,
        g_state.wifi_ok ? "true" : "false",
        g_state.mqtt_ok ? "true" : "false",
        (unsigned long)g_state.ir_tx_count,
        (unsigned long)g_state.ai_req_count,
        (long)g_state.ac_temp,
        g_state.ac_mode,
        ws_client_count(),
        (long long)(esp_timer_get_time() / 1000000)
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* POST /api/ir — 发送红外命令 */
static esp_err_t api_ir_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int recv = httpd_req_recv(req, buf, MIN((unsigned)req->content_len, sizeof(buf)-1));
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "空请求");
        return ESP_FAIL;
    }
    buf[recv] = '\0';

    extern QueueHandle_t g_ir_queue;
    ir_cmd_t cmd;
    if (ir_parse_json(buf, &cmd) == ESP_OK) {
        xQueueSend(g_ir_queue, &cmd, pdMS_TO_TICKS(200));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  WebSocket 处理器
 * ═══════════════════════════════════════════════════ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket 握手请求 fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    ws_msg_t msg;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));

    /* 获取消息类型 */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ws_recv_frame failed: %d", ret);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket 关闭 fd=%d", req->handle);
        /* 从客户端列表移除 */
        for (int i = 0; i < ws_fd_count; i++) {
            if (ws_fds[i] == httpd_req_to_sockfd(req)) {
                ws_fds[i] = ws_fds[ws_fd_count - 1];
                ws_fd_count--;
                break;
            }
        }
        if (ws_user_cb) {
            msg.event = WS_EVT_DISCONNECT;
            msg.fd = httpd_req_to_sockfd(req);
            ws_user_cb(&msg, ws_user_data);
        }
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.len > 0) {
        char *data = calloc(1, ws_pkt.len + 1);
        if (data) {
            ws_pkt.payload = (uint8_t *)data;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret == ESP_OK) {
                data[ws_pkt.len] = '\0';

                /* 处理 IR 命令 */
                extern QueueHandle_t g_ir_queue;
                ir_cmd_t cmd;
                if (ir_parse_json(data, &cmd) == ESP_OK) {
                    xQueueSend(g_ir_queue, &cmd, pdMS_TO_TICKS(200));
                    /* 广播 IR 执行结果 (简化: 立即回复) */
                    char result[128];
                    snprintf(result, sizeof(result),
                             "{\"type\":\"ir_sent\",\"dev\":\"%s\",\"act\":\"%s\"}",
                             ir_device_name(cmd.device), ir_action_name(cmd.action));
                    ws_broadcast(result, strlen(result));
                }

                /* 传递给用户回调 */
                if (ws_user_cb) {
                    msg.event = WS_EVT_DATA;
                    msg.data = data;
                    msg.data_len = ws_pkt.len;
                    msg.fd = httpd_req_to_sockfd(req);
                    ws_user_cb(&msg, ws_user_data);
                }
            }
            free(data);
        }
    }

    return ESP_OK;
}

static void ws_add_client(int fd)
{
    for (int i = 0; i < ws_fd_count; i++) {
        if (ws_fds[i] == fd) return; /* 已存在 */
    }
    if (ws_fd_count < WS_MAX_CLIENTS) {
        ws_fds[ws_fd_count++] = fd;
        ESP_LOGI(TAG, "WS客户端+1 (fd=%d, total=%d)", fd, ws_fd_count);
    }
}

/* ═══════════════════════════════════════════════════
 *  WebSocket 广播
 * ═══════════════════════════════════════════════════ */

void ws_broadcast(const char *message, int len)
{
    if (!server) return;
    if (len < 0) len = strlen(message);

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)message,
        .len = (size_t)len,
    };

    /* 向所有注册的客户端发送 */
    for (int i = 0; i < ws_fd_count; i++) {
        httpd_ws_send_frame_async(server, ws_fds[i], &ws_pkt);
    }
}

void ws_broadcast_state(const char *json_state)
{
    ws_broadcast(json_state, -1);
}

void ws_register_cb(ws_cb_t cb, void *user_data)
{
    ws_user_cb = cb;
    ws_user_data = user_data;
}

int ws_client_count(void)
{
    return ws_fd_count;
}

/* ═══════════════════════════════════════════════════
 *  CORS 头
 * ═══════════════════════════════════════════════════ */

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

/* ═══════════════════════════════════════════════════
 *  注册 URI 处理器
 * ═══════════════════════════════════════════════════ */

esp_err_t web_register_api(const char *method, const char *uri, http_handler_t handler)
{
    if (!server) return ESP_ERR_INVALID_STATE;

    httpd_uri_t u = {
        .uri      = uri,
        .method   = HTTP_GET,
        .handler  = (esp_err_t(*)(httpd_req_t*))handler,
        .user_ctx = NULL,
    };

    if (strcmp(method, "POST") == 0) u.method = HTTP_POST;
    else if (strcmp(method, "PUT") == 0) u.method = HTTP_PUT;

    return httpd_register_uri_handler(server, &u);
}

/* ═══════════════════════════════════════════════════
 *  启动/停止
 * ═══════════════════════════════════════════════════ */

esp_err_t web_server_start(void)
{
    ESP_LOGI(TAG, "启动 Web 服务器...");

    /* SPIFFS 挂载 */
    spiffs_init();

    /* HTTPD 配置 */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "启动HTTPD失败");

    /* ── 注册路由 ── */

    /* 首页 */
    httpd_uri_t root_uri = {.uri="/", .method=HTTP_GET, .handler=root_handler};
    httpd_register_uri_handler(server, &root_uri);

    /* 静态文件 (SPIFFS) */
    httpd_uri_t static_uri = {.uri="/assets/*", .method=HTTP_GET, .handler=static_handler};
    httpd_register_uri_handler(server, &static_uri);

    /* REST API */
    httpd_uri_t api_status = {.uri="/api/status", .method=HTTP_GET, .handler=api_status_handler};
    httpd_register_uri_handler(server, &api_status);

    httpd_uri_t api_ir = {.uri="/api/ir", .method=HTTP_POST, .handler=api_ir_handler};
    httpd_register_uri_handler(server, &api_ir);

    /* WebSocket */
    httpd_uri_t ws_uri = {
        .uri      = "/ws",
        .method   = HTTP_GET,
        .handler  = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    httpd_register_uri_handler(server, &ws_uri);

    /* ── WebSocket 状态推送任务 ── */

    ESP_LOGI(TAG, "Web 服务器已启动 (HTTP+WS)");

    /* 打印 IP */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "访问: http://" IPSTR, IP2STR(&ip_info.ip));
    }

    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    return ESP_OK;
}