cmake_minimum_required(VERSION 3.16)

# Skip entirely when neither WiFi nor BLE is selected.
if(NOT CONFIG_MTK3_WIFI_ENABLED AND NOT CONFIG_MTK3_BLE_ENABLED)
    idf_component_register()
    return()
endif()

#
# ble_stack — WiFi init and/or NimBLE host + BT controller init (ESP-IDF side)
#
# This component intentionally does NOT require mtkernel. The µT-Kernel
# include tree contains include/sys/queue.h (a doubly-linked ring queue
# for µT-Kernel internals) which shadows the toolchain's BSD sys/queue.h
# when both include paths are present. NimBLE headers require the BSD
# sys/queue.h for STAILQ_ENTRY / SLIST_ENTRY etc.
#
# Keeping this component mtkernel-free means the correct sys/queue.h is
# found. µT-Kernel primitives (tk_dly_tsk, tk_ext_tsk) are forward-
# declared using their raw ABI types instead.
#

set(srcs "wifi_init.c")
set(requires nvs_flash esp_rom esp_timer esp_system esp_wifi esp_netif esp_event)

if(CONFIG_MTK3_BLE_ENABLED)
    list(APPEND srcs "ble_init.c")
    list(APPEND requires bt)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ""
    REQUIRES ${requires}
)

# IDF v6.1 workaround: REQUIRES-based include propagation is broken for
# custom project components in this dev build. Explicitly link the IDF
# component targets so their INTERFACE_INCLUDE_DIRECTORIES are applied.
set(link_libs
    idf::nvs_flash
    idf::esp_rom
    idf::esp_timer
    idf::esp_system
    idf::esp_wifi
    idf::esp_netif
    idf::esp_event
)
if(CONFIG_MTK3_BLE_ENABLED)
    list(APPEND link_libs idf::bt)
endif()
target_link_libraries(${COMPONENT_LIB} PUBLIC ${link_libs})
