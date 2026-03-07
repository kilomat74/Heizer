idf_component_register(
    SRCS "main.c" "display_tft.cpp" "log_wrap_compat.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_http_server mqtt driver json esp_lcd arduino TFT_eSPI
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u __wrap_esp_log_write"
    "-u __wrap_esp_log_writev"
)
