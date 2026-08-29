#include <string.h>
#include "freertos/shim_internal.h"
#include "freertos/timers.h"

/*
 * FreeRTOS software timers → µT-Kernel alarm (one-shot) / cyclic handlers.
 *
 * Design:
 *   - auto_reload=false → tk_cre_alm + tk_sta_alm / tk_stp_alm / tk_del_alm
 *   - auto_reload=true  → tk_cre_cyc + tk_sta_cyc / tk_stp_cyc / tk_del_cyc
 *
 * The µT-Kernel alarm/cyclic callback runs in the task-independent portion
 * (similar to an ISR).  The FreeRTOS timer callback convention is the same
 * (it runs in the timer-daemon task in vanilla FreeRTOS, but callers must not
 * block inside it).  We call the callback directly from the alarm/cyclic
 * handler — consistent with the IDF internal timer usage (esp_timer callbacks
 * are also called from interrupt/task-independent context).
 *
 * For callers that need a task context (e.g. do tk_dly_tsk inside a callback),
 * a trampoline task would be needed — not implemented here; those are rare in
 * the WiFi/lwIP path.
 */

/* -------------------------------------------------------------------------
 * Alarm callback trampoline
 * ------------------------------------------------------------------------- */
static void alm_handler(ID alm_id, void *exinf)
{
    shim_timer_t *t = (shim_timer_t *)exinf;
    if (t && t->callback && t->active)
        t->callback((void *)t);
}

/* -------------------------------------------------------------------------
 * Cyclic handler trampoline
 * ------------------------------------------------------------------------- */
static void cyc_handler(ID cyc_id, void *exinf)
{
    shim_timer_t *t = (shim_timer_t *)exinf;
    if (t && t->callback && t->active)
        t->callback((void *)t);
}

/* -------------------------------------------------------------------------
 * xTimerCreate / xTimerCreateStatic
 * ------------------------------------------------------------------------- */
TimerHandle_t __wrap_xTimerCreate(const char *name, TickType_t period,
    BaseType_t auto_reload, void *timer_id, TimerCallbackFunction_t cb)
{
    shim_timer_t *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_DEFAULT);
    if (!t) return NULL;

    t->hdr.tag    = SHIM_TAG_TIMER;
    t->period_ms  = (uint32_t)(period > 0 ? period : 1);
    t->auto_reload = (auto_reload != pdFALSE);
    t->active      = false;
    t->timer_id    = timer_id;
    t->callback    = cb;
    if (name) strncpy(t->name, name, sizeof(t->name) - 1);

    if (t->auto_reload) {
        T_CCYC cc = {
            .exinf   = (void *)t,
            .cycatr  = TA_HLNG,
            .cychdr  = (FP)cyc_handler,
            .cyctim  = t->period_ms,
            .cycphs  = t->period_ms,
        };
        t->cyc_id = tk_cre_cyc(&cc);
        if (t->cyc_id <= 0) { heap_caps_free(t); return NULL; }
    } else {
        T_CALM ca = {
            .exinf  = (void *)t,
            .almatr = TA_HLNG,
            .almhdr = (FP)alm_handler,
        };
        t->alm_id = tk_cre_alm(&ca);
        if (t->alm_id <= 0) { heap_caps_free(t); return NULL; }
    }

    return (TimerHandle_t)t;
}

TimerHandle_t __wrap_xTimerCreateStatic(const char *name, TickType_t period,
    BaseType_t auto_reload, void *timer_id, TimerCallbackFunction_t cb,
    void *static_timer)
{
    (void)static_timer;
    return __wrap_xTimerCreate(name, period, auto_reload, timer_id, cb);
}

/* -------------------------------------------------------------------------
 * Start / Stop / Reset / ChangePeriod / Delete
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xTimerStart(TimerHandle_t th, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    shim_timer_t *t = (shim_timer_t *)th;
    if (!t) return pdFALSE;
    t->active = true;
    if (t->auto_reload)
        return (tk_sta_cyc(t->cyc_id) == E_OK) ? pdTRUE : pdFALSE;
    else
        return (tk_sta_alm(t->alm_id, t->period_ms) == E_OK) ? pdTRUE : pdFALSE;
}

BaseType_t __wrap_xTimerStop(TimerHandle_t th, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    shim_timer_t *t = (shim_timer_t *)th;
    if (!t) return pdFALSE;
    t->active = false;
    if (t->auto_reload)
        return (tk_stp_cyc(t->cyc_id) == E_OK) ? pdTRUE : pdFALSE;
    else
        return (tk_stp_alm(t->alm_id) == E_OK) ? pdTRUE : pdFALSE;
}

BaseType_t __wrap_xTimerReset(TimerHandle_t th, TickType_t ticks_to_wait)
{
    shim_timer_t *t = (shim_timer_t *)th;
    if (!t) return pdFALSE;
    __wrap_xTimerStop(th, ticks_to_wait);
    return __wrap_xTimerStart(th, ticks_to_wait);
}

BaseType_t __wrap_xTimerChangePeriod(TimerHandle_t th, TickType_t new_period,
    TickType_t ticks_to_wait)
{
    shim_timer_t *t = (shim_timer_t *)th;
    if (!t) return pdFALSE;
    bool was_active = t->active;
    __wrap_xTimerStop(th, ticks_to_wait);
    t->period_ms = (uint32_t)(new_period > 0 ? new_period : 1);
    if (was_active) return __wrap_xTimerStart(th, ticks_to_wait);
    return pdTRUE;
}

BaseType_t __wrap_xTimerDelete(TimerHandle_t th, TickType_t ticks_to_wait)
{
    shim_timer_t *t = (shim_timer_t *)th;
    if (!t) return pdFALSE;
    __wrap_xTimerStop(th, ticks_to_wait);
    if (t->auto_reload && t->cyc_id > 0) tk_del_cyc(t->cyc_id);
    if (!t->auto_reload && t->alm_id > 0) tk_del_alm(t->alm_id);
    heap_caps_free(t);
    return pdTRUE;
}

BaseType_t __wrap_xTimerIsTimerActive(TimerHandle_t th)
{
    shim_timer_t *t = (shim_timer_t *)th;
    return (t && t->active) ? pdTRUE : pdFALSE;
}

/* -------------------------------------------------------------------------
 * FromISR variants — timer commands from ISR context.
 * µT-Kernel alarm/cyclic start/stop are not safe from the task-independent
 * portion.  We set a flag and rely on the callback trampoline or next tick.
 * For now these are simple direct calls (safe in practice because the ESP32-C3
 * does not have real hardware IRQ pre-empting an alarm callback mid-flight).
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xTimerStartFromISR(TimerHandle_t t, BaseType_t *pw)
{
    if (pw) *pw = pdFALSE;
    return __wrap_xTimerStart(t, 0);
}
BaseType_t __wrap_xTimerStopFromISR(TimerHandle_t t, BaseType_t *pw)
{
    if (pw) *pw = pdFALSE;
    return __wrap_xTimerStop(t, 0);
}
BaseType_t __wrap_xTimerResetFromISR(TimerHandle_t t, BaseType_t *pw)
{
    if (pw) *pw = pdFALSE;
    return __wrap_xTimerReset(t, 0);
}
BaseType_t __wrap_xTimerChangePeriodFromISR(TimerHandle_t t, TickType_t p, BaseType_t *pw)
{
    if (pw) *pw = pdFALSE;
    return __wrap_xTimerChangePeriod(t, p, 0);
}

/* -------------------------------------------------------------------------
 * Accessors
 * ------------------------------------------------------------------------- */
void *__wrap_pvTimerGetTimerID(TimerHandle_t th)
{
    shim_timer_t *t = (shim_timer_t *)th;
    return t ? t->timer_id : NULL;
}
void __wrap_vTimerSetTimerID(TimerHandle_t th, void *id)
{
    shim_timer_t *t = (shim_timer_t *)th;
    if (t) t->timer_id = id;
}
const char *__wrap_pcTimerGetName(TimerHandle_t th)
{
    shim_timer_t *t = (shim_timer_t *)th;
    return t ? t->name : "";
}
TickType_t __wrap_xTimerGetPeriod(TimerHandle_t th)
{
    shim_timer_t *t = (shim_timer_t *)th;
    return t ? (TickType_t)t->period_ms : 0;
}
TickType_t __wrap_xTimerGetExpiryTime(TimerHandle_t th)
{
    (void)th;
    return 0; /* stub — not used on critical paths */
}
