# ble_nimble — NimBLE-only component, intentionally NO mtkernel dependency.
# See ble_beacon's ble_nimble/CMakeLists.txt for the reason.
idf_component_register(SRCS "ble_gatt.c"
                       INCLUDE_DIRS ""
                       REQUIRES bt nvs_flash esp_rom esp_timer)
