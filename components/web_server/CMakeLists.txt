idf_component_register(
    SRCS
        "web_server.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        config
        esp_http_server
        esp_timer
        spiffs
        esp_wifi
        nvs_flash
        driver
        ir_control
    PRIV_REQUIRES
        esp_timer
)

# Existing web_server.c targets ESP-IDF 5.3 WebSocket APIs. The compatibility
# header supplies declarations used by the legacy implementation.
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include${CMAKE_CURRENT_LIST_DIR}/web_server_compat.h"
    "-Wno-error=format"
)
