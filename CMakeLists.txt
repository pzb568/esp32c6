cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ir_control"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/mqtt_client"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ai_engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/web_server"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_home_terminal)
