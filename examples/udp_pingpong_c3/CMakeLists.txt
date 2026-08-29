cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components/freertos"
    "../../components/mtkernel"
    "../../components/freertos_shim")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(udp_pingpong_c3)
