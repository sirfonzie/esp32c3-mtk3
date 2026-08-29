set(MTK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

set(MTK_BOARD "iote_esp32c3_mini" CACHE STRING "micro T-Kernel board sysdepend kit")
set_property(CACHE MTK_BOARD PROPERTY STRINGS iote_esp32c3_mini iote_m5stamp_c3)

if(MTK_BOARD STREQUAL "iote_esp32c3_mini")
    set(MTK_BOARD_DEFINE _IOTE_ESP32C3_MINI_)
elseif(MTK_BOARD STREQUAL "iote_m5stamp_c3")
    set(MTK_BOARD_DEFINE _IOTE_M5STAMP_C3_)
else()
    message(FATAL_ERROR "Unsupported MTK_BOARD='${MTK_BOARD}'. Expected iote_esp32c3_mini or iote_m5stamp_c3.")
endif()

if(MTK_BOARD STREQUAL "iote_m5stamp_c3")
    if(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG OR NOT CONFIG_ESP_CONSOLE_UART_DEFAULT OR NOT "${CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM}" STREQUAL "0")
        message(FATAL_ERROR
            "MTK_BOARD=iote_m5stamp_c3 requires the UART0 console for the on-board CH9102 bridge. "
            "Use: idf.py -B build_m5stamp_c3 -DMTK_BOARD=iote_m5stamp_c3 "
            "-DSDKCONFIG=build_m5stamp_c3/sdkconfig "
            "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.m5stamp_c3 build")
    endif()
endif()

set(TKERNEL_SRCS
    cpuctl.c device.c deviceio.c eventflag.c int.c klock.c
    mailbox.c memory.c mempfix.c mempool.c messagebuf.c misc_calls.c
    mutex.c objname.c power.c rendezvous.c semaphore.c
    task.c task_manage.c task_sync.c time_calls.c timer.c tkinit.c wait.c)
list(TRANSFORM TKERNEL_SRCS PREPEND "${MTK_ROOT}/kernel/tkernel/")

set(TSTDLIB_SRCS
    bitop.c string.c)
list(TRANSFORM TSTDLIB_SRCS PREPEND "${MTK_ROOT}/kernel/tstdlib/")

set(LIBTK_SRCS
    fastlock.c fastmlock.c kmalloc.c)
list(TRANSFORM LIBTK_SRCS PREPEND "${MTK_ROOT}/lib/libtk/")

set(LIBTM_SRCS
    libtm.c libtm_printf.c)
list(TRANSFORM LIBTM_SRCS PREPEND "${MTK_ROOT}/lib/libtm/")

## kernel/usermain/usermain.c is intentionally NOT included here.  It defines a
## WEAK_FUNC usermain stub that returns 0 -- harmless in isolation, but inside
## the same archive as inittask.o it gets dragged in by the call site BEFORE
## the linker reaches the app's libmain.a, and ld doesn't upgrade weak->strong
## across archives.  Result: app's strong usermain is ignored, inittask sees a
## 0 return, prints "<< SYSTEM SHUTDOWN >>" right after the version banner.
## Dropping the stub forces every app to supply its own usermain.  A missing
## one is a loud link error instead of a silent boot.
set(SYSINIT_SRCS
    ${MTK_ROOT}/kernel/sysinit/sysinit.c
    ${MTK_ROOT}/kernel/inittask/inittask.c)

set(SYSDEPEND_SRCS
    ${MTK_ROOT}/lib/libtk/sysdepend/cpu/core/riscv_esp/int_riscv.c
    ${MTK_ROOT}/lib/libtk/sysdepend/cpu/esp32c3/ptimer_esp32c3.c
    ${MTK_ROOT}/kernel/sysdepend/cpu/core/riscv_esp/cpu_cntl.c
    ${MTK_ROOT}/kernel/sysdepend/cpu/core/riscv_esp/interrupt.c
    ${MTK_ROOT}/kernel/sysdepend/cpu/core/riscv_esp/sys_timer.c
    ${MTK_ROOT}/kernel/sysdepend/${MTK_BOARD}/devinit.c
    ${MTK_ROOT}/kernel/sysdepend/${MTK_BOARD}/hw_setting.c
    ${MTK_ROOT}/lib/libtm/sysdepend/${MTK_BOARD}/tm_com.c)

set(DEVICE_SRCS
    ${MTK_ROOT}/device/common/drvif/msdrvif.c
    ${MTK_ROOT}/device/ser/ser.c
    ${MTK_ROOT}/device/ser/sysdepend/esp32c3/ser_esp32c3.c
    ${MTK_ROOT}/device/adc/adc.c
    ${MTK_ROOT}/device/adc/sysdepend/esp32c3/adc_esp32c3.c)

idf_component_register(
    SRCS ${TKERNEL_SRCS} ${TSTDLIB_SRCS} ${LIBTK_SRCS} ${LIBTM_SRCS}
         ${SYSINIT_SRCS} ${SYSDEPEND_SRCS} ${DEVICE_SRCS}
         mtk_startup.c
    INCLUDE_DIRS
        "${MTK_ROOT}/kernel/sysdepend/${MTK_BOARD}"
        "${MTK_ROOT}/include"
        "${MTK_ROOT}/config"
    PRIV_INCLUDE_DIRS
        "${MTK_ROOT}/kernel/knlinc"
        "${MTK_ROOT}/kernel/tkernel"
        "${MTK_ROOT}/kernel/sysdepend"
        "${MTK_ROOT}/device/include"
        "${MTK_ROOT}/device/common/drvif"
    REQUIRES esp_rom esp_system esp_hw_support hal soc esp_hal_uart esp_hal_timg esp_hal_ana_conv)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ${MTK_BOARD_DEFINE}
    ADD_PREFIX_KNL_TO_GLOBAL_NAME)

## Bypass FreeRTOS scheduler startup.  Any project linking this component
## gets the wrapped esp_startup_start_app entry (the weak default in
## mtk_startup.c launches knl_main with a 32 KB heap) plus the dispatcher's
## rtos_int_exit wrap from cpu_cntl.c.  Applied via INTERFACE so the
## project-level link picks them up.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_startup_start_app"
    "-Wl,--wrap=rtos_int_exit"
    ## Panic diagnostic: __wrap_esp_panic_handler (cpu_cntl.c) prints the
    ## current uT-Kernel task before chaining to the real IDF panic handler.
    "-Wl,--wrap=esp_panic_handler")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
    -Wno-cast-function-type
    -Wno-unterminated-string-initialization
    ## -Wimplicit-fallthrough's CFG analysis is optimization-dependent: the
    ## intentional %X/%x/%o fallthrough in libtm_printf.c only trips it at -O0
    ## (T9 optimization-matrix finding; upstream file left untouched).
    -Wno-error=implicit-fallthrough)
