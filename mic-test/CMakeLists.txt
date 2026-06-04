cmake_minimum_required(VERSION 3.16)

# All vendor components live next door in the userdemo clone we did earlier.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../userdemo/components/M5IOE1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userdemo/components/M5PM1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userdemo/components/M5GFX"
    "${CMAKE_CURRENT_SOURCE_DIR}/../userdemo/components/lvgl"
)

# Stamp every build with a unix-timestamp version so esp_https_ota sees a
# different `version` field in the firmware image header on every rebuild.
string(TIMESTAMP BUILD_TS "%s" UTC)
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/version.txt "${BUILD_TS}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mic_test)
# lv_conf.h lives in userdemo/components/lvgl/ (next to LVGL src) so LVGL
# finds it via its own include path. No global flags needed.
