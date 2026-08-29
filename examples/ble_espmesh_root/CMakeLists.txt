cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components/freertos"
    "../../components/mtkernel"
    "../../components/freertos_shim"
    "../../components/net/espmesh"
    "../../components/net/blebridge")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble_espmesh_root)
