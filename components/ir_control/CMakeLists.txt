idf_component_register(
    SRCS
        "ir_control.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        config
        esp_driver_uart
        nvs_flash
        esp_system
    PRIV_REQUIRES
        esp_timer
)

# ir_control.c is intentionally built with strict -Werror=all.
# Keep the component portable across GCC versions where compact single-line
# guard statements trigger -Wmisleading-indentation without changing codegen.
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=misleading-indentation")
