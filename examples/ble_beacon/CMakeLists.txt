cmake_minimum_required(VERSION 3.16)
# ble_nimble must precede ../../components so its local CMakeLists.txt is found first.
set(EXTRA_COMPONENT_DIRS ble_nimble ../../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mtk3c3_ble_beacon)
