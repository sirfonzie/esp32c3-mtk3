#include "freertos/shim_internal.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"   /* IRAM_ATTR */

extern int32_t knl_isr_nest;

/* Forward declarations for shim functions in sibling translation units. */
extern QueueHandle_t __wrap_xQueueGenericCreate(UBaseType_t len,
                                                 UBaseType_t item_sz,
                                                 uint8_t type);
extern TaskHandle_t  __wrap_xTaskGetCurrentTaskHandle(void);
extern void          __wrap_vTaskDelayUntil(TickType_t *prev, TickType_t incr);

/* =========================================================================
 * Stubs for FreeRTOS symbols removed by the local components/freertos/
 * shadow component (tasks.c, queue.c, timers.c, event_groups.c, portasm.S).
 * µT-Kernel 3 owns all scheduling; none of these are called at runtime
 * unless noted otherwise.
 * ========================================================================= */

/* ---- portasm.S stubs --------------------------------------------------- */

/* rtos_int_enter — IDF's vectors.S calls this on EVERY interrupt's entry
 * (vectors.S:427), before dispatching to the registered handler.  We use it to
 * mark hardware ISR nesting and task-independent context for the whole port,
 * and to switch to the dedicated interrupt stack on the first nesting level.
 *
 * The counters are essential for correctness: peripheral interrupts registered
 * via esp_intr_alloc() (BLE controller, WiFi, coex) do NOT go through
 * tk_def_int's trampoline, so without this they would run with
 * knl_taskindp == 0.  A FreeRTOS *FromISR call from such an ISR (e.g. a
 * controller ISR giving the semaphore that wakes the host task) would then be
 * treated by µT-Kernel as a task-context call and dispatch synchronously from
 * inside the ISR, corrupting the interrupted task's saved context (observed as
 * an intermittent resume to PC=0 during BLE connections).  Incrementing
 * knl_taskindp here makes knl_dispatch() defer to __wrap_rtos_int_exit's
 * tail-edge switch instead.
 *
 * The stack switch mirrors IDF's own FreeRTOS port: vectors.S has already
 * saved the trap frame on the interrupted stack and expects this routine to
 * move sp ("If this is a non-nested interrupt, SP now points to the interrupt
 * stack").  The old sp is kept in knl_int_saved_sp; __wrap_rtos_int_exit
 * (kernel sysdepend cpu_cntl.c, which also owns the stack itself) restores it
 * -- or the next task's frame pointer -- on the final unnest.  Nested
 * interrupts stay on the interrupt stack.
 *
 * Naked asm: switching sp inside a C body is unsound (the compiler may
 * address locals through it).  Only t-regs and a0 are clobbered, matching the
 * standard call ABI vectors.S uses.  Runs with MIE=0 (trap entry), so the
 * increments and the sp swap are race-free.
 *
 * IRAM: vectors.S lives in IRAM and may run with the flash cache disabled. */
__attribute__((naked, section(".iram1.text")))
void *rtos_int_enter(void)
{
    __asm__ volatile (
        "la    t0, knl_taskindp        \n"
        "lw    t1, 0(t0)               \n"
        "addi  t1, t1, 1               \n"
        "sw    t1, 0(t0)               \n"
        "la    t0, knl_isr_nest        \n"
        "lw    t1, 0(t0)               \n"
        "addi  t1, t1, 1               \n"
        "sw    t1, 0(t0)               \n"
        "li    t2, 1                   \n"
        "bne   t1, t2, 1f              \n"   /* nested: already on the int stack */
        "la    t0, knl_int_saved_sp    \n"
        "sw    sp, 0(t0)               \n"   /* remember the interrupted frame */
        "la    t0, knl_int_stack_top   \n"
        "lw    sp, 0(t0)               \n"   /* switch to the interrupt stack */
    "1:                                \n"
        "li    a0, 0                   \n"   /* abstract ctx for rtos_int_exit */
        "ret                           \n"
    );
}

/* a0 = mstatus before interrupt, a1 = context from rtos_int_enter.
 * Wrapped by MTK3 kernel (__wrap_rtos_int_exit in cpu_cntl.c), so this
 * __real_ body is never reached either. */
IRAM_ATTR void rtos_int_exit(uint32_t mstatus, void *ctx)
{
    (void)mstatus;
    (void)ctx;
}

/* ---- tasks.c stubs ------------------------------------------------------ */

/* Called from app_startup.c:esp_startup_start_app().  That function is
 * wrapped (→ __wrap_esp_startup_start_app → knl_main()), so this is
 * unreachable. */
void vTaskStartScheduler(void) {}

/* vTaskSuspendAll / xTaskResumeAll — protect SPI flash operations and
 * esp_restart.  Map to MTK3 dispatch-disable so no other task preempts
 * the calling task during flash I/O. */
void vTaskSuspendAll(void)
{
    tk_dis_dsp();
}

BaseType_t xTaskResumeAll(void)
{
    tk_ena_dsp();
    return pdFALSE; /* no pending context switch in MTK3 */
}

/* vTaskSuspend — idle path in esp_event_loop_run_task and idf_additions.
 * No-op: event tasks block in __wrap_xQueueReceive (ring-buffer + nonempty sem)
 * rather than via FreeRTOS suspend/resume. */
void vTaskSuspend(TaskHandle_t task)
{
    (void)task;
}

/* xTaskGetCurrentTaskHandleForCore — panic handler debug output.
 * Single-core: ignore coreID and return current task handle. */
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t core)
{
    (void)core;
    return __wrap_xTaskGetCurrentTaskHandle();
}

/* pcTaskGetName — panic handler prints the task name.
 * We don't track names in shim_task_t; return a placeholder. */
char *pcTaskGetName(TaskHandle_t task)
{
    (void)task;
    return "(mtk3)";
}

/* xTaskDelayUntil — new name for vTaskDelayUntil; used by lwip ping.
 * Route to the existing __wrap_vTaskDelayUntil implementation. */
BaseType_t xTaskDelayUntil(TickType_t *prev, TickType_t incr)
{
    __wrap_vTaskDelayUntil(prev, incr);
    return pdTRUE;
}

/* ---- queue.c stubs ------------------------------------------------------ */

/* xQueueCreateCountingSemaphore — used by esp_wifi and esp_coex adapters.
 * Route to our existing __wrap_xQueueGenericCreate.
 * type 2 = queueQUEUE_TYPE_COUNTING_SEMAPHORE; item_sz 0 = semaphore. */
QueueHandle_t xQueueCreateCountingSemaphore(UBaseType_t maxCount,
                                             UBaseType_t initCount)
{
    (void)initCount; /* shim sets initial count to 0; callers Give() to initCount */
    return __wrap_xQueueGenericCreate(maxCount, 0, 2);
}

/* xQueueGetMutexHolder — used by pthread, mbedtls DS, and esp_libc locks.
 * The shim does not track owners; returning NULL is safe (means "no holder"). */
IRAM_ATTR TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q)
{
    (void)q;
    return NULL;
}
