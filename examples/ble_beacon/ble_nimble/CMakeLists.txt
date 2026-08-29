# ble_nimble — NimBLE-only component, intentionally NO mtkernel dependency.
#
# The µT-Kernel include tree contains include/sys/queue.h which shadows the
# BSD sys/queue.h that NimBLE requires. Keeping this component mtkernel-free
# lets the compiler find the correct sys/queue.h from the toolchain.
# µT-Kernel functions are resolved at link time via forward declarations.
idf_component_register(SRCS "ble_beacon.c"
                       INCLUDE_DIRS ""
                       REQUIRES bt nvs_flash esp_rom esp_timer)
