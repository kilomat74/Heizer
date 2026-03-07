#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "display_tft.h"

#define WIFI_SSID              "FritzEG2,4"
#define WIFI_PASS              "1snickers"
#define MQTT_BROKER_URI_DEFAULT "mqtt://192.168.2.200:1885"
#define MQTT_TOPIC_STATE_DEFAULT "heizer/room/state"
#define MQTT_TOPIC_CMD_DEFAULT   "heizer/room/cmd"
#define MQTT_TOPIC_POWER_DEFAULT "zigbee/0/0c4314fffe52af5b/load_power"
#define MQTT_TOPIC_TIMER_DEFAULT "heizer/room/timer/config"
#define MQTT_TOPIC_SWITCH_DEFAULT "zigbee/0/0c4314fffe52af5b/state"
#define MQTT_TOPIC_STATUS_DEFAULT "heizer/room/status"
#define MQTT_TOPIC_CONFIG_DEFAULT "heizer/room/config"
#define MQTT_TOPIC_REQUEST_DEFAULT "heizer/room/request"
#define MQTT_TOPIC_AVAIL_DEFAULT "heizer/room/availability"
#define MQTT_URI_MAX            128
#define MQTT_TOPIC_MAX          64
#define MQTT_USER_MAX           64
#define MQTT_PASS_MAX           64
#define HEAT_REASON_MAX         64

#define AHT10_ADDR             0x38

#define RELAY_GPIO             GPIO_NUM_26
#define RELAY_ACTIVE_LEVEL     1

#define I2C_PORT               I2C_NUM_0
#define I2C_SDA                GPIO_NUM_21
#define I2C_SCL                GPIO_NUM_22
#define I2C_FREQ_HZ            100000

#define LCD_HOST               SPI2_HOST
#define LCD_PIN_MOSI           GPIO_NUM_23
#define LCD_PIN_SCLK           GPIO_NUM_18
#define LCD_PIN_CS             GPIO_NUM_5
#define LCD_PIN_DC             GPIO_NUM_2
#define LCD_PIN_RST            GPIO_NUM_4
#define LCD_PIN_BL             GPIO_NUM_27
#define LCD_BL_ACTIVE_LEVEL    0
#define LCD_PIXEL_CLOCK_HZ     (2 * 1000 * 1000)
#define LCD_H_RES              284
#define LCD_V_RES              76
#define LCD_X_GAP              18
#define LCD_Y_GAP              82
#define DISPLAY_DIAG_MODE      0
#define FB_MIRROR_X            1
#define FB_MIRROR_Y            0

#define WIFI_CONNECTED_BIT     BIT0

static const char *TAG = "HEIZER";

typedef enum {
    HEAT_MODE_AUTO = 0,
    HEAT_MODE_MANUAL_ON,
    HEAT_MODE_MANUAL_OFF,
} heat_mode_t;

typedef struct {
    float temp_c;
    float hum_pct;
    float sensor_offset_c;
    float target_c;
    float window_c;
    bool heater_on;          // effective displayed heater state
    bool heater_request_on;  // local requested state (for ioBroker automation)
    bool heater_feedback_on; // confirmed state from ioBroker
    bool heater_feedback_valid;
    bool automation_remote;      // true: ioBroker decides final state
    bool local_fallback_enabled; // use relay output when remote automation unavailable
    heat_mode_t mode;
    bool sensor_ok;
    float power_w;
    float energy_today_kwh;
    float price_eur_kwh;
    float session_energy_kwh;
    float session_cost_eur;
    bool session_from_iobroker;
    bool timer_enabled;
    uint16_t timer_start_min;
    uint16_t timer_end_min;
    heat_mode_t timer_mode_in;
    heat_mode_t timer_mode_out;
    heat_mode_t effective_mode;
    char heat_reason[HEAT_REASON_MAX];
    bool mqtt_connected;
    int64_t last_status_rx_ms;
} app_state_t;

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_state_mutex;
static app_state_t s_state = {
    .temp_c = 0.0f,
    .hum_pct = 0.0f,
    .sensor_offset_c = 0.0f,
    .target_c = 21.0f,
    .window_c = 1.0f,
    .heater_on = false,
    .heater_request_on = false,
    .heater_feedback_on = false,
    .heater_feedback_valid = false,
    .automation_remote = true,
    .local_fallback_enabled = false,
    .mode = HEAT_MODE_AUTO,
    .sensor_ok = false,
    .power_w = 0.0f,
    .energy_today_kwh = 0.0f,
    .price_eur_kwh = 0.393f,
    .session_energy_kwh = 0.0f,
    .session_cost_eur = 0.0f,
    .session_from_iobroker = false,
    .timer_enabled = false,
    .timer_start_min = 360,   // 06:00
    .timer_end_min = 1320,    // 22:00
    .timer_mode_in = HEAT_MODE_MANUAL_ON,
    .timer_mode_out = HEAT_MODE_AUTO,
    .effective_mode = HEAT_MODE_AUTO,
    .heat_reason = "init",
    .mqtt_connected = false,
    .last_status_rx_ms = 0,
};
static esp_mqtt_client_handle_t s_mqtt_client;
static esp_lcd_panel_handle_t s_lcd_panel;
static uint16_t s_lcd_fb[LCD_H_RES * LCD_V_RES];
static char s_mqtt_broker_uri[MQTT_URI_MAX] = MQTT_BROKER_URI_DEFAULT;
static char s_mqtt_topic_state[MQTT_TOPIC_MAX] = MQTT_TOPIC_STATE_DEFAULT;
static char s_mqtt_topic_cmd[MQTT_TOPIC_MAX] = MQTT_TOPIC_CMD_DEFAULT;
static char s_mqtt_topic_power[MQTT_TOPIC_MAX] = MQTT_TOPIC_POWER_DEFAULT;
static char s_mqtt_topic_timer[MQTT_TOPIC_MAX] = MQTT_TOPIC_TIMER_DEFAULT;
static char s_mqtt_topic_switch[MQTT_TOPIC_MAX] = MQTT_TOPIC_SWITCH_DEFAULT;
static char s_mqtt_topic_status[MQTT_TOPIC_MAX] = MQTT_TOPIC_STATUS_DEFAULT;
static char s_mqtt_topic_config[MQTT_TOPIC_MAX] = MQTT_TOPIC_CONFIG_DEFAULT;
static char s_mqtt_topic_request[MQTT_TOPIC_MAX] = MQTT_TOPIC_REQUEST_DEFAULT;
static char s_mqtt_topic_avail[MQTT_TOPIC_MAX] = MQTT_TOPIC_AVAIL_DEFAULT;
static char s_mqtt_username[MQTT_USER_MAX] = "schaefra";
static char s_mqtt_password[MQTT_PASS_MAX] = "1snickers";
static int64_t s_energy_last_us = 0;
static int s_energy_day_token = -1;
static bool s_switch_topic_last_valid = false;
static bool s_switch_topic_last_state = false;
static bool s_request_topic_last_valid = false;
static bool s_request_topic_last_state = false;
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

static bool parse_power_payload(const char *payload, int len, float *out_watts);
static heat_mode_t mode_from_string(const char *s, heat_mode_t fallback);
static const char *mode_to_string(heat_mode_t m);
static bool parse_bool_text(const char *s, bool *out);
static bool parse_hhmm_to_min(const char *s, uint16_t *out_min);
static void format_hhmm(uint16_t min_of_day, char out[6]);
static bool timer_is_active_now(uint16_t start_min, uint16_t end_min);
static void time_sync_init(void);
static void app_cfg_load(void);
static void app_cfg_save(void);

static const char s_web_ui[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Heizer Control</title><style>"
":root{--bg:#0a0f14;--bg2:#0f1822;--fg:#e9f1f8;--muted:#8fa3b8;--acc:#46c2ff;--acc2:#5cf5c8;--card:#101924;--card2:#142132;--line:#233447;}"
"body{font-family:Segoe UI,Roboto,Ubuntu,sans-serif;background:radial-gradient(1200px 500px at 10% -10%,#1c2f45 0%,transparent 60%),radial-gradient(800px 400px at 90% 0%,#123549 0%,transparent 60%),linear-gradient(165deg,var(--bg),var(--bg2));color:var(--fg);margin:0;padding:1rem;min-height:100vh;}"
".card{max-width:720px;margin:0 auto;background:linear-gradient(180deg,rgba(255,255,255,.03),rgba(255,255,255,.01));border:1px solid var(--line);border-radius:16px;padding:1.1rem 1.25rem;box-shadow:0 20px 40px rgba(0,0,0,.4),inset 0 1px 0 rgba(255,255,255,.05);backdrop-filter:blur(6px);}"
"h1{margin:.2rem 0 1rem;font-size:1.45rem;letter-spacing:.02em;}"
".kpi{display:flex;gap:.8rem;flex-wrap:wrap}.k{flex:1;min-width:150px;background:linear-gradient(180deg,var(--card2),var(--card));border:1px solid var(--line);border-radius:12px;padding:.85rem}"
"label{display:block;margin-top:.95rem;font-weight:600;color:#d6e3ef}"
"input[type=range]{width:100%;accent-color:#59d0ff}input[type=number],input[type=text],input[type=time],input[type=password],select{width:100%;padding:.5rem .6rem;box-sizing:border-box;background:#0b131d;color:var(--fg);border:1px solid var(--line);border-radius:9px;outline:none}"
"input[type=number]:focus,input[type=text]:focus,input[type=time]:focus,input[type=password]:focus,select:focus{border-color:#59d0ff;box-shadow:0 0 0 3px rgba(89,208,255,.15)}"
"button{margin-top:1rem;background:linear-gradient(90deg,var(--acc),var(--acc2));color:#06222f;border:0;padding:.62rem .95rem;border-radius:10px;font-weight:800;letter-spacing:.02em;cursor:pointer}"
"h2{margin:1.2rem 0 .4rem;font-size:1.0rem;color:#b8cee1;letter-spacing:.03em;text-transform:uppercase}"
"button:hover{filter:brightness(1.07)}small{color:var(--muted)}code{color:#9de6ff}</style></head><body><div class='card'><h1>Heizer Control</h1>"
"<h2>Live Werte</h2>"
"<div class='kpi'><div class='k'>Temp: <b id='t'>--.-</b> &deg;C</div><div class='k'>Feuchte: <b id='h'>--.-</b> %</div><div class='k'>Heizer Ist: <b id='r'>---</b></div></div>"
"<div class='kpi'><div class='k'>Heizer Soll: <b id='rr'>---</b></div><div class='k'>Leistung: <b id='pw'>0</b> W</div><div class='k'>Grund: <b id='reason'>-</b></div></div>"
"<div class='kpi'><div class='k'>Tag: <b id='ek'>0.000</b> kWh</div><div class='k'>Session: <b id='sk'>0.000</b> kWh</div><div class='k'>Kosten: <b id='ee'>0.00</b> EUR</div></div>"
"<div class='kpi'><div class='k'>Timer: <b id='timst'>AUS</b></div><div class='k'>Aktiver Modus: <b id='emode'>AUTO</b></div><div class='k'>MQTT: <b id='mq'>off</b></div></div>"
"<h2>Heizparameter</h2>"
"<label>Betriebsmodus</label><select id='mode'><option value='AUTO'>AUTO</option><option value='ON'>MANUAL_ON</option><option value='OFF'>MANUAL_OFF</option></select>"
"<label><input id='ar' type='checkbox' style='width:auto;margin-right:.5rem'>Automationslogik in ioBroker (empfohlen)</label>"
"<label><input id='lf' type='checkbox' style='width:auto;margin-right:.5rem'>Lokalen Relay-Fallback aktivieren</label>"
"<label>Zieltemperatur: <span id='tv'>21.0</span> &deg;C</label><input id='target' type='range' min='15' max='28' step='0.1'>"
"<label>Temperaturfenster (Hysterese): <span id='wv'>1.0</span> &deg;C</label><input id='window' type='range' min='0.2' max='5.0' step='0.1'>"
"<label>Sensor Offset (&deg;C)</label><input id='so' type='number' min='-10' max='10' step='0.1'>"
"<label>Oder exakt eingeben</label><input id='tn' type='number' min='15' max='28' step='0.1'><input id='wn' type='number' min='0.2' max='5' step='0.1'>"
"<div><small>Heizer EIN unter: <span id='onv'>--.-</span> &deg;C, AUS ueber: <span id='offv'>--.-</span> &deg;C</small></div>"
"<label><input id='ten' type='checkbox' style='width:auto;margin-right:.5rem'>Timer aktiv</label>"
"<div class='kpi'><div class='k'><label style='margin-top:0'>Start</label><input id='ts' type='time' value='06:00'></div><div class='k'><label style='margin-top:0'>Ende</label><input id='te' type='time' value='22:00'></div></div>"
"<div class='kpi'><div class='k'><label style='margin-top:0'>Modus im Zeitfenster</label><select id='tmi'><option value='ON'>ON</option><option value='OFF'>OFF</option><option value='AUTO'>AUTO</option></select></div><div class='k'><label style='margin-top:0'>Modus ausserhalb</label><select id='tmo'><option value='AUTO'>AUTO</option><option value='ON'>ON</option><option value='OFF'>OFF</option></select></div></div>"
"<h2>MQTT & Technik</h2>"
"<label>MQTT Broker (z. B. mqtt://192.168.2.10)</label><input id='mb' type='text'>"
"<label>MQTT Login</label><input id='mu' type='text'>"
"<label>MQTT Passwort</label><input id='mp' type='password'>"
"<label>MQTT State Topic (publish)</label><input id='mts' type='text'>"
"<label>MQTT Config Topic (publish)</label><input id='mcfg' type='text'>"
"<label>MQTT Request Topic (publish)</label><input id='mreq' type='text'>"
"<label>MQTT Availability Topic (publish)</label><input id='mav' type='text'>"
"<label>MQTT Command Topic (subscribe)</label><input id='mtc' type='text'>"
"<label>MQTT Power Topic (Watt subscribe)</label><input id='mtp' type='text'>"
"<label>MQTT Timer Topic (JSON subscribe)</label><input id='mtt' type='text'>"
"<label>MQTT ioBroker Status Topic (subscribe)</label><input id='mstat' type='text'>"
"<label>MQTT Heizer Schalt-Topic (publish Ist-Zustand)</label><input id='mhs' type='text'>"
"<label>Strompreis (EUR/kWh)</label><input id='pkwh' type='number' min='0.05' max='2.00' step='0.001'>"
"<div><small>Steuerung per MQTT: sende <b>ON</b>, <b>OFF</b> oder <b>AUTO</b> an <code id='cmdtopic'>-</code></small></div>"
"<button id='save'>Speichern</button><p id='msg'></p></div>"
"<script>"
"const t=document.getElementById('target'),w=document.getElementById('window'),tn=document.getElementById('tn'),wn=document.getElementById('wn');"
"const mb=document.getElementById('mb'),mu=document.getElementById('mu'),mp=document.getElementById('mp'),mts=document.getElementById('mts'),mcfg=document.getElementById('mcfg'),mreq=document.getElementById('mreq'),mav=document.getElementById('mav'),mtc=document.getElementById('mtc'),mtp=document.getElementById('mtp'),mtt=document.getElementById('mtt'),mstat=document.getElementById('mstat'),mhs=document.getElementById('mhs'),pkwh=document.getElementById('pkwh'),cmdtopic=document.getElementById('cmdtopic');"
"const mode=document.getElementById('mode'),ar=document.getElementById('ar'),lf=document.getElementById('lf'),so=document.getElementById('so');"
"const ten=document.getElementById('ten'),ts=document.getElementById('ts'),te=document.getElementById('te'),tmi=document.getElementById('tmi'),tmo=document.getElementById('tmo');"
"let editing=false;"
"function upd(){document.getElementById('tv').textContent=Number(t.value).toFixed(1);document.getElementById('wv').textContent=Number(w.value).toFixed(1);"
"tn.value=t.value;wn.value=w.value;const on=Number(t.value)-Number(w.value)/2,off=Number(t.value)+Number(w.value)/2;"
"document.getElementById('onv').textContent=on.toFixed(1);document.getElementById('offv').textContent=off.toFixed(1);}"
"t.oninput=()=>{editing=true;upd()};w.oninput=()=>{editing=true;upd()};tn.oninput=()=>{editing=true;t.value=tn.value;upd()};wn.oninput=()=>{editing=true;w.value=wn.value;upd()};"
"[mb,mu,mp,mts,mcfg,mreq,mav,mtc,mtp,mtt,mstat,mhs,pkwh,ten,ts,te,tmi,tmo,mode,ar,lf,so].forEach(e=>{e.oninput=()=>{editing=true;};});"
"async function load(){const r=await fetch('/api/state');const j=await r.json();"
"if(editing){return;}"
"document.getElementById('t').textContent=j.temp_c.toFixed(1);document.getElementById('h').textContent=j.hum_pct.toFixed(1);"
"document.getElementById('pw').textContent=(j.power_w||0).toFixed(0);"
"document.getElementById('r').textContent=j.heater_on?'AN':'AUS';document.getElementById('rr').textContent=j.heater_request_on?'AN':'AUS';t.value=j.target_c;w.value=j.window_c;"
"document.getElementById('emode').textContent=j.effective_mode||'AUTO';"
"document.getElementById('reason').textContent=j.heat_reason||'-';"
"document.getElementById('mq').textContent=j.mqtt_connected?'online':'offline';"
"document.getElementById('timst').textContent=(j.timer_enabled?((j.timer_start||'06:00')+'-'+(j.timer_end||'22:00')):'AUS');"
"document.getElementById('ek').textContent=(j.energy_today_kwh||0).toFixed(3);document.getElementById('sk').textContent=(j.session_energy_kwh||0).toFixed(3);document.getElementById('ee').textContent=((j.session_cost_eur||0)>0?(j.session_cost_eur||0):((j.energy_today_kwh||0)*(j.price_eur_kwh||0))).toFixed(2);"
"mb.value=j.mqtt_broker_uri||'';mu.value=j.mqtt_username||'';mp.value=j.mqtt_password||'';mts.value=j.mqtt_topic_state||'';mcfg.value=j.mqtt_topic_config||'';mreq.value=j.mqtt_topic_request||'';mav.value=j.mqtt_topic_avail||'';mtc.value=j.mqtt_topic_cmd||'';mtp.value=j.mqtt_topic_power||'';mtt.value=j.mqtt_topic_timer||'';mstat.value=j.mqtt_topic_status||'';mhs.value=j.mqtt_topic_switch||'';pkwh.value=(j.price_eur_kwh||0.393).toFixed(3);"
"mode.value=j.mode||'AUTO';ar.checked=!!j.automation_remote;lf.checked=!!j.local_fallback_enabled;so.value=(j.sensor_offset_c||0).toFixed(1);"
"ten.checked=!!j.timer_enabled;ts.value=j.timer_start||'06:00';te.value=j.timer_end||'22:00';tmi.value=j.timer_mode_in||'ON';tmo.value=j.timer_mode_out||'AUTO';cmdtopic.textContent=mtc.value||'-';upd();}"
"document.getElementById('save').onclick=async()=>{const body={target_c:Number(t.value),window_c:Number(w.value),sensor_offset_c:Number(so.value),mode:mode.value,automation_remote:ar.checked,local_fallback_enabled:lf.checked,mqtt_broker_uri:mb.value,mqtt_username:mu.value,mqtt_password:mp.value,mqtt_topic_state:mts.value,mqtt_topic_config:mcfg.value,mqtt_topic_request:mreq.value,mqtt_topic_avail:mav.value,mqtt_topic_cmd:mtc.value,mqtt_topic_power:mtp.value,mqtt_topic_timer:mtt.value,mqtt_topic_status:mstat.value,mqtt_topic_switch:mhs.value,price_eur_kwh:Number(pkwh.value),timer_enabled:ten.checked,timer_start:ts.value,timer_end:te.value,timer_mode_in:tmi.value,timer_mode_out:tmo.value};"
"const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"document.getElementById('msg').textContent=r.ok?'Gespeichert':'Fehler beim Speichern';if(r.ok){editing=false;}setTimeout(()=>document.getElementById('msg').textContent='',2500);load();};"
"document.addEventListener('focusout',()=>{setTimeout(()=>{editing=false;},300);});"
"mtc.oninput=()=>cmdtopic.textContent=mtc.value||'-';"
"setInterval(load,4000);load();"
"</script></body></html>";

static void relay_set(bool on)
{
    gpio_set_level(RELAY_GPIO, on ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
}

static const uint8_t *glyph5x7(char c)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t dot[5] = {0, 0x60, 0x60, 0, 0};
    static const uint8_t colon[5] = {0, 0x36, 0x36, 0, 0};
    static const uint8_t percent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t C[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t H[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
    static const uint8_t T[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
    static const uint8_t A[5] = {0x7E, 0x09, 0x09, 0x09, 0x7E};
    static const uint8_t N[5] = {0x7F, 0x06, 0x18, 0x60, 0x7F};
    static const uint8_t U[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
    static const uint8_t S[5] = {0x26, 0x49, 0x49, 0x49, 0x32};
    static const uint8_t E[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
    static const uint8_t O[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
    static const uint8_t F[5] = {0x7F, 0x48, 0x48, 0x48, 0x40};
    static const uint8_t R[5] = {0x7F, 0x48, 0x4C, 0x4A, 0x31};
    static const uint8_t W[5] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
    static const uint8_t P[5] = {0x7F, 0x48, 0x48, 0x48, 0x30};
    static const uint8_t L[5] = {0x7F, 0x01, 0x01, 0x01, 0x01};
    static const uint8_t D[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
    static const uint8_t Y[5] = {0x70, 0x08, 0x07, 0x08, 0x70};
    static const uint8_t M[5] = {0x7F, 0x30, 0x0C, 0x30, 0x7F};
    static const uint8_t I[5] = {0x41, 0x41, 0x7F, 0x41, 0x41};
    static const uint8_t G[5] = {0x3E, 0x41, 0x49, 0x49, 0x2E};
    static const uint8_t K[5] = {0x7F, 0x08, 0x14, 0x22, 0x41};
    static const uint8_t V[5] = {0x78, 0x06, 0x01, 0x06, 0x78};
    static const uint8_t Z[5] = {0x43, 0x45, 0x49, 0x51, 0x61};
    static const uint8_t digits[10][5] = {
        {0x3E, 0x45, 0x49, 0x51, 0x3E},
        {0x00, 0x21, 0x7F, 0x01, 0x00},
        {0x21, 0x43, 0x45, 0x49, 0x31},
        {0x42, 0x41, 0x51, 0x69, 0x46},
        {0x0C, 0x14, 0x24, 0x7F, 0x04},
        {0x72, 0x51, 0x51, 0x51, 0x4E},
        {0x1E, 0x29, 0x49, 0x49, 0x06},
        {0x40, 0x47, 0x48, 0x50, 0x60},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x30, 0x49, 0x49, 0x4A, 0x3C},
    };

    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    switch (c) {
        case '.': return dot;
        case ':': return colon;
        case '%': return percent;
        case '-': return dash;
        case 'C': return C;
        case 'H': return H;
        case 'T': return T;
        case 'A': return A;
        case 'N': return N;
        case 'U': return U;
        case 'S': return S;
        case 'E': return E;
        case 'O': return O;
        case 'F': return F;
        case 'R': return R;
        case 'W': return W;
        case 'P': return P;
        case 'L': return L;
        case 'D': return D;
        case 'Y': return Y;
        case 'M': return M;
        case 'I': return I;
        case 'G': return G;
        case 'K': return K;
        case 'V': return V;
        case 'Z': return Z;
        case ' ': return blank;
        default: return blank;
    }
}

static void lcd_clear(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_lcd_fb[i] = color;
    }
}

static inline void lcd_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= LCD_H_RES || y >= LCD_V_RES) {
        return;
    }
#if FB_MIRROR_X
    x = (LCD_H_RES - 1) - x;
#endif
#if FB_MIRROR_Y
    y = (LCD_V_RES - 1) - y;
#endif
    s_lcd_fb[y * LCD_H_RES + x] = color;
}

static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > LCD_H_RES ? LCD_H_RES : (x + w);
    int y1 = (y + h) > LCD_V_RES ? LCD_V_RES : (y + h);

    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            s_lcd_fb[yy * LCD_H_RES + xx] = color;
        }
    }
}

static void lcd_draw_char(int x, int y, char c, uint16_t fg)
{
    const uint8_t *g = glyph5x7(c);
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 8; row++) {
            if (bits & (1U << row)) {
                lcd_pixel(x + col, y + row, fg);
            }
        }
    }
}

static void lcd_draw_char_scaled(int x, int y, char c, uint16_t fg, int scale)
{
    const uint8_t *g = glyph5x7(c);
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 8; row++) {
            if (bits & (1U << row)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        lcd_pixel(x + col * scale + sx, y + row * scale + sy, fg);
                    }
                }
            }
        }
    }
}

static void lcd_draw_text(int x, int y, const char *text, uint16_t fg)
{
    int cx = x;
    while (*text) {
        lcd_draw_char(cx, y, *text++, fg);
        cx += 6;
    }
}

static void lcd_draw_text_scaled(int x, int y, const char *text, uint16_t fg, int scale)
{
    int cx = x;
    while (*text) {
        lcd_draw_char_scaled(cx, y, *text++, fg, scale);
        cx += 6 * scale;
    }
}

static void lcd_draw_mode_icon(int x, int y, bool is_auto, uint16_t color)
{
    if (is_auto) {
        // small gear-like 8x8 glyph
        static const uint8_t g[8] = {0x18, 0x3C, 0x66, 0xDB, 0xDB, 0x66, 0x3C, 0x18};
        for (int yy = 0; yy < 8; yy++) {
            for (int xx = 0; xx < 8; xx++) {
                if (g[yy] & (1U << (7 - xx))) {
                    lcd_pixel(x + xx, y + yy, color);
                }
            }
        }
    } else {
        // simple hand-ish block icon 8x8
        static const uint8_t h[8] = {0x18, 0x18, 0x1C, 0x1C, 0x3E, 0x3E, 0x1C, 0x08};
        for (int yy = 0; yy < 8; yy++) {
            for (int xx = 0; xx < 8; xx++) {
                if (h[yy] & (1U << (7 - xx))) {
                    lcd_pixel(x + xx, y + yy, color);
                }
            }
        }
    }
}

static void lcd_flush(void)
{
    esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_lcd_fb);
}

#if DISPLAY_DIAG_MODE
static void lcd_draw_diag_bars(void)
{
    const uint16_t bars[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    const int n = (int)(sizeof(bars) / sizeof(bars[0]));
    int bar_w = LCD_H_RES / n;

    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            int idx = x / bar_w;
            if (idx >= n) idx = n - 1;
            s_lcd_fb[y * LCD_H_RES + x] = bars[idx];
        }
    }
}
#endif

static void display_update(void)
{
    app_state_t st;
    float disp_energy_kwh;
    float disp_price_kwh;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    st = s_state;
    xSemaphoreGive(s_state_mutex);

    disp_energy_kwh = st.session_from_iobroker ? st.session_energy_kwh : st.energy_today_kwh;
    disp_price_kwh = st.price_eur_kwh;
    if (st.session_from_iobroker && st.session_energy_kwh > 0.0001f) {
        disp_price_kwh = st.session_cost_eur / st.session_energy_kwh;
    }

    display_tft_render(
        st.temp_c,
        st.hum_pct,
        st.target_c,
        st.window_c,
        st.heater_on,
        (int)st.effective_mode,
        st.sensor_ok,
        st.power_w,
        disp_energy_kwh,
        disp_price_kwh,
        st.timer_enabled,
        st.timer_start_min,
        st.timer_end_min);
}

static int energy_day_token_now(void)
{
    time_t now = time(NULL);
    if (now > 1700000000) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        return (tm_now.tm_year + 1900) * 1000 + tm_now.tm_yday;
    }
    return (int)(esp_timer_get_time() / (24LL * 3600LL * 1000000LL));
}

static void energy_integrate_locked(int64_t now_us)
{
    int token = energy_day_token_now();
    if (s_energy_day_token < 0) {
        s_energy_day_token = token;
    } else if (token != s_energy_day_token) {
        s_energy_day_token = token;
        s_state.energy_today_kwh = 0.0f;
    }

    if (s_energy_last_us <= 0) {
        s_energy_last_us = now_us;
        return;
    }

    int64_t dt_us = now_us - s_energy_last_us;
    if (dt_us < 0) {
        dt_us = 0;
    }
    s_energy_last_us = now_us;

    float dt_h = (float)dt_us / 3600000000.0f;
    float p_kw = s_state.power_w > 0.0f ? (s_state.power_w / 1000.0f) : 0.0f;
    s_state.energy_today_kwh += p_kw * dt_h;
}

static esp_err_t aht10_write_cmd(uint8_t c0, uint8_t c1, uint8_t c2)
{
    uint8_t cmd[3] = {c0, c1, c2};
    return i2c_master_write_to_device(I2C_PORT, AHT10_ADDR, cmd, sizeof(cmd), pdMS_TO_TICKS(100));
}

static bool aht10_init(void)
{
    esp_err_t err;

    err = i2c_master_write_to_device(I2C_PORT, AHT10_ADDR, (uint8_t[]){0xBA}, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT10 reset failed: %s", esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    err = aht10_write_cmd(0xE1, 0x08, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT10 init failed: %s", esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

static bool aht10_read(float *temp_c, float *hum_pct)
{
    uint8_t data[6] = {0};

    if (aht10_write_cmd(0xAC, 0x33, 0x00) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(90));

    if (i2c_master_read_from_device(I2C_PORT, AHT10_ADDR, data, sizeof(data), pdMS_TO_TICKS(100)) != ESP_OK) {
        return false;
    }

    if (data[0] & 0x80) {
        return false;
    }

    uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | ((uint32_t)(data[3] & 0xF0) >> 4);
    uint32_t raw_t = ((uint32_t)(data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *hum_pct = (raw_h * 100.0f) / 1048576.0f;
    *temp_c = (raw_t * 200.0f) / 1048576.0f - 50.0f;
    return true;
}

static heat_mode_t mode_from_string(const char *s, heat_mode_t fallback)
{
    if (!s) return fallback;
    if (strcasecmp(s, "AUTO") == 0) return HEAT_MODE_AUTO;
    if (strcasecmp(s, "ON") == 0 || strcasecmp(s, "MANUAL_ON") == 0) return HEAT_MODE_MANUAL_ON;
    if (strcasecmp(s, "OFF") == 0 || strcasecmp(s, "MANUAL_OFF") == 0) return HEAT_MODE_MANUAL_OFF;
    return fallback;
}

static const char *mode_to_string(heat_mode_t m)
{
    return m == HEAT_MODE_AUTO ? "AUTO" : (m == HEAT_MODE_MANUAL_ON ? "ON" : "OFF");
}

static bool parse_bool_text(const char *s, bool *out)
{
    if (!s || !out) {
        return false;
    }
    if (strcasecmp(s, "true") == 0 || strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(s, "false") == 0 || strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_hhmm_to_min(const char *s, uint16_t *out_min)
{
    if (!s || !out_min) return false;
    int h = -1, m = -1;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out_min = (uint16_t)(h * 60 + m);
    return true;
}

static void format_hhmm(uint16_t min_of_day, char out[6])
{
    int h = (min_of_day / 60) % 24;
    int m = min_of_day % 60;
    snprintf(out, 6, "%02d:%02d", h, m);
}

static bool timer_is_active_now(uint16_t start_min, uint16_t end_min)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        return false;
    }

    struct tm t;
    localtime_r(&now, &t);
    int cur = t.tm_hour * 60 + t.tm_min;

    if (start_min == end_min) return true; // 24h
    if (start_min < end_min) {
        return (cur >= start_min && cur < end_min);
    }
    return (cur >= start_min || cur < end_min); // crosses midnight
}

static void time_sync_init(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void nvs_get_str_or_default(nvs_handle_t nvs, const char *key, char *dst, size_t dst_len)
{
    size_t required = dst_len;
    if (nvs_get_str(nvs, key, dst, &required) != ESP_OK) {
        dst[0] = '\0';
    }
}

static void app_cfg_load(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("heizer_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "Keine gespeicherte Konfiguration gefunden, nutze Defaults");
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    nvs_get_blob(nvs, "target_c", &s_state.target_c, &(size_t){sizeof(s_state.target_c)});
    nvs_get_blob(nvs, "window_c", &s_state.window_c, &(size_t){sizeof(s_state.window_c)});
    nvs_get_blob(nvs, "price_kwh", &s_state.price_eur_kwh, &(size_t){sizeof(s_state.price_eur_kwh)});
    nvs_get_blob(nvs, "sensor_off", &s_state.sensor_offset_c, &(size_t){sizeof(s_state.sensor_offset_c)});
    nvs_get_blob(nvs, "timer_en", &s_state.timer_enabled, &(size_t){sizeof(s_state.timer_enabled)});
    nvs_get_blob(nvs, "tstart", &s_state.timer_start_min, &(size_t){sizeof(s_state.timer_start_min)});
    nvs_get_blob(nvs, "tend", &s_state.timer_end_min, &(size_t){sizeof(s_state.timer_end_min)});
    nvs_get_blob(nvs, "tmode_in", &s_state.timer_mode_in, &(size_t){sizeof(s_state.timer_mode_in)});
    nvs_get_blob(nvs, "tmode_out", &s_state.timer_mode_out, &(size_t){sizeof(s_state.timer_mode_out)});
    nvs_get_blob(nvs, "mode", &s_state.mode, &(size_t){sizeof(s_state.mode)});
    nvs_get_blob(nvs, "auto_rem", &s_state.automation_remote, &(size_t){sizeof(s_state.automation_remote)});
    nvs_get_blob(nvs, "fallback", &s_state.local_fallback_enabled, &(size_t){sizeof(s_state.local_fallback_enabled)});

    nvs_get_str_or_default(nvs, "mqtt_uri", s_mqtt_broker_uri, sizeof(s_mqtt_broker_uri));
    nvs_get_str_or_default(nvs, "mqtt_usr", s_mqtt_username, sizeof(s_mqtt_username));
    nvs_get_str_or_default(nvs, "mqtt_pwd", s_mqtt_password, sizeof(s_mqtt_password));
    nvs_get_str_or_default(nvs, "topic_st", s_mqtt_topic_state, sizeof(s_mqtt_topic_state));
    nvs_get_str_or_default(nvs, "topic_cmd", s_mqtt_topic_cmd, sizeof(s_mqtt_topic_cmd));
    nvs_get_str_or_default(nvs, "topic_pow", s_mqtt_topic_power, sizeof(s_mqtt_topic_power));
    nvs_get_str_or_default(nvs, "topic_tim", s_mqtt_topic_timer, sizeof(s_mqtt_topic_timer));
    nvs_get_str_or_default(nvs, "topic_swi", s_mqtt_topic_switch, sizeof(s_mqtt_topic_switch));
    nvs_get_str_or_default(nvs, "topic_sta", s_mqtt_topic_status, sizeof(s_mqtt_topic_status));
    nvs_get_str_or_default(nvs, "topic_cfg", s_mqtt_topic_config, sizeof(s_mqtt_topic_config));
    nvs_get_str_or_default(nvs, "topic_req", s_mqtt_topic_request, sizeof(s_mqtt_topic_request));
    nvs_get_str_or_default(nvs, "topic_avl", s_mqtt_topic_avail, sizeof(s_mqtt_topic_avail));
    xSemaphoreGive(s_state_mutex);

    // Restore defaults if empty in NVS.
    if (s_mqtt_broker_uri[0] == '\0') strlcpy(s_mqtt_broker_uri, MQTT_BROKER_URI_DEFAULT, sizeof(s_mqtt_broker_uri));
    if (s_mqtt_topic_state[0] == '\0') strlcpy(s_mqtt_topic_state, MQTT_TOPIC_STATE_DEFAULT, sizeof(s_mqtt_topic_state));
    if (s_mqtt_topic_cmd[0] == '\0') strlcpy(s_mqtt_topic_cmd, MQTT_TOPIC_CMD_DEFAULT, sizeof(s_mqtt_topic_cmd));
    if (s_mqtt_topic_power[0] == '\0') strlcpy(s_mqtt_topic_power, MQTT_TOPIC_POWER_DEFAULT, sizeof(s_mqtt_topic_power));
    if (s_mqtt_topic_timer[0] == '\0') strlcpy(s_mqtt_topic_timer, MQTT_TOPIC_TIMER_DEFAULT, sizeof(s_mqtt_topic_timer));
    if (s_mqtt_topic_switch[0] == '\0') strlcpy(s_mqtt_topic_switch, MQTT_TOPIC_SWITCH_DEFAULT, sizeof(s_mqtt_topic_switch));
    if (s_mqtt_topic_status[0] == '\0') strlcpy(s_mqtt_topic_status, MQTT_TOPIC_STATUS_DEFAULT, sizeof(s_mqtt_topic_status));
    if (s_mqtt_topic_config[0] == '\0') strlcpy(s_mqtt_topic_config, MQTT_TOPIC_CONFIG_DEFAULT, sizeof(s_mqtt_topic_config));
    if (s_mqtt_topic_request[0] == '\0') strlcpy(s_mqtt_topic_request, MQTT_TOPIC_REQUEST_DEFAULT, sizeof(s_mqtt_topic_request));
    if (s_mqtt_topic_avail[0] == '\0') strlcpy(s_mqtt_topic_avail, MQTT_TOPIC_AVAIL_DEFAULT, sizeof(s_mqtt_topic_avail));

    nvs_close(nvs);
    ESP_LOGI(TAG, "Konfiguration aus NVS geladen");
}

static void app_cfg_save(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("heizer_cfg", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open fehlgeschlagen, Konfiguration nicht gespeichert");
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    nvs_set_blob(nvs, "target_c", &s_state.target_c, sizeof(s_state.target_c));
    nvs_set_blob(nvs, "window_c", &s_state.window_c, sizeof(s_state.window_c));
    nvs_set_blob(nvs, "price_kwh", &s_state.price_eur_kwh, sizeof(s_state.price_eur_kwh));
    nvs_set_blob(nvs, "sensor_off", &s_state.sensor_offset_c, sizeof(s_state.sensor_offset_c));
    nvs_set_blob(nvs, "timer_en", &s_state.timer_enabled, sizeof(s_state.timer_enabled));
    nvs_set_blob(nvs, "tstart", &s_state.timer_start_min, sizeof(s_state.timer_start_min));
    nvs_set_blob(nvs, "tend", &s_state.timer_end_min, sizeof(s_state.timer_end_min));
    nvs_set_blob(nvs, "tmode_in", &s_state.timer_mode_in, sizeof(s_state.timer_mode_in));
    nvs_set_blob(nvs, "tmode_out", &s_state.timer_mode_out, sizeof(s_state.timer_mode_out));
    nvs_set_blob(nvs, "mode", &s_state.mode, sizeof(s_state.mode));
    nvs_set_blob(nvs, "auto_rem", &s_state.automation_remote, sizeof(s_state.automation_remote));
    nvs_set_blob(nvs, "fallback", &s_state.local_fallback_enabled, sizeof(s_state.local_fallback_enabled));
    nvs_set_str(nvs, "mqtt_uri", s_mqtt_broker_uri);
    nvs_set_str(nvs, "mqtt_usr", s_mqtt_username);
    nvs_set_str(nvs, "mqtt_pwd", s_mqtt_password);
    nvs_set_str(nvs, "topic_st", s_mqtt_topic_state);
    nvs_set_str(nvs, "topic_cmd", s_mqtt_topic_cmd);
    nvs_set_str(nvs, "topic_pow", s_mqtt_topic_power);
    nvs_set_str(nvs, "topic_tim", s_mqtt_topic_timer);
    nvs_set_str(nvs, "topic_swi", s_mqtt_topic_switch);
    nvs_set_str(nvs, "topic_sta", s_mqtt_topic_status);
    nvs_set_str(nvs, "topic_cfg", s_mqtt_topic_config);
    nvs_set_str(nvs, "topic_req", s_mqtt_topic_request);
    nvs_set_str(nvs, "topic_avl", s_mqtt_topic_avail);
    xSemaphoreGive(s_state_mutex);

    nvs_commit(nvs);
    nvs_close(nvs);
}

static void update_control_locked(app_state_t *st)
{
    bool new_request = st->heater_request_on;
    heat_mode_t effective = st->mode;

    if (st->timer_enabled) {
        bool in_window = timer_is_active_now(st->timer_start_min, st->timer_end_min);
        effective = in_window ? st->timer_mode_in : st->timer_mode_out;
    }
    st->effective_mode = effective;

    if (effective == HEAT_MODE_MANUAL_ON) {
        new_request = true;
        strlcpy(st->heat_reason, "manual_on", sizeof(st->heat_reason));
    } else if (effective == HEAT_MODE_MANUAL_OFF) {
        new_request = false;
        strlcpy(st->heat_reason, "manual_off", sizeof(st->heat_reason));
    } else if (st->sensor_ok) {
        float on_th = st->target_c - (st->window_c * 0.5f);
        float off_th = st->target_c + (st->window_c * 0.5f);

        if (st->temp_c <= on_th) {
            new_request = true;
            strlcpy(st->heat_reason, "below_on_threshold", sizeof(st->heat_reason));
        }
        if (st->temp_c >= off_th) {
            new_request = false;
            strlcpy(st->heat_reason, "above_off_threshold", sizeof(st->heat_reason));
        }
    } else {
        strlcpy(st->heat_reason, "sensor_invalid", sizeof(st->heat_reason));
    }

    st->heater_request_on = new_request;

    if (st->automation_remote) {
        // ioBroker is the authority; relay is optional local fallback only.
        bool relay_out = st->local_fallback_enabled ? st->heater_request_on : false;
        relay_set(relay_out);
        st->heater_on = st->heater_feedback_valid ? st->heater_feedback_on : st->heater_request_on;
    } else {
        // Local fallback mode: ESP decides and drives relay directly.
        st->heater_on = st->heater_request_on;
        relay_set(st->heater_on);
    }
}

static void mqtt_publish_state(void)
{
    if (!s_mqtt_client) {
        return;
    }

    app_state_t st;
    char topic_state[MQTT_TOPIC_MAX];
    char topic_config[MQTT_TOPIC_MAX];
    char topic_request[MQTT_TOPIC_MAX];
    char topic_avail[MQTT_TOPIC_MAX];
    char topic_switch[MQTT_TOPIC_MAX];
    int64_t now_ms = esp_timer_get_time() / 1000;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    st = s_state;
    strlcpy(topic_state, s_mqtt_topic_state, sizeof(topic_state));
    strlcpy(topic_config, s_mqtt_topic_config, sizeof(topic_config));
    strlcpy(topic_request, s_mqtt_topic_request, sizeof(topic_request));
    strlcpy(topic_avail, s_mqtt_topic_avail, sizeof(topic_avail));
    strlcpy(topic_switch, s_mqtt_topic_switch, sizeof(topic_switch));
    xSemaphoreGive(s_state_mutex);

    char timer_start[6], timer_end[6];
    format_hhmm(st.timer_start_min, timer_start);
    format_hhmm(st.timer_end_min, timer_end);

    cJSON *root = cJSON_CreateObject();
    cJSON *telemetry = cJSON_AddObjectToObject(root, "telemetry");
    cJSON *cfg = cJSON_AddObjectToObject(root, "config");
    cJSON *status = cJSON_AddObjectToObject(root, "status");

    cJSON_AddNumberToObject(telemetry, "temp_c", st.temp_c);
    cJSON_AddNumberToObject(telemetry, "hum_pct", st.hum_pct);
    cJSON_AddBoolToObject(telemetry, "sensor_ok", st.sensor_ok);
    cJSON_AddNumberToObject(telemetry, "power_w", st.power_w);
    cJSON_AddNumberToObject(telemetry, "energy_today_kwh", st.energy_today_kwh);
    cJSON_AddNumberToObject(telemetry, "session_energy_kwh", st.session_energy_kwh);
    cJSON_AddNumberToObject(telemetry, "session_cost_eur", st.session_cost_eur);
    cJSON_AddBoolToObject(telemetry, "session_from_iobroker", st.session_from_iobroker);

    cJSON_AddNumberToObject(cfg, "target_c", st.target_c);
    cJSON_AddNumberToObject(cfg, "window_c", st.window_c);
    cJSON_AddNumberToObject(cfg, "sensor_offset_c", st.sensor_offset_c);
    cJSON_AddNumberToObject(cfg, "price_eur_kwh", st.price_eur_kwh);
    cJSON_AddStringToObject(cfg, "mode", mode_to_string(st.mode));
    cJSON_AddBoolToObject(cfg, "automation_remote", st.automation_remote);
    cJSON_AddBoolToObject(cfg, "local_fallback_enabled", st.local_fallback_enabled);
    cJSON_AddBoolToObject(cfg, "timer_enabled", st.timer_enabled);
    cJSON_AddStringToObject(cfg, "timer_start", timer_start);
    cJSON_AddStringToObject(cfg, "timer_end", timer_end);
    cJSON_AddStringToObject(cfg, "timer_mode_in", mode_to_string(st.timer_mode_in));
    cJSON_AddStringToObject(cfg, "timer_mode_out", mode_to_string(st.timer_mode_out));

    cJSON_AddBoolToObject(status, "heater_request_on", st.heater_request_on);
    cJSON_AddBoolToObject(status, "heater_on", st.heater_on);
    cJSON_AddBoolToObject(status, "heater_feedback_valid", st.heater_feedback_valid);
    cJSON_AddStringToObject(status, "effective_mode", mode_to_string(st.effective_mode));
    cJSON_AddStringToObject(status, "reason", st.heat_reason);
    cJSON_AddBoolToObject(status, "mqtt_connected", st.mqtt_connected);
    cJSON_AddNumberToObject(status, "last_status_rx_ms", (double)st.last_status_rx_ms);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms);
    // Backward compatibility for existing consumers.
    cJSON_AddNumberToObject(root, "temp_c", st.temp_c);
    cJSON_AddNumberToObject(root, "hum_pct", st.hum_pct);
    cJSON_AddNumberToObject(root, "target_c", st.target_c);
    cJSON_AddNumberToObject(root, "window_c", st.window_c);
    cJSON_AddBoolToObject(root, "heater_on", st.heater_on);
    cJSON_AddStringToObject(root, "mode", mode_to_string(st.mode));
    cJSON_AddStringToObject(root, "effective_mode", mode_to_string(st.effective_mode));
    cJSON_AddBoolToObject(root, "sensor_ok", st.sensor_ok);
    cJSON_AddNumberToObject(root, "power_w", st.power_w);
    cJSON_AddNumberToObject(root, "energy_today_kwh", st.energy_today_kwh);
    cJSON_AddNumberToObject(root, "price_eur_kwh", st.price_eur_kwh);

    char *json = cJSON_PrintUnformatted(root);
    if (json && topic_state[0] != '\0') {
        esp_mqtt_client_publish(s_mqtt_client, topic_state, json, 0, 1, 1);
        cJSON_free(json);
    } else if (json) {
        cJSON_free(json);
    }

    cJSON *cfg_root = cJSON_Duplicate(cfg, 1);
    if (cfg_root) {
        char *cfg_json = cJSON_PrintUnformatted(cfg_root);
        if (cfg_json && topic_config[0] != '\0') {
            esp_mqtt_client_publish(s_mqtt_client, topic_config, cfg_json, 0, 1, 1);
            cJSON_free(cfg_json);
        } else if (cfg_json) {
            cJSON_free(cfg_json);
        }
        cJSON_Delete(cfg_root);
    }

    if (topic_request[0] != '\0') {
        if (!s_request_topic_last_valid || s_request_topic_last_state != st.heater_request_on) {
            cJSON *req = cJSON_CreateObject();
            cJSON_AddBoolToObject(req, "heater_request_on", st.heater_request_on);
            cJSON_AddStringToObject(req, "mode", mode_to_string(st.effective_mode));
            cJSON_AddStringToObject(req, "reason", st.heat_reason);
            cJSON_AddNumberToObject(req, "temp_c", st.temp_c);
            cJSON_AddNumberToObject(req, "target_c", st.target_c);
            cJSON_AddNumberToObject(req, "window_c", st.window_c);
            char *req_json = cJSON_PrintUnformatted(req);
            if (req_json) {
                esp_mqtt_client_publish(s_mqtt_client, topic_request, req_json, 0, 1, 1);
                cJSON_free(req_json);
                s_request_topic_last_valid = true;
                s_request_topic_last_state = st.heater_request_on;
            }
            cJSON_Delete(req);
        }
    }

    if (topic_switch[0] != '\0') {
        if (!s_switch_topic_last_valid || s_switch_topic_last_state != st.heater_on) {
            esp_mqtt_client_publish(s_mqtt_client, topic_switch, st.heater_on ? "true" : "false", 0, 1, 1);
            s_switch_topic_last_valid = true;
            s_switch_topic_last_state = st.heater_on;
        }
    }

    if (topic_avail[0] != '\0') {
        esp_mqtt_client_publish(s_mqtt_client, topic_avail, "online", 0, 1, 1);
    }
    cJSON_Delete(root);
}

static void mqtt_restart(void)
{
    char broker_uri[MQTT_URI_MAX];
    char mqtt_user[MQTT_USER_MAX];
    char mqtt_pass[MQTT_PASS_MAX];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(broker_uri, s_mqtt_broker_uri, sizeof(broker_uri));
    strlcpy(mqtt_user, s_mqtt_username, sizeof(mqtt_user));
    strlcpy(mqtt_pass, s_mqtt_password, sizeof(mqtt_pass));
    xSemaphoreGive(s_state_mutex);

    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    s_switch_topic_last_valid = false;
    s_request_topic_last_valid = false;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = mqtt_user[0] ? mqtt_user : NULL,
        .credentials.authentication.password = mqtt_pass[0] ? mqtt_pass : NULL,
    };
    s_mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

static void control_task(void *arg)
{
    (void)arg;
    while (1) {
        int64_t now_us = esp_timer_get_time();
        float t = 0.0f;
        float h = 0.0f;
        bool ok = aht10_read(&t, &h);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        energy_integrate_locked(now_us);
        s_state.sensor_ok = ok;
        if (ok) {
            s_state.temp_c = t + s_state.sensor_offset_c;
            s_state.hum_pct = h;
        }
        update_control_locked(&s_state);
        xSemaphoreGive(s_state_mutex);

        display_update();
        mqtt_publish_state();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WLAN getrennt, reconnect...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "WLAN verbunden");
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t http_index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_web_ui, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_api_state_get(httpd_req_t *req)
{
    app_state_t st;
    char broker_uri[MQTT_URI_MAX];
    char topic_state[MQTT_TOPIC_MAX];
    char topic_cmd[MQTT_TOPIC_MAX];
    char topic_power[MQTT_TOPIC_MAX];
    char topic_timer[MQTT_TOPIC_MAX];
    char topic_switch[MQTT_TOPIC_MAX];
    char topic_status[MQTT_TOPIC_MAX];
    char topic_config[MQTT_TOPIC_MAX];
    char topic_request[MQTT_TOPIC_MAX];
    char topic_avail[MQTT_TOPIC_MAX];
    char mqtt_user[MQTT_USER_MAX];
    char mqtt_pass[MQTT_PASS_MAX];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    st = s_state;
    strlcpy(broker_uri, s_mqtt_broker_uri, sizeof(broker_uri));
    strlcpy(topic_state, s_mqtt_topic_state, sizeof(topic_state));
    strlcpy(topic_cmd, s_mqtt_topic_cmd, sizeof(topic_cmd));
    strlcpy(topic_power, s_mqtt_topic_power, sizeof(topic_power));
    strlcpy(topic_timer, s_mqtt_topic_timer, sizeof(topic_timer));
    strlcpy(topic_switch, s_mqtt_topic_switch, sizeof(topic_switch));
    strlcpy(topic_status, s_mqtt_topic_status, sizeof(topic_status));
    strlcpy(topic_config, s_mqtt_topic_config, sizeof(topic_config));
    strlcpy(topic_request, s_mqtt_topic_request, sizeof(topic_request));
    strlcpy(topic_avail, s_mqtt_topic_avail, sizeof(topic_avail));
    strlcpy(mqtt_user, s_mqtt_username, sizeof(mqtt_user));
    strlcpy(mqtt_pass, s_mqtt_password, sizeof(mqtt_pass));
    xSemaphoreGive(s_state_mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temp_c", st.temp_c);
    cJSON_AddNumberToObject(root, "hum_pct", st.hum_pct);
    cJSON_AddNumberToObject(root, "target_c", st.target_c);
    cJSON_AddNumberToObject(root, "window_c", st.window_c);
    cJSON_AddNumberToObject(root, "sensor_offset_c", st.sensor_offset_c);
    cJSON_AddBoolToObject(root, "heater_on", st.heater_on);
    cJSON_AddBoolToObject(root, "heater_request_on", st.heater_request_on);
    cJSON_AddBoolToObject(root, "heater_feedback_on", st.heater_feedback_on);
    cJSON_AddBoolToObject(root, "heater_feedback_valid", st.heater_feedback_valid);
    cJSON_AddStringToObject(root, "mode", mode_to_string(st.mode));
    cJSON_AddStringToObject(root, "effective_mode", mode_to_string(st.effective_mode));
    cJSON_AddStringToObject(root, "heat_reason", st.heat_reason);
    cJSON_AddBoolToObject(root, "automation_remote", st.automation_remote);
    cJSON_AddBoolToObject(root, "local_fallback_enabled", st.local_fallback_enabled);
    cJSON_AddBoolToObject(root, "sensor_ok", st.sensor_ok);
    cJSON_AddBoolToObject(root, "mqtt_connected", st.mqtt_connected);
    cJSON_AddNumberToObject(root, "power_w", st.power_w);
    cJSON_AddNumberToObject(root, "energy_today_kwh", st.energy_today_kwh);
    cJSON_AddNumberToObject(root, "session_energy_kwh", st.session_energy_kwh);
    cJSON_AddNumberToObject(root, "session_cost_eur", st.session_cost_eur);
    cJSON_AddBoolToObject(root, "session_from_iobroker", st.session_from_iobroker);
    cJSON_AddNumberToObject(root, "price_eur_kwh", st.price_eur_kwh);
    cJSON_AddBoolToObject(root, "timer_enabled", st.timer_enabled);
    char timer_start[6], timer_end[6];
    format_hhmm(st.timer_start_min, timer_start);
    format_hhmm(st.timer_end_min, timer_end);
    cJSON_AddStringToObject(root, "timer_start", timer_start);
    cJSON_AddStringToObject(root, "timer_end", timer_end);
    cJSON_AddStringToObject(root, "timer_mode_in", mode_to_string(st.timer_mode_in));
    cJSON_AddStringToObject(root, "timer_mode_out", mode_to_string(st.timer_mode_out));
    cJSON_AddStringToObject(root, "mqtt_broker_uri", broker_uri);
    cJSON_AddStringToObject(root, "mqtt_username", mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_password", mqtt_pass);
    cJSON_AddStringToObject(root, "mqtt_topic_state", topic_state);
    cJSON_AddStringToObject(root, "mqtt_topic_cmd", topic_cmd);
    cJSON_AddStringToObject(root, "mqtt_topic_power", topic_power);
    cJSON_AddStringToObject(root, "mqtt_topic_timer", topic_timer);
    cJSON_AddStringToObject(root, "mqtt_topic_switch", topic_switch);
    cJSON_AddStringToObject(root, "mqtt_topic_status", topic_status);
    cJSON_AddStringToObject(root, "mqtt_topic_config", topic_config);
    cJSON_AddStringToObject(root, "mqtt_topic_request", topic_request);
    cJSON_AddStringToObject(root, "mqtt_topic_avail", topic_avail);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return ret;
}

static esp_err_t http_api_config_post(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
    }

    char *buf = calloc((size_t)len + 1U, 1);
    if (!buf) {
        return httpd_resp_send_500(req);
    }

    int r = httpd_req_recv(req, buf, len);
    if (r <= 0) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
    }
    buf[r] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    cJSON *target = cJSON_GetObjectItem(root, "target_c");
    cJSON *window = cJSON_GetObjectItem(root, "window_c");
    if (!cJSON_IsNumber(target) || !cJSON_IsNumber(window)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target_c/window_c missing");
    }

    float target_c = (float)target->valuedouble;
    float window_c = (float)window->valuedouble;

    if (target_c < 10.0f) target_c = 10.0f;
    if (target_c > 35.0f) target_c = 35.0f;
    if (window_c < 0.2f) window_c = 0.2f;
    if (window_c > 10.0f) window_c = 10.0f;

    cJSON *mqtt_broker = cJSON_GetObjectItem(root, "mqtt_broker_uri");
    cJSON *mqtt_user = cJSON_GetObjectItem(root, "mqtt_username");
    cJSON *mqtt_pass = cJSON_GetObjectItem(root, "mqtt_password");
    cJSON *mqtt_state = cJSON_GetObjectItem(root, "mqtt_topic_state");
    cJSON *mqtt_cmd = cJSON_GetObjectItem(root, "mqtt_topic_cmd");
    cJSON *mqtt_power = cJSON_GetObjectItem(root, "mqtt_topic_power");
    cJSON *mqtt_timer = cJSON_GetObjectItem(root, "mqtt_topic_timer");
    cJSON *mqtt_switch = cJSON_GetObjectItem(root, "mqtt_topic_switch");
    cJSON *mqtt_status = cJSON_GetObjectItem(root, "mqtt_topic_status");
    cJSON *mqtt_config = cJSON_GetObjectItem(root, "mqtt_topic_config");
    cJSON *mqtt_request = cJSON_GetObjectItem(root, "mqtt_topic_request");
    cJSON *mqtt_avail = cJSON_GetObjectItem(root, "mqtt_topic_avail");
    cJSON *timer_enabled = cJSON_GetObjectItem(root, "timer_enabled");
    cJSON *timer_start = cJSON_GetObjectItem(root, "timer_start");
    cJSON *timer_end = cJSON_GetObjectItem(root, "timer_end");
    cJSON *timer_mode_in = cJSON_GetObjectItem(root, "timer_mode_in");
    cJSON *timer_mode_out = cJSON_GetObjectItem(root, "timer_mode_out");
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    cJSON *automation_remote = cJSON_GetObjectItem(root, "automation_remote");
    cJSON *local_fallback = cJSON_GetObjectItem(root, "local_fallback_enabled");
    cJSON *sensor_offset = cJSON_GetObjectItem(root, "sensor_offset_c");
    cJSON *price_eur_kwh = cJSON_GetObjectItem(root, "price_eur_kwh");
    bool mqtt_changed = false;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.target_c = target_c;
    s_state.window_c = window_c;
    if (cJSON_IsNumber(sensor_offset)) {
        float off = (float)sensor_offset->valuedouble;
        if (off < -10.0f) off = -10.0f;
        if (off > 10.0f) off = 10.0f;
        s_state.sensor_offset_c = off;
    }
    if (cJSON_IsString(mode) && mode->valuestring) {
        s_state.mode = mode_from_string(mode->valuestring, s_state.mode);
    }
    if (cJSON_IsBool(automation_remote)) {
        s_state.automation_remote = cJSON_IsTrue(automation_remote);
    }
    if (cJSON_IsBool(local_fallback)) {
        s_state.local_fallback_enabled = cJSON_IsTrue(local_fallback);
    }
    if (cJSON_IsString(mqtt_broker) && mqtt_broker->valuestring && mqtt_broker->valuestring[0] != '\0' &&
        strncmp(s_mqtt_broker_uri, mqtt_broker->valuestring, sizeof(s_mqtt_broker_uri) - 1) != 0) {
        strlcpy(s_mqtt_broker_uri, mqtt_broker->valuestring, sizeof(s_mqtt_broker_uri));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_user) && mqtt_user->valuestring &&
        strncmp(s_mqtt_username, mqtt_user->valuestring, sizeof(s_mqtt_username) - 1) != 0) {
        strlcpy(s_mqtt_username, mqtt_user->valuestring, sizeof(s_mqtt_username));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_pass) && mqtt_pass->valuestring &&
        strncmp(s_mqtt_password, mqtt_pass->valuestring, sizeof(s_mqtt_password) - 1) != 0) {
        strlcpy(s_mqtt_password, mqtt_pass->valuestring, sizeof(s_mqtt_password));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_state) && mqtt_state->valuestring && mqtt_state->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_state, mqtt_state->valuestring, sizeof(s_mqtt_topic_state) - 1) != 0) {
        strlcpy(s_mqtt_topic_state, mqtt_state->valuestring, sizeof(s_mqtt_topic_state));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_cmd) && mqtt_cmd->valuestring && mqtt_cmd->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_cmd, mqtt_cmd->valuestring, sizeof(s_mqtt_topic_cmd) - 1) != 0) {
        strlcpy(s_mqtt_topic_cmd, mqtt_cmd->valuestring, sizeof(s_mqtt_topic_cmd));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_power) && mqtt_power->valuestring && mqtt_power->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_power, mqtt_power->valuestring, sizeof(s_mqtt_topic_power) - 1) != 0) {
        strlcpy(s_mqtt_topic_power, mqtt_power->valuestring, sizeof(s_mqtt_topic_power));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_timer) && mqtt_timer->valuestring && mqtt_timer->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_timer, mqtt_timer->valuestring, sizeof(s_mqtt_topic_timer) - 1) != 0) {
        strlcpy(s_mqtt_topic_timer, mqtt_timer->valuestring, sizeof(s_mqtt_topic_timer));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_switch) && mqtt_switch->valuestring && mqtt_switch->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_switch, mqtt_switch->valuestring, sizeof(s_mqtt_topic_switch) - 1) != 0) {
        strlcpy(s_mqtt_topic_switch, mqtt_switch->valuestring, sizeof(s_mqtt_topic_switch));
        mqtt_changed = true;
        s_switch_topic_last_valid = false;
    }
    if (cJSON_IsString(mqtt_status) && mqtt_status->valuestring && mqtt_status->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_status, mqtt_status->valuestring, sizeof(s_mqtt_topic_status) - 1) != 0) {
        strlcpy(s_mqtt_topic_status, mqtt_status->valuestring, sizeof(s_mqtt_topic_status));
        mqtt_changed = true;
    }
    if (cJSON_IsString(mqtt_config) && mqtt_config->valuestring && mqtt_config->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_config, mqtt_config->valuestring, sizeof(s_mqtt_topic_config) - 1) != 0) {
        strlcpy(s_mqtt_topic_config, mqtt_config->valuestring, sizeof(s_mqtt_topic_config));
    }
    if (cJSON_IsString(mqtt_request) && mqtt_request->valuestring && mqtt_request->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_request, mqtt_request->valuestring, sizeof(s_mqtt_topic_request) - 1) != 0) {
        strlcpy(s_mqtt_topic_request, mqtt_request->valuestring, sizeof(s_mqtt_topic_request));
        s_request_topic_last_valid = false;
    }
    if (cJSON_IsString(mqtt_avail) && mqtt_avail->valuestring && mqtt_avail->valuestring[0] != '\0' &&
        strncmp(s_mqtt_topic_avail, mqtt_avail->valuestring, sizeof(s_mqtt_topic_avail) - 1) != 0) {
        strlcpy(s_mqtt_topic_avail, mqtt_avail->valuestring, sizeof(s_mqtt_topic_avail));
    }
    if (cJSON_IsBool(timer_enabled)) {
        s_state.timer_enabled = cJSON_IsTrue(timer_enabled);
    }
    if (cJSON_IsString(timer_start) && timer_start->valuestring) {
        uint16_t m = 0;
        if (parse_hhmm_to_min(timer_start->valuestring, &m)) {
            s_state.timer_start_min = m;
        }
    }
    if (cJSON_IsString(timer_end) && timer_end->valuestring) {
        uint16_t m = 0;
        if (parse_hhmm_to_min(timer_end->valuestring, &m)) {
            s_state.timer_end_min = m;
        }
    }
    if (cJSON_IsString(timer_mode_in) && timer_mode_in->valuestring) {
        s_state.timer_mode_in = mode_from_string(timer_mode_in->valuestring, s_state.timer_mode_in);
    }
    if (cJSON_IsString(timer_mode_out) && timer_mode_out->valuestring) {
        s_state.timer_mode_out = mode_from_string(timer_mode_out->valuestring, s_state.timer_mode_out);
    }
    if (cJSON_IsNumber(price_eur_kwh)) {
        float p = (float)price_eur_kwh->valuedouble;
        if (p < 0.05f) p = 0.05f;
        if (p > 2.00f) p = 2.00f;
        s_state.price_eur_kwh = p;
    }
    update_control_locked(&s_state);
    xSemaphoreGive(s_state_mutex);
    app_cfg_save();

    cJSON_Delete(root);
    if (mqtt_changed) {
        mqtt_restart();
    }
    mqtt_publish_state();
    return httpd_resp_sendstr(req, "ok");
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 10240;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = http_index_get,
            .user_ctx = NULL,
        };
        httpd_uri_t api_state = {
            .uri = "/api/state",
            .method = HTTP_GET,
            .handler = http_api_state_get,
            .user_ctx = NULL,
        };
        httpd_uri_t api_config = {
            .uri = "/api/config",
            .method = HTTP_POST,
            .handler = http_api_config_post,
            .user_ctx = NULL,
        };

        httpd_register_uri_handler(server, &index);
        httpd_register_uri_handler(server, &api_state);
        httpd_register_uri_handler(server, &api_config);
    }

    return server;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    char topic_cmd[MQTT_TOPIC_MAX];
    char topic_power[MQTT_TOPIC_MAX];
    char topic_timer[MQTT_TOPIC_MAX];
    char topic_status[MQTT_TOPIC_MAX];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(topic_cmd, s_mqtt_topic_cmd, sizeof(topic_cmd));
    strlcpy(topic_power, s_mqtt_topic_power, sizeof(topic_power));
    strlcpy(topic_timer, s_mqtt_topic_timer, sizeof(topic_timer));
    strlcpy(topic_status, s_mqtt_topic_status, sizeof(topic_status));
    xSemaphoreGive(s_state_mutex);

    if (event_id == MQTT_EVENT_CONNECTED) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.mqtt_connected = true;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "MQTT verbunden, cmd=%s power=%s timer=%s status=%s", topic_cmd, topic_power, topic_timer, topic_status);
        esp_mqtt_client_subscribe(s_mqtt_client, topic_cmd, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, topic_power, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, topic_timer, 1);
        esp_mqtt_client_subscribe(s_mqtt_client, topic_status, 1);
        mqtt_publish_state();
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.mqtt_connected = false;
        xSemaphoreGive(s_state_mutex);
    } else if (event_id == MQTT_EVENT_DATA) {
        char topic[128] = {0};

        int tl = event->topic_len < (int)(sizeof(topic) - 1) ? event->topic_len : (int)(sizeof(topic) - 1);
        memcpy(topic, event->topic, tl);

        if (strcmp(topic, topic_cmd) == 0) {
            int cap = event->data_len + 1;
            if (cap < 2) cap = 2;
            char *data = calloc((size_t)cap, 1);
            if (!data) {
                return;
            }
            memcpy(data, event->data, (size_t)event->data_len);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (strcasecmp(data, "ON") == 0) {
                s_state.mode = HEAT_MODE_MANUAL_ON;
            } else if (strcasecmp(data, "OFF") == 0) {
                s_state.mode = HEAT_MODE_MANUAL_OFF;
            } else if (strcasecmp(data, "AUTO") == 0) {
                s_state.mode = HEAT_MODE_AUTO;
            } else if (strcasecmp(data, "MANUAL_ON") == 0) {
                s_state.mode = HEAT_MODE_MANUAL_ON;
            } else if (strcasecmp(data, "MANUAL_OFF") == 0) {
                s_state.mode = HEAT_MODE_MANUAL_OFF;
            } else if (strcasecmp(data, "TEST_TOGGLE") == 0) {
                s_state.mode = s_state.heater_request_on ? HEAT_MODE_MANUAL_OFF : HEAT_MODE_MANUAL_ON;
            } else {
                cJSON *cmd = cJSON_Parse(data);
                if (cmd) {
                    cJSON *mode = cJSON_GetObjectItemCaseSensitive(cmd, "mode");
                    cJSON *test_toggle = cJSON_GetObjectItemCaseSensitive(cmd, "test_toggle");
                    if (cJSON_IsString(mode) && mode->valuestring) {
                        s_state.mode = mode_from_string(mode->valuestring, s_state.mode);
                    } else if (cJSON_IsBool(test_toggle) && cJSON_IsTrue(test_toggle)) {
                        s_state.mode = s_state.heater_request_on ? HEAT_MODE_MANUAL_OFF : HEAT_MODE_MANUAL_ON;
                    }
                    cJSON_Delete(cmd);
                }
            }
            update_control_locked(&s_state);
            xSemaphoreGive(s_state_mutex);
            app_cfg_save();
            free(data);
            mqtt_publish_state();
        } else if (strcmp(topic, topic_power) == 0) {
            float p = 0.0f;
            if (parse_power_payload(event->data, event->data_len, &p) && p >= 0.0f && p < 50000.0f) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                energy_integrate_locked(esp_timer_get_time());
                s_state.power_w = p;
                xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "MQTT power update: %.1f W", p);
            }
        } else if (strcmp(topic, topic_timer) == 0) {
            int cap = event->data_len + 1;
            if (cap < 2) cap = 2;
            char *data = calloc((size_t)cap, 1);
            if (!data) {
                return;
            }
            memcpy(data, event->data, (size_t)event->data_len);
            cJSON *root = cJSON_Parse(data);
            if (root) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
                cJSON *start = cJSON_GetObjectItemCaseSensitive(root, "start");
                cJSON *end = cJSON_GetObjectItemCaseSensitive(root, "end");
                cJSON *mode_in = cJSON_GetObjectItemCaseSensitive(root, "modeInWindow");
                cJSON *mode_out = cJSON_GetObjectItemCaseSensitive(root, "modeOutWindow");
                if (cJSON_IsBool(enabled)) {
                    s_state.timer_enabled = cJSON_IsTrue(enabled);
                }
                if (cJSON_IsString(start) && start->valuestring) {
                    uint16_t m = 0;
                    if (parse_hhmm_to_min(start->valuestring, &m)) s_state.timer_start_min = m;
                }
                if (cJSON_IsString(end) && end->valuestring) {
                    uint16_t m = 0;
                    if (parse_hhmm_to_min(end->valuestring, &m)) s_state.timer_end_min = m;
                }
                if (cJSON_IsString(mode_in) && mode_in->valuestring) {
                    s_state.timer_mode_in = mode_from_string(mode_in->valuestring, s_state.timer_mode_in);
                }
                if (cJSON_IsString(mode_out) && mode_out->valuestring) {
                    s_state.timer_mode_out = mode_from_string(mode_out->valuestring, s_state.timer_mode_out);
                }
                update_control_locked(&s_state);
                xSemaphoreGive(s_state_mutex);
                cJSON_Delete(root);
                ESP_LOGI(TAG, "MQTT timer config applied");
                mqtt_publish_state();
            }
            free(data);
        } else if (strcmp(topic, topic_status) == 0) {
            int cap = event->data_len + 1;
            if (cap < 2) cap = 2;
            char *data = calloc((size_t)cap, 1);
            if (!data) {
                return;
            }
            memcpy(data, event->data, (size_t)event->data_len);

            bool state_ok = false;
            bool heater_state = false;
            bool update_publish = false;

            cJSON *root = cJSON_Parse(data);
            if (root) {
                cJSON *heater = cJSON_GetObjectItemCaseSensitive(root, "heater_state");
                if (!heater) heater = cJSON_GetObjectItemCaseSensitive(root, "heater_on");
                if (!heater) heater = cJSON_GetObjectItemCaseSensitive(root, "state");
                if (cJSON_IsBool(heater)) {
                    heater_state = cJSON_IsTrue(heater);
                    state_ok = true;
                } else if (cJSON_IsString(heater) && heater->valuestring) {
                    state_ok = parse_bool_text(heater->valuestring, &heater_state);
                }

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                if (state_ok) {
                    s_state.heater_feedback_on = heater_state;
                    s_state.heater_feedback_valid = true;
                    update_publish = true;
                }
                cJSON *mode_state = cJSON_GetObjectItemCaseSensitive(root, "mode_state");
                if (cJSON_IsString(mode_state) && mode_state->valuestring) {
                    s_state.effective_mode = mode_from_string(mode_state->valuestring, s_state.effective_mode);
                    update_publish = true;
                }
                cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
                if (cJSON_IsString(reason) && reason->valuestring) {
                    strlcpy(s_state.heat_reason, reason->valuestring, sizeof(s_state.heat_reason));
                    update_publish = true;
                }
                cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power_w");
                if (cJSON_IsNumber(power)) {
                    s_state.power_w = (float)power->valuedouble;
                    update_publish = true;
                }
                cJSON *sess_kwh = cJSON_GetObjectItemCaseSensitive(root, "session_kwh");
                if (!sess_kwh) sess_kwh = cJSON_GetObjectItemCaseSensitive(root, "session_energy_kwh");
                if (cJSON_IsNumber(sess_kwh)) {
                    s_state.session_energy_kwh = (float)sess_kwh->valuedouble;
                    s_state.session_from_iobroker = true;
                    update_publish = true;
                }
                cJSON *sess_cost = cJSON_GetObjectItemCaseSensitive(root, "session_cost_eur");
                if (!sess_cost) sess_cost = cJSON_GetObjectItemCaseSensitive(root, "cost_eur");
                if (cJSON_IsNumber(sess_cost)) {
                    s_state.session_cost_eur = (float)sess_cost->valuedouble;
                    s_state.session_from_iobroker = true;
                    update_publish = true;
                }
                s_state.last_status_rx_ms = esp_timer_get_time() / 1000;
                update_control_locked(&s_state);
                xSemaphoreGive(s_state_mutex);

                cJSON_Delete(root);
            } else {
                bool parsed = parse_bool_text(data, &heater_state);
                if (parsed) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_state.heater_feedback_on = heater_state;
                    s_state.heater_feedback_valid = true;
                    s_state.last_status_rx_ms = esp_timer_get_time() / 1000;
                    update_control_locked(&s_state);
                    xSemaphoreGive(s_state_mutex);
                    update_publish = true;
                }
            }
            free(data);

            if (update_publish) {
                mqtt_publish_state();
            }
        }
    }
}

static bool parse_power_payload(const char *payload, int len, float *out_watts)
{
    if (!payload || len <= 0 || !out_watts) {
        return false;
    }

    char *buf = calloc((size_t)len + 1U, 1);
    if (!buf) {
        return false;
    }
    memcpy(buf, payload, (size_t)len);

    char *end = NULL;
    float v = strtof(buf, &end);
    if (end != buf) {
        *out_watts = v;
        free(buf);
        return true;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        const char *keys[] = {"val", "value", "power", "load_power", "watts", "w"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            cJSON *n = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
            if (cJSON_IsNumber(n)) {
                *out_watts = (float)n->valuedouble;
                cJSON_Delete(root);
                free(buf);
                return true;
            }
        }
        cJSON_Delete(root);
    }

    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
            float f = strtof(&buf[i], &end);
            if (end != &buf[i]) {
                *out_watts = f;
                free(buf);
                return true;
            }
        }
    }

    free(buf);
    return false;
}

static void mqtt_start(void)
{
    mqtt_restart();
}

static void i2c_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0));
}

static void lcd_init(void)
{
    display_tft_init();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    s_state_mutex = xSemaphoreCreateMutex();
    app_cfg_load();

    gpio_config_t relay_cfg = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_cfg);
    relay_set(false);

    i2c_init();
    bool aht_ok = aht10_init();
    ESP_LOGI(TAG, "AHT10 init: %s", aht_ok ? "OK" : "FAIL");

    lcd_init();

#if DISPLAY_DIAG_MODE
    while (1) {
        lcd_clear(0xF800);
        lcd_draw_text(2, 2, "RED", 0xFFFF);
        lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(1000));

        lcd_clear(0x07E0);
        lcd_draw_text(2, 2, "GREEN", 0xFFFF);
        lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(1000));

        lcd_clear(0x001F);
        lcd_draw_text(2, 2, "BLUE", 0xFFFF);
        lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(1000));

        lcd_draw_diag_bars();
        lcd_draw_text(2, 2, "BARS", 0xFFE0);
        lcd_flush();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    wifi_init_sta();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    time_sync_init();

    start_webserver();
    mqtt_start();

    xTaskCreate(control_task, "control_task", 4096, NULL, 5, NULL);
}
