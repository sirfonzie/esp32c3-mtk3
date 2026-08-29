idf_component_register(
    SRCS "pingpong_c3.c"
    INCLUDE_DIRS "."
    REQUIRES mtkernel freertos_shim esp_wifi nvs_flash)
