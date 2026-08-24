cmake_minimum_required(VERSION 3.16)

# EXTRA_COMPONENT_DIRS 必须在 project() 之前声明
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_home_terminal)
