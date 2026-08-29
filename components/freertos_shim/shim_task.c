#include <string.h>
#include "freertos/shim_internal.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

/* -------------------------------------------------------------------------
 * Task handle table — maps µT-Kernel task IDs to shim_task_t wrappers.
 * The table is protected by a critical section (DI/EI) since task creation
 * can happen from multiple tasks during WiFi init.
 * ------------------------------------------------------------------------- */
#define SHIM_TASK_MAX  48   /* must match CNF_MAX_TSKID */

static shim_task_t s_tasks[SHIM_TASK_MAX];

static shim_task_t *alloc_task_entry(void)
{
    UINT saved; DI(saved);
    shim_task_t *t = NULL;
    for (int i = 0; i < SHIM_TASK_MAX; i++) {
        if (s_tasks[i].tsk_id == 0) { t = &s_tasks[i]; break; }
    }
    EI(saved);
    return t;
}

static shim_task_t *find_by_tk_id(ID id)
{
    for (int i = 0; i < SHIM_TASK_MAX; i++)
        if (s_tasks[i].tsk_id == id) return &s_tasks[i];
    return NULL;
}

static shim_task_t *find_by_handle(TaskHandle_t h)
{
    return (shim_task_t *)h;
}

/* -------------------------------------------------------------------------
 * xTaskCreatePinnedToCore / xTaskCreate
 *
 * FreeRTOS stack_words is in 32-bit words; T_CTSK.stksz is in bytes.
 * Minimum stack: 2 KB (512 words * 4) — the WiFi tasks ask for 3072+ words.
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t stack_words, void *param, UBaseType_t prio,
    TaskHandle_t *out, BaseType_t core)
{
    (void)core; /* single-core */

    shim_task_t *entry = alloc_task_entry();
    if (!entry) {
        tm_printf((UB*)"[shim] xTaskCreate: no free task entry (%s)\n", name ? name : "?");
        return pdFALSE;
    }

    /* Notification semaphore (binary, initially empty) */
    T_CSEM cs = { .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
    ID notify_sem = tk_cre_sem(&cs);
    if (notify_sem <= 0) {
        tm_printf((UB*)"[shim] xTaskCreate: tk_cre_sem failed (%s)\n", name ? name : "?");
        return pdFALSE;
    }

    /* IMPORTANT unit fix: ESP-IDF's xTaskCreate[PinnedToCore] takes the stack
     * size in BYTES (see freertos task.h: "the stack size DEFINED IN BYTES.
     * Note that this differs from vanilla FreeRTOS").  IDF components call this
     * wrapper with byte counts (esp_timer 4096, wifi 6656, ...).  The previous
     * code multiplied by sizeof(StackType_t), over-allocating every task stack
     * 4x and wasting ~76 KB of internal RAM — which starved the BLE connection
     * path and broke nimble_host allocation under any added memory pressure.
     *
     * Allocate the requested bytes plus a fixed headroom.  µT-Kernel runs ISRs
     * on the current task's stack (unlike IDF FreeRTOS, which has a dedicated
     * interrupt stack), so add 2 KB of margin on top of IDF's sizing.  Measured
     * peak usage across all tasks (incl. ISR frames) is < 1.8 KB, so this is
     * very safe. */
    SZ stack_sz = (SZ)stack_words + 2048;
    void *stack = heap_caps_malloc(stack_sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    /* Paint the stack with a sentinel so uxTaskGetStackHighWaterMark() and
     * shim_dump_stack_watermarks() can measure peak usage by scanning for the
     * first overwritten word. */
    if (stack) {
        uint32_t *w = (uint32_t *)stack;
        for (SZ i = 0; i < stack_sz / 4; i++) w[i] = 0xA5A5A5A5u;
    }
    if (!stack) {
        tk_del_sem(notify_sem);
        esp_rom_printf("[shim] xTaskCreate: stack alloc FAILED (%s) need=%u free_int=%u largest=%u\n",
                       name ? name : "?", (unsigned)stack_sz,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return pdFALSE;
    }

    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG0 | TA_USERBUF,
        .task    = (FP)fn,
        .itskpri = shim_prio_to_tk((uint32_t)prio),
        .stksz   = stack_sz,
        .bufptr  = stack,
    };

    ID tsk_id = tk_cre_tsk(&ctsk);
    if (tsk_id <= 0) {
        heap_caps_free(stack);
        tk_del_sem(notify_sem);
        tm_printf((UB*)"[shim] xTaskCreate: tk_cre_tsk failed (%s) err=%d\n",
                  name ? name : "?", (int)tsk_id);
        return pdFALSE;
    }

    entry->hdr.tag    = (shim_tag_t)0; /* not a queue type */
    entry->tsk_id     = tsk_id;
    entry->notify_sem = notify_sem;
    entry->stack      = stack;
    entry->stack_sz   = (uint32_t)stack_sz;
    if (name) { strncpy(entry->name, name, sizeof(entry->name) - 1); entry->name[sizeof(entry->name) - 1] = '\0'; }
    else      { entry->name[0] = '\0'; }

    ER er = tk_sta_tsk(tsk_id, (INT)(intptr_t)param);
    if (er != E_OK) {
        tk_del_tsk(tsk_id);
        tk_del_sem(notify_sem);
        entry->tsk_id = 0;
        return pdFALSE;
    }

    if (out) *out = (TaskHandle_t)entry;
    return pdTRUE;
}

BaseType_t __wrap_xTaskCreate(TaskFunction_t fn, const char *name,
    uint32_t stack_words, void *param, UBaseType_t prio, TaskHandle_t *out)
{
    return __wrap_xTaskCreatePinnedToCore(fn, name, stack_words, param, prio, out, 0);
}

void __wrap_vTaskDelete(TaskHandle_t h)
{
    if (!h) {
        /* Delete self — not supported cleanly; just call tk_ext_tsk() */
        tk_ext_tsk();
        return;
    }
    shim_task_t *entry = find_by_handle(h);
    if (!entry || entry->tsk_id == 0) return;

    tk_ter_tsk(entry->tsk_id);
    tk_del_tsk(entry->tsk_id);
    tk_del_sem(entry->notify_sem);
    if (entry->stack) { heap_caps_free(entry->stack); entry->stack = NULL; }
    entry->tsk_id     = 0;
    entry->notify_sem = 0;
}

/* -------------------------------------------------------------------------
 * Delay / tick count
 * ------------------------------------------------------------------------- */
void __wrap_vTaskDelay(TickType_t ticks)
{
    if (ticks == 0) return;
    tk_dly_tsk((RELTIM)ticks);
}

void __wrap_vTaskDelayUntil(TickType_t *prev, TickType_t incr)
{
    SYSTIM t; tk_get_tim(&t);
    TickType_t now = (TickType_t)t.lo;
    TickType_t next = *prev + incr;
    if ((int32_t)(next - now) > 0)
        tk_dly_tsk((RELTIM)(next - now));
    *prev = next;
}

TickType_t __wrap_xTaskGetTickCount(void)
{
    SYSTIM t;
    tk_get_tim(&t);
    return (TickType_t)t.lo; /* ms since boot; wraps after ~49 days */
}

TickType_t __wrap_xTaskGetTickCountFromISR(void)
{
    return __wrap_xTaskGetTickCount();
}

/* -------------------------------------------------------------------------
 * Current task handle — walk the table for the running task ID
 * ------------------------------------------------------------------------- */
TaskHandle_t __wrap_xTaskGetCurrentTaskHandle(void)
{
    ID cur = tk_get_tid();
    shim_task_t *e = find_by_tk_id(cur);
    return (TaskHandle_t)e; /* may be NULL if task not created via shim */
}

TaskHandle_t __wrap_xTaskGetHandle(const char *name)
{
    (void)name;
    return NULL; /* name-based lookup not supported */
}

UBaseType_t __wrap_uxTaskPriorityGet(TaskHandle_t h)
{
    (void)h;
    return 5; /* stub: return mid-range priority */
}

void __wrap_vTaskPrioritySet(TaskHandle_t h, UBaseType_t prio)
{
    if (!h) return;
    shim_task_t *entry = find_by_handle(h);
    if (!entry || entry->tsk_id == 0) return;
    T_CTSK info;
    if (tk_ref_tsk(entry->tsk_id, (T_RTSK*)&info) != E_OK) return;
    tk_chg_pri(entry->tsk_id, shim_prio_to_tk((uint32_t)prio));
}

/* Scan a painted stack for the first non-sentinel word from the low end;
 * returns bytes still untouched (the FreeRTOS high-water-mark semantic). */
static uint32_t stack_free_bytes(const shim_task_t *e)
{
    if (!e->stack || e->stack_sz == 0) return 0;
    const uint32_t *w = (const uint32_t *)e->stack;
    uint32_t n = e->stack_sz / 4, i = 0;
    while (i < n && w[i] == 0xA5A5A5A5u) i++;
    return i * 4;  /* untouched bytes at the low (overflow) end */
}

UBaseType_t __wrap_uxTaskGetStackHighWaterMark(TaskHandle_t h)
{
    shim_task_t *e = h ? find_by_handle(h) : find_by_tk_id(tk_get_tid());
    if (!e) return 0;
    return stack_free_bytes(e);
}

void shim_dump_stack_watermarks(void)
{
    for (int i = 0; i < SHIM_TASK_MAX; i++) {
        shim_task_t *e = &s_tasks[i];
        if (e->tsk_id == 0 || e->stack == NULL) continue;
        uint32_t freeb = stack_free_bytes(e);
        uint32_t used  = e->stack_sz - freeb;
        esp_rom_printf("[stackwm] %-12s used=%u/%u free=%u%s\n",
                       e->name[0] ? e->name : "?", (unsigned)used,
                       (unsigned)e->stack_sz, (unsigned)freeb,
                       freeb < 256 ? "  <<< LOW" : "");
    }
}

/* -------------------------------------------------------------------------
 * Task notifications — backed by per-task semaphore + notify_val bitmask.
 *
 * eIncrement / eNoAction: just signal the semaphore (no bit accumulation).
 * eSetBits: OR bits into notify_val, then signal.
 * eSetValueWithOverwrite: replace notify_val, then signal.
 * Receivers use ulTaskNotifyTake (ignores value, just wakes) or
 * xTaskNotifyWait (reads and clears bits from notify_val).
 *
 * DI/EI guards the read-modify-write on notify_val; tk_sig_sem / tk_wai_sem
 * are already kernel-safe.
 * ------------------------------------------------------------------------- */
static BaseType_t notify_signal(shim_task_t *e, uint32_t val, eNotifyAction act)
{
    if (!e || e->notify_sem == 0) return pdFALSE;
    if (act == eSetBits) {
        UINT saved; DI(saved);
        e->notify_val |= val;
        EI(saved);
    } else if (act == eSetValueWithOverwrite) {
        UINT saved; DI(saved);
        e->notify_val = val;
        EI(saved);
    }
    /* eIncrement, eNoAction: semaphore count is the value; notify_val stays 0 */
    return (tk_sig_sem(e->notify_sem, 1) == E_OK) ? pdTRUE : pdFALSE;
}

BaseType_t __wrap_xTaskNotifyGive(TaskHandle_t h)
{
    shim_task_t *e = find_by_handle(h);
    if (!e || e->notify_sem == 0) return pdFALSE;
    return (tk_sig_sem(e->notify_sem, 1) == E_OK) ? pdTRUE : pdFALSE;
}

void __wrap_xTaskNotifyGiveFromISR(TaskHandle_t h, BaseType_t *pw)
{
    shim_task_t *e = find_by_handle(h);
    if (!e || e->notify_sem == 0) return;
    tk_sig_sem(e->notify_sem, 1);
    if (pw) *pw = pdFALSE;
}

uint32_t __wrap_ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks)
{
    ID cur = tk_get_tid();
    shim_task_t *e = find_by_tk_id(cur);
    if (!e || e->notify_sem == 0) return 0;
    ER er = tk_wai_sem(e->notify_sem, 1, shim_tmo(ticks));
    if (er != E_OK) return 0;
    UINT saved; DI(saved);
    uint32_t v = e->notify_val;
    if (clear_on_exit) e->notify_val = 0;
    EI(saved);
    return v ? v : 1; /* return accumulated bits, or 1 for plain eIncrement */
}

BaseType_t __wrap_xTaskNotify(TaskHandle_t h, uint32_t val, eNotifyAction act)
{
    return notify_signal(find_by_handle(h), val, act);
}

BaseType_t __wrap_xTaskNotifyFromISR(TaskHandle_t h, uint32_t val,
    eNotifyAction act, BaseType_t *pw)
{
    shim_task_t *e = find_by_handle(h);
    if (!e || e->notify_sem == 0) return pdFALSE;
    if (act == eSetBits) {
        UINT saved; DI(saved);
        e->notify_val |= val;
        EI(saved);
    } else if (act == eSetValueWithOverwrite) {
        UINT saved; DI(saved);
        e->notify_val = val;
        EI(saved);
    }
    tk_sig_sem(e->notify_sem, 1);
    if (pw) *pw = pdFALSE;
    return pdTRUE;
}

BaseType_t __wrap_xTaskNotifyWait(uint32_t clr_entry, uint32_t clr_exit,
    uint32_t *pval, TickType_t ticks)
{
    ID cur = tk_get_tid();
    shim_task_t *e = find_by_tk_id(cur);
    if (!e || e->notify_sem == 0) return pdFALSE;
    if (clr_entry) {
        UINT saved; DI(saved);
        e->notify_val &= ~clr_entry;
        EI(saved);
    }
    ER er = tk_wai_sem(e->notify_sem, 1, shim_tmo(ticks));
    if (er != E_OK) return pdFALSE;
    UINT saved; DI(saved);
    uint32_t v = e->notify_val;
    e->notify_val &= ~clr_exit;
    EI(saved);
    if (pval) *pval = v;
    return pdTRUE;
}

/* -------------------------------------------------------------------------
 * "Generic" notification variants — IDF's FreeRTOS exposes these as the
 * actual linker symbols; the non-Generic names are inline macros.
 * We route them all through the same notify-semaphore mechanism.
 * uxIndexToWaitOn / uxIndexToNotify is always 0 (tskDEFAULT_INDEX_TO_NOTIFY)
 * in practice so the index is ignored.
 * ------------------------------------------------------------------------- */
uint32_t __wrap_ulTaskGenericNotifyTake(UBaseType_t idx, BaseType_t clr, TickType_t ticks)
{
    (void)idx;
    return __wrap_ulTaskNotifyTake(clr, ticks);
}

BaseType_t __wrap_xTaskGenericNotify(TaskHandle_t h, UBaseType_t idx,
    uint32_t val, eNotifyAction act, uint32_t *prev)
{
    (void)idx; (void)prev;
    return notify_signal(find_by_handle(h), val, act);
}

BaseType_t __wrap_xTaskGenericNotifyFromISR(TaskHandle_t h, UBaseType_t idx,
    uint32_t val, eNotifyAction act, uint32_t *prev, BaseType_t *pw)
{
    (void)idx; (void)prev;
    return __wrap_xTaskNotifyFromISR(h, val, act, pw);
}

void __wrap_vTaskGenericNotifyGiveFromISR(TaskHandle_t h, UBaseType_t idx, BaseType_t *pw)
{
    (void)idx;
    __wrap_xTaskNotifyGiveFromISR(h, pw);
}

BaseType_t __wrap_xTaskGenericNotifyWait(UBaseType_t idx,
    uint32_t clr_entry, uint32_t clr_exit, uint32_t *pval, TickType_t ticks)
{
    (void)idx; (void)clr_entry;
    return __wrap_xTaskNotifyWait(clr_entry, clr_exit, pval, ticks);
}

BaseType_t __wrap_xTaskGenericNotifyStateClear(TaskHandle_t h, UBaseType_t idx)
{
    (void)h; (void)idx;
    return pdTRUE;
}

uint32_t __wrap_ulTaskGenericNotifyValueClear(TaskHandle_t h, UBaseType_t idx, uint32_t bits)
{
    (void)h; (void)idx; (void)bits;
    return 0;
}

/* -------------------------------------------------------------------------
 * Thread-local storage (TLS) — backed by the per-task tls[] array.
 * The IDF's pthread layer uses a single TLS slot per task to store a
 * per-thread semaphore handle (wifi_thread_semphr_get_wrapper).
 * Tasks not created via xTaskCreate (e.g. user tasks using tk_cre_tsk
 * directly) fall through to a per-core global TLS fallback.
 * ------------------------------------------------------------------------- */
static void *s_tls_fallback[SHIM_TLS_MAX];

void *__wrap_pvTaskGetThreadLocalStoragePointer(TaskHandle_t task, BaseType_t idx)
{
    if ((unsigned)idx >= SHIM_TLS_MAX) return NULL;
    shim_task_t *e = task ? find_by_handle(task) : find_by_tk_id(tk_get_tid());
    return e ? e->tls[idx] : s_tls_fallback[idx];
}

void __wrap_vTaskSetThreadLocalStoragePointer(TaskHandle_t task, BaseType_t idx, void *val)
{
    if ((unsigned)idx >= SHIM_TLS_MAX) return;
    shim_task_t *e = task ? find_by_handle(task) : find_by_tk_id(tk_get_tid());
    if (e) e->tls[idx] = val;
    else s_tls_fallback[idx] = val;
}

/* TlsDeleteCallbackFunction_t: void (*)(int, void*) — we ignore the callback
 * since µT-Kernel tasks are not tracked with lifecycle callbacks. */
typedef void (*TlsDeleteCallbackFunction_t)(int, void *);
void __wrap_vTaskSetThreadLocalStoragePointerAndDelCallback(
    TaskHandle_t task, BaseType_t idx, void *val,
    TlsDeleteCallbackFunction_t cb)
{
    (void)cb;
    __wrap_vTaskSetThreadLocalStoragePointer(task, idx, val);
}

/* __wrap_xPortGetCoreID is defined in shim_port.c */
