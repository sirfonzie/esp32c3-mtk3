cmake_minimum_required(VERSION 3.16)

#
# blebridge — NimBLE scan-all + advertise, forwarding sightings up an
# ESP-MESH uplink.  Like ble_stack, this component intentionally does NOT
# require mtkernel: the kernel include tree ships its own sys/queue.h that
# shadows the BSD sys/queue.h NimBLE needs.  The mesh uplink is used only via
# the kernel-agnostic espmesh API; shim_ble_patch_modules_funcs() is
# forward-declared and resolved at final link from freertos_shim (pulled in by
# the app's main component), so it is NOT required here.
#
idf_component_register(
    SRCS "blebridge.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        esp_rom
        esp_timer
        espmesh
)
