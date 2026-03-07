#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_tft_init(void);
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
                        uint16_t timer_end_min);

#ifdef __cplusplus
}
#endif
