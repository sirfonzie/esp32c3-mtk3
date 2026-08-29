#pragma once
/*
 * FreeRTOS.h — minimal compatibility header for the freertos_shim component.
 *
 * IDF's network stack includes <freertos/FreeRTOS.h> throughout.  This shim
 * header provides just enough type and macro definitions so those files compile
 * without the real FreeRTOS kernel headers.  It does NOT pull in any FreeRTOS
 * kernel source; the shim sources (shim_*.c) back every runtime call onto
 * µT-Kernel primitives.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* IDF expects these to be defined by FreeRTOSConfig.h via FreeRTOS.h */
#define configTICK_RATE_HZ           1000U
#define configMAX_PRIORITIES         25U
#define configMINIMAL_STACK_SIZE     512U
#define portMAX_DELAY                ((uint32_t)0xffffffffUL)
#define portTICK_PERIOD_MS           1U
#define portTICK_RATE_MS             portTICK_PERIOD_MS

/* Types */
typedef uint32_t  TickType_t;
typedef uint32_t  StackType_t;
typedef int32_t   BaseType_t;
typedef uint32_t  UBaseType_t;

#define pdFALSE    ((BaseType_t)0)
#define pdTRUE     ((BaseType_t)1)
#define pdPASS     pdTRUE
#define pdFAIL     pdFALSE

/* Scheduler state (xTaskGetSchedulerState return values) */
#define taskSCHEDULER_NOT_STARTED  ((BaseType_t)1)
#define taskSCHEDULER_RUNNING      ((BaseType_t)2)
#define taskSCHEDULER_SUSPENDED    ((BaseType_t)0)

/* portMUX_TYPE — IDF uses this for spinlocks.  In our single-core build it is
 * a simple uint32_t; spinlock_* functions are shimmed to DI/EI. */
typedef struct { uint32_t owner; uint32_t count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0xB33FEEFFu, 0 }
#define portMUX_FREE_VAL             0xB33FEEFFu

/* Yield from ISR — the existing __wrap_rtos_int_exit fires the actual switch;
 * this macro just signals µT-Kernel's dispatch request. */
extern void shim_yield_from_isr(BaseType_t higher_prio_woken);
#define portYIELD_FROM_ISR(x)  shim_yield_from_isr(x)
#define portEND_SWITCHING_ISR(x) portYIELD_FROM_ISR(x)

/* Critical section macros — backed by shim_port.c */
extern void  __wrap_vPortEnterCritical(void);
extern void  __wrap_vPortExitCritical(void);
#define taskENTER_CRITICAL()       __wrap_vPortEnterCritical()
#define taskEXIT_CRITICAL()        __wrap_vPortExitCritical()
#define taskENTER_CRITICAL_ISR()   __wrap_vPortEnterCritical()
#define taskEXIT_CRITICAL_ISR()    __wrap_vPortExitCritical()

/* portDISABLE_INTERRUPTS / portENABLE_INTERRUPTS */
#define portDISABLE_INTERRUPTS()  do { UINT _s; DI(_s); (void)_s; } while(0)
#define portENABLE_INTERRUPTS()   do { EI(0); } while(0)

/* Misc */
#define portNUM_CONFIGURABLE_REGIONS  0
#define configNUM_CORES               1
#define portCORE_ID_NONE              0
