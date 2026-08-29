set(main_srcs "main.c" "smoke_kernel_api.c" "app_multitask.c")
set(main_requires mtkernel esp_rom hal soc esp_system heap)

if(CONFIG_MTK3_WIFI_ENABLED OR CONFIG_MTK3_BLE_ENABLED)
    list(APPEND main_requires ble_stack)
endif()

if(CONFIG_MTK3_BLE_ENABLED)
    list(APPEND main_srcs "ble_task.c")
endif()

idf_component_register(
    SRCS ${main_srcs}
    INCLUDE_DIRS ""
    REQUIRES ${main_requires}
)
