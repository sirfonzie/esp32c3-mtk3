#
# espmesh — reusable ESP-MESH stack (ESP-IDF side, MTK3-agnostic).
#
# Self-contained on purpose: it does NOT require mtkernel, so it can be reused
# from a plain IDF app, and so the shared MTK3 components stay untouched by this
# experiment.  If libesp_mesh.a needs FreeRTOS symbols the shim lacks
# (eTaskGetState, xTaskAbortDelay), add a component-local stub file here rather
# than editing the shared components/freertos_shim.
#
idf_component_register(
    SRCS
        "espmesh.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi          # ESP-MESH API (esp_mesh.h) lives here
        esp_netif
        esp_event
        nvs_flash
        esp_rom
        esp_system
        wpa_supplicant
)
