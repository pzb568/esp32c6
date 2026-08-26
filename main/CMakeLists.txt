idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        config
        nvs_flash
        esp_wifi
        esp_http_server
        esp_http_client
        mqtt
        driver
        json
        spiffs
        ir_control
        audio
        mqtt_client
        ai_engine
        web_server
)

# ESP32-C6 RISC-V maps fixed-width int32_t/uint32_t to long/unsigned long.
# Keep the existing JSON formatting source compatible with ESP-IDF 5.3.
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format")
