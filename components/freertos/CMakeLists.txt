# components/freertos/CMakeLists.txt
# Local shadow of IDF's freertos component for the MTK3 port.
# Excludes the four FreeRTOS kernel scheduler sources (tasks.c, queue.c,
# timers.c, event_groups.c) and portasm.S. All scheduling and sync primitives
# are provided by the freertos_shim component which routes to µT-Kernel 3.

cmake_minimum_required(VERSION 3.16)

idf_build_get_property(non_os_build NON_OS_BUILD)
if(non_os_build)
    idf_component_register()
    return()
endif()

set(IDF_FR $ENV{IDF_PATH}/components/freertos)
set(KERN FreeRTOS-Kernel)
set(ARCH riscv)

# Sources: everything from IDF freertos EXCEPT:
#   - FreeRTOS-Kernel/tasks.c         (replaced by shim_task.c)
#   - FreeRTOS-Kernel/queue.c         (replaced by shim_queue.c)
#   - FreeRTOS-Kernel/timers.c        (replaced by shim_timer.c)
#   - FreeRTOS-Kernel/event_groups.c  (replaced by shim_evtgrp.c)
#   - FreeRTOS-Kernel/portable/riscv/portasm.S  (MTK3 owns mtvec/interrupt path)
set(srcs
    "${IDF_FR}/heap_idf.c"
    "${IDF_FR}/app_startup.c"
    "${IDF_FR}/port_common.c"
    "${IDF_FR}/port_systick.c"
    "${IDF_FR}/${KERN}/list.c"
    "${IDF_FR}/${KERN}/stream_buffer.c"
    "${IDF_FR}/${KERN}/portable/${ARCH}/port.c"
    "${IDF_FR}/esp_additions/idf_additions_event_groups.c"
    "${IDF_FR}/esp_additions/idf_additions.c"
)

# Public include directories — identical to IDF's original.
set(include_dirs
    "${IDF_FR}/config/include"
    "${IDF_FR}/config/include/freertos"
    "${IDF_FR}/config/${ARCH}/include"
    "${IDF_FR}/${KERN}/include"
    "${IDF_FR}/${KERN}/portable/${ARCH}/include"
    "${IDF_FR}/${KERN}/portable/${ARCH}/include/freertos"
    "${IDF_FR}/esp_additions/include"
)

# Private include directories — identical to IDF's original.
set(private_include_dirs
    "${IDF_FR}"
    "${IDF_FR}/${KERN}/include/freertos"
    "${IDF_FR}/esp_additions"
)

# Linker fragments are local copies of IDF's linker.lf / linker_common.lf.
idf_component_register(
    SRCS              ${srcs}
    INCLUDE_DIRS      ${include_dirs}
    PRIV_INCLUDE_DIRS ${private_include_dirs}
    LDFRAGMENTS       linker_common.lf linker.lf
)

# Replicate the property downstream components use to locate FreeRTOS kernel headers.
idf_component_set_property(freertos ORIG_INCLUDE_PATH "${IDF_FR}/${KERN}/include/freertos/")

# stream_buffer.c accesses internal FreeRTOS struct layout.
set_source_files_properties(
    "${IDF_FR}/${KERN}/stream_buffer.c"
    PROPERTIES COMPILE_DEFINITIONS _ESP_FREERTOS_INTERNAL
)

# Force app_main to be linked (mirrors IDF's original).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u app_main")

if(CONFIG_FREERTOS_DEBUG_OCDAWARE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--undefined=FreeRTOS_openocd_params")
endif()
