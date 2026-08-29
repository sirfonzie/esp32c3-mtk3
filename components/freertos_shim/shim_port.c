#include "freertos/shim_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"                       /* IRAM_ATTR */
#include "esp_private/startup_internal.h"  /* ESP_SYSTEM_INIT_FN */

/* IRAM-resident IDF helper (esp_private/cache_utils.h); forward-declared to
 * avoid adding a spi_flash include dependency to this component. */
extern bool spi_flash_cache_enabled(void);

/*
 * These port primitives MUST live in IRAM.  IDF calls them from contexts where
 * the flash cache is disabled -- specifically from IRAM-resident WiFi/coex/PHY
 * interrupt handlers that keep firing during SPI-flash write/erase windows (only
 * non-IRAM interrupts are masked then).  If a flash-resident version is called
 * with the cache off, the CPU faults with "Cache error" (observed: a coex ISR
 * calling __wrap_xPortInIsrContext mid-flash-op).  The stock IDF FreeRTOS port
 * marks the equivalents IRAM_ATTR for the same reason.
 */

/* Critical-section nesting counter.  We use µT-Kernel DI/EI (mstatus.MIE).
 *
 * Save/restore MIE state rather than force-enabling on exit.  During kernel
 * init (knl_main runs with DISABLE_INTERRUPT / MIE=0), IDF's esp_intr_alloc
 * calls vPortEnterCritical.  If we force EI(MSTATUS_MIE) on exit, MIE goes
 * to 1 mid-knl_main and a pending systimer ISR fires, triggering a premature
 * context switch that loses the boot thread.  Saving and restoring the actual
 * MIE state before the outermost enter avoids this: during kernel init the
 * saved value is 0, so exit is a no-op; in task context the saved value is 1,
 * so exit correctly re-enables interrupts.
 */
static volatile int  s_cs_depth = 0;
static volatile UINT s_cs_saved = 0;  /* MIE state before outermost enter */

IRAM_ATTR void __wrap_vPortEnterCritical(void)
{
    UINT s;
    DI(s);                          /* atomic read-clear; s = old MIE bit */
    if (s_cs_depth == 0) s_cs_saved = s;
    s_cs_depth++;
}

/* Yield to a higher-priority task that became ready while the critical section
 * or spinlock was held.  Mirrors FreeRTOS vPortExitCritical portYIELD.
 *
 * Guards:
 *  saved != 0  → MIE was 1 before entry (task context, not kernel-init)
 *  knl_dispatch_disabled == 0  → dispatch not suppressed by kernel
 *  knl_taskindp == 0  → not inside an interrupt handler
 *  knl_ctxtsk != NULL && != knl_schedtsk  → a different task should run now
 *
 * When these hold, trigger FROM_CPU_INTR0.  The interrupt fires at the next
 * instruction boundary (MIE=1 just restored), __wrap_rtos_int_exit switches
 * context, and we resume here on our way back to the caller. */
/* knl_dispatch_disabled declared in shim_internal.h */
extern void *knl_ctxtsk, *knl_schedtsk;
extern void knl_dispatch(void);

IRAM_ATTR static inline void shim_yield_if_needed(UINT saved)
{
    /* knl_dispatch() is flash-resident; never call it with the cache disabled
     * (i.e. mid SPI-flash op).  spi_flash_cache_enabled() is itself in IRAM, so
     * this guard is safe to evaluate from a cache-off context. */
    if (saved && !knl_dispatch_disabled && !knl_taskindp &&
        knl_ctxtsk && knl_schedtsk && knl_ctxtsk != knl_schedtsk &&
        spi_flash_cache_enabled()) {
        knl_dispatch();
    }
}

IRAM_ATTR void __wrap_vPortExitCritical(void)
{
    if (--s_cs_depth <= 0) {
        s_cs_depth = 0;
        EI(s_cs_saved);             /* restore pre-enter MIE state */
        shim_yield_if_needed(s_cs_saved);
    }
}

IRAM_ATTR void __wrap_vPortEnterCriticalFromISR(void)
{
    __wrap_vPortEnterCritical();
}

IRAM_ATTR void __wrap_vPortExitCriticalFromISR(void)
{
    __wrap_vPortExitCritical();
}

/* portYIELD_FROM_ISR — context switch at ISR tail is already handled by
 * __wrap_rtos_int_exit; no extra action needed here. */
IRAM_ATTR void shim_yield_from_isr(BaseType_t higher_prio_woken) { (void)higher_prio_woken; }

IRAM_ATTR BaseType_t __wrap_xPortInIsrContext(void)
{
    return shim_in_isr() ? pdTRUE : pdFALSE;
}

IRAM_ATTR BaseType_t __wrap_xPortGetCoreID(void)
{
    return 0; /* single-core ESP32-C3 */
}

IRAM_ATTR BaseType_t __wrap_xTaskGetSchedulerState(void)
{
    return taskSCHEDULER_RUNNING; /* must lie; µT-Kernel is the scheduler */
}

/* Spinlock shims — on single-core ESP32-C3 these reduce to DI/EI.
 * Same save/restore approach as vPortEnterCritical: track a global nesting
 * depth and restore the MIE state from before the outermost acquire. */
static volatile int  s_sl_depth = 0;
static volatile UINT s_sl_saved = 0;

IRAM_ATTR void __wrap_spinlock_initialize(portMUX_TYPE *mux)
{
    mux->owner = portMUX_FREE_VAL;
    mux->count = 0;
}

IRAM_ATTR void __wrap_spinlock_acquire(portMUX_TYPE *mux, int32_t timeout)
{
    (void)timeout;
    UINT s; DI(s);
    if (s_sl_depth == 0) s_sl_saved = s;
    s_sl_depth++;
    mux->count++;
}

IRAM_ATTR void __wrap_spinlock_release(portMUX_TYPE *mux)
{
    if (mux->count > 0) {
        mux->count--;
        if (--s_sl_depth <= 0) {
            s_sl_depth = 0;
            EI(s_sl_saved);
            shim_yield_if_needed(s_sl_saved);
        }
    }
}

/* Heap wrappers — delegate to IDF's heap_caps (not FreeRTOS heap).
 * esp_heap_caps.h is already included via shim_internal.h. */

void *__wrap_pvPortMalloc(size_t sz)       { return heap_caps_malloc(sz, MALLOC_CAP_DEFAULT); }
void  __wrap_vPortFree(void *p)            { heap_caps_free(p); }
void *__wrap_pvPortCalloc(size_t n, size_t sz) { return heap_caps_calloc(n, sz, MALLOC_CAP_DEFAULT); }
void *__wrap_pvPortRealloc(void *p, size_t sz) { return heap_caps_realloc(p, sz, MALLOC_CAP_DEFAULT); }

size_t __wrap_xPortGetFreeHeapSize(void) {
    return heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}
size_t __wrap_xPortGetMinimumEverFreeHeapSize(void) {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
}

/* -------------------------------------------------------------------------
 * Early µT-Kernel object-table initialization
 *
 * IDF's do_core_init() runs VFS init, libc locks init, and other subsystems
 * that call xQueueCreateMutex (via our shim) BEFORE knl_main() starts.
 * tk_cre_mtx() crashes if the kernel free-ID lists are uninitialized.
 *
 * Fix: wrap start_cpu0_default to call knl_init_object() (pure table setup,
 * no hardware) before IDF's system init functions run.  Wrap knl_init_object
 * itself to be idempotent so the second call from knl_main() is a no-op,
 * preserving all objects created during pre-kernel IDF init.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Early µT-Kernel object-table initialization
 *
 * IDF's do_core_init() runs init_libc (priority 102) and init_libc_stdio
 * (priority 120), which call xQueueCreateMutex(Static) via our shim.
 * tk_cre_mtx() crashes when the kernel free-ID lists are uninitialized.
 *
 * Fix: register an ESP_SYSTEM_INIT_FN with priority 1 (runs before everything)
 * that calls knl_init_object() once.  Guard knl_init_object() to be
 * idempotent so knl_main()'s second call is a safe no-op.
 * ------------------------------------------------------------------------- */
extern ER __real_knl_init_object(void);

static volatile bool s_obj_tables_ready = false;

ER __wrap_knl_init_object(void)
{
    if (s_obj_tables_ready) return E_OK;
    s_obj_tables_ready = true;
    return __real_knl_init_object();
}

ESP_SYSTEM_INIT_FN(shim_early_knl_init, CORE, BIT(0), 1)
{
    __wrap_knl_init_object();
    return ESP_OK;
}
