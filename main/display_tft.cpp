#include "display_tft.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "fonts/NotoSansBold15.h"
#include "fonts/NotoSansBold36.h"

static TFT_eSPI tft;
static TFT_eSprite spr = TFT_eSprite(&tft);
static bool s_display_ready = false;

static constexpr int TFT_ROTATION = 1;
static constexpr int DISP_W = 284;
static constexpr int DISP_H = 76;
static constexpr int TFT_BL_PIN = 27;

void display_tft_init(void)
{
    initArduino();
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);
    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.invertDisplay(false);
    tft.fillScreen(TFT_BLACK);
    spr.setColorDepth(16);
    spr.createSprite(DISP_W, DISP_H);

    s_display_ready = true;
}

void display_tft_render(float temp_c,
                        float hum_pct,
                        float target_c,
                        float window_c,
                        bool heater_on,
                        int mode,
                        bool sensor_ok,
                        float power_w,
                        float energy_today_kwh,
                        float price_eur_kwh,
                        bool timer_enabled,
                        uint16_t timer_start_min,
                        uint16_t timer_end_min)
{
    if (!s_display_ready) {
        return;
    }

    const float target_min = target_c - (window_c * 0.5f);
    const float target_max = target_c + (window_c * 0.5f);
    const float cost = energy_today_kwh * price_eur_kwh;
    const bool pulse = ((millis() / 400U) & 1U) != 0U;

    char temp_str[16];
    char rh_set_str[40];
    char mode_str[8];
    char heat_str[8];
    char cost_str[18];
    char power_str[14];
    char day_str[16];
    char timer_str[20];

    snprintf(temp_str, sizeof(temp_str), "%.1fC", temp_c);
    snprintf(rh_set_str, sizeof(rh_set_str), "H:%.0f%% Z:%.1f-%.1f", hum_pct, target_min, target_max);
    snprintf(mode_str, sizeof(mode_str), "%s", mode == 0 ? "AUTO" : "MAN");
    snprintf(heat_str, sizeof(heat_str), "%s", heater_on ? "HEIZT" : "AUS");
    snprintf(cost_str, sizeof(cost_str), "%.2f EUR", cost);
    snprintf(power_str, sizeof(power_str), "%.0f W", power_w);
    snprintf(day_str, sizeof(day_str), "%.3f kWh", energy_today_kwh);
    if (timer_enabled) {
        snprintf(timer_str, sizeof(timer_str), "T %02u:%02u-%02u:%02u",
                 (unsigned)(timer_start_min / 60U) % 24U, (unsigned)(timer_start_min % 60U),
                 (unsigned)(timer_end_min / 60U) % 24U, (unsigned)(timer_end_min % 60U));
    } else {
        snprintf(timer_str, sizeof(timer_str), "T AUS");
    }

    const uint16_t bg_global = tft.color565(8, 11, 18);
    const uint16_t bg_status = tft.color565(30, 33, 40);
    uint16_t bg_main = heater_on ? tft.color565(26, 14, 10) : tft.color565(8, 16, 30);
    const uint16_t bg_cost = tft.color565(28, 24, 10);
    const uint16_t line = tft.color565(55, 65, 86);

    if (heater_on && temp_c > target_max) {
        bg_main = tft.color565(45, 12, 12);
    }

    const uint16_t temp_col = heater_on
        ? (pulse ? tft.color565(255, 136, 46) : tft.color565(255, 91, 52))
        : tft.color565(125, 210, 255);

    const int xA = 0,   wA = 66;
    const int xB = 68,  wB = 134;
    const int xC = 204, wC = 80;

    spr.fillSprite(bg_global);
    spr.fillRoundRect(xA, 0, wA, DISP_H, 6, bg_status);
    spr.fillRoundRect(xB, 0, wB, DISP_H, 6, bg_main);
    spr.fillRoundRect(xC, 0, wC, DISP_H, 6, bg_cost);
    spr.drawFastVLine(xA + wA + 1, 3, DISP_H - 6, line);
    spr.drawFastVLine(xB + wB + 1, 3, DISP_H - 6, line);

    spr.loadFont(NotoSansBold15);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(tft.color565(185, 190, 200), bg_status, true);
    spr.drawString("MODUS", xA + 6, 4);
    spr.setTextColor(TFT_WHITE, bg_status, true);
    spr.drawString(mode_str, xA + 6, 22);

    const uint16_t badge = heater_on ? tft.color565(196, 30, 40) : tft.color565(72, 78, 92);
    spr.fillRoundRect(xA + 6, 49, 54, 20, 5, badge);
    spr.setTextColor(TFT_WHITE, badge, true);
    spr.drawString(heat_str, xA + 12, 52);

    spr.setTextColor(tft.color565(192, 208, 226), bg_main, true);
    spr.drawString(rh_set_str, xB + 7, 53);

    spr.setTextColor(tft.color565(250, 210, 90), bg_cost, true);
    spr.drawString("KOSTEN", xC + 5, 4);
    spr.setTextColor(TFT_WHITE, bg_cost, true);
    spr.drawString(cost_str, xC + 5, 23);
    spr.setTextColor(tft.color565(175, 185, 195), bg_cost, true);
    spr.drawString(power_str, xC + 5, 41);
    spr.drawString(day_str, xC + 5, 57);
    spr.unloadFont();

    spr.setTextFont(1);
    spr.setTextColor(tft.color565(160, 175, 192), bg_status);
    spr.drawString(timer_str, xA + 4, 42);

    spr.loadFont(NotoSansBold36);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(temp_col, bg_main, true);
    spr.drawString(temp_str, xB + (wB / 2), 25);
    spr.unloadFont();

    if (!sensor_ok) {
        const uint16_t err_bg = tft.color565(150, 20, 20);
        spr.fillRoundRect(xB + 20, 30, 94, 16, 4, err_bg);
        spr.loadFont(NotoSansBold15);
        spr.setTextDatum(TL_DATUM);
        spr.setTextColor(TFT_WHITE, err_bg, true);
        spr.drawString("SENSOR FEHLER", xB + 24, 31);
        spr.unloadFont();
    }

    spr.pushSprite(0, 0);
}
