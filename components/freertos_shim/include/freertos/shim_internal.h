#pragma once
/*
 * shim_internal.h — types and helpers shared across shim_*.c files.
 *
 * This header is NOT a FreeRTOS public header.  It lives in include/freertos/
 * only so the shim sources can include it with a consistent path; nothing
 * outside the shim component should include it directly.
 *
 * Include order matters: riscv/encoding.h must come before tk/tkernel.h so
 * that IDF's MSTATUS_* definitions land first; µT-Kernel's sysdef.h already
 * has #ifndef guards and will skip the duplicates.
 */

#include "riscv/encoding.h"      /* defines MSTATUS_MIE/MPIE/MPP first */
#include "esp_heap_caps.h"       /* heap_caps_malloc/free/calloc/realloc */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <tk/tkernel.h>          /* µT-Kernel 3 API */
#include <tm/tmonitor.h>

/* -------------------------------------------------------------------------
 * Priority conversion
 *
 * FreeRTOS: higher number = higher priority (0 = idle, configMAX_PRIORITIES-1 = max)
 * µT-Kernel: lower number = higher priority (1 = highest, 32 = lowest by default)
 *
 * WiFi tasks arrive at FreeRTOS prio 23; they must preempt all app tasks.
 * We map them into µT-Kernel prio band 1–5.  App tasks should use prio 6+.
 *
 *   tk_prio = CFG_WIFI_PRI_BASE - fr_prio
 *   clamped to [1, CNF_MAX_TSKPRI]
 * ------------------------------------------------------------------------- */
#define CFG_WIFI_PRI_BASE  25   /* FreeRTOS prio 23 → tk prio 2 */

static inline PRI shim_prio_to_tk(uint32_t fr)
{
    int p = (int)CFG_WIFI_PRI_BASE - (int)fr;
    if (p < 1) p = 1;
    if (p > 32) p = 32;
    return (PRI)p;
}

/* -------------------------------------------------------------------------
 * Timeout conversion — known tick-rate mismatch
 *
 * µT-Kernel runs at CNF_TIMER_PERIOD = 1 ms/tick.
 * IDF is configured at CONFIG_FREERTOS_HZ = 100 (10 ms/tick).
 * shim_tmo() maps FreeRTOS tick counts 1:1 to MTK3 ms ticks, so all
 * shim-internal timed waits are 10× shorter than the FreeRTOS caller intends.
 *
 * This is intentional: the WiFi blob calls task_ms_to_tick_wrapper()
 * (= ms / portTICK_PERIOD_MS = ms/10) before every queue/semaphore wait.
 * With the 1:1 shim this gives ms/10 ms, making blob tasks poll 10× faster
 * than designed.  That accidental speedup is load-bearing — slowing them
 * back down (e.g. by setting CONFIG_FREERTOS_HZ=1000 or multiplying
 * shim_tmo by portTICK_PERIOD_MS) causes WPA2 4-way handshake to time out
 * (WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) because the pp_task/supplicant queue
 * polls drop from ~1 ms to ~10 ms intervals.
 *
 * Application-layer workaround: callers that specify timeouts in real ms
 * (esp_http_client timeout_ms, lwIP sys_arch_sem_wait, etc.) must multiply
 * their intended value by portTICK_PERIOD_MS (×10) so the effective wait
 * reaches the right duration after the shim's 1:1 conversion.
 * ------------------------------------------------------------------------- */
#define SHIM_MAX_DELAY  0xffffffffUL

static inline TMO shim_tmo(uint32_t ticks)
{
    if (ticks == SHIM_MAX_DELAY) return TMO_FEVR;
    if (ticks == 0)              return TMO_POL;
    return (TMO)ticks;
}

/* -------------------------------------------------------------------------
 * Handle tags — FreeRTOS routes both real queues and semaphores through the
 * same xQueue* functions (semphr.h macros expand to xQueueSemaphoreTake etc.).
 * We distinguish them with a tag embedded in a malloc'd wrapper.
 * ------------------------------------------------------------------------- */
typedef enum {
    SHIM_TAG_QUEUE   = 0x51554555, /* 'QUEU' */
    SHIM_TAG_SEM_BIN = 0x53454D42, /* 'SEMB' */
    SHIM_TAG_SEM_CNT = 0x53454D43, /* 'SEMC' */
    SHIM_TAG_MUTEX   = 0x4D555458, /* 'MUTX' */
    SHIM_TAG_RMUTEX  = 0x524D5458, /* 'RMTX' */
    SHIM_TAG_TIMER   = 0x54494D52, /* 'TIMR' */
    SHIM_TAG_EVTGRP  = 0x45564752, /* 'EVGR' */
} shim_tag_t;

/* Common header for all shim handle types.
 * is_static=1: handle overlaid on caller-supplied buffer (not heap-alloc'd);
 * vQueueDelete skips heap_caps_free for these. */
typedef struct {
    shim_tag_t tag;
    uint8_t    is_static;
} shim_hdr_t;

/* Queue handle — ring buffer; ISR-safe on both send and receive paths.
 *
 * nonempty/nonfull are NOTIFICATION-only: their counts may lag (ISR paths
 * bypass tk_wai_sem), so task paths always recheck head/tail after waking.
 */
typedef struct {
    shim_hdr_t        hdr;      /* tag = SHIM_TAG_QUEUE */
    uint32_t          item_sz;
    uint32_t          cap;      /* ring buffer capacity in items */
    volatile uint32_t head;     /* dequeue index */
    volatile uint32_t tail;     /* enqueue index */
    uint8_t          *data;     /* cap * item_sz bytes, heap-alloc'd */
    ID                nonempty; /* notification sem — task receivers block here */
    ID                nonfull;  /* notification sem — task senders block here */
} shim_queue_t;

/* Semaphore handle (binary or counting).
 *
 * cnt is the authoritative count, ISR-accessible under DI/MIE=0.
 * sem_id is notification-only: tasks block here; Give wakes via tk_sig_sem
 * (ISR-safe).  sem_id.isemcnt may lag cnt so task Takes recheck cnt on wake.
 */
typedef struct {
    shim_hdr_t   hdr;     /* tag = SHIM_TAG_SEM_BIN or SHIM_TAG_SEM_CNT */
    volatile INT  cnt;    /* current count (ISR-accessible) */
    INT           max_cnt;
    ID            sem_id; /* notification sem: max=max_cnt, init=0 */
} shim_sem_t;

/* Mutex handle (non-recursive) */
typedef struct {
    shim_hdr_t hdr;     /* tag = SHIM_TAG_MUTEX */
    ID         mtx_id;  /* tk_cre_mtx ID */
} shim_mutex_t;

/* Recursive mutex handle */
typedef struct {
    shim_hdr_t hdr;     /* tag = SHIM_TAG_RMUTEX */
    ID         mtx_id;
    ID         owner_task;  /* µT-Kernel task ID of current holder, or 0 */
    int        depth;       /* recursion depth */
} shim_rmutex_t;

/* Task handle — wraps a µT-Kernel task ID plus a notification semaphore */
#define SHIM_TLS_MAX  1  /* configNUM_THREAD_LOCAL_STORAGE_POINTERS = 1 */
typedef struct {
    shim_hdr_t       hdr;
    ID               tsk_id;
    ID               notify_sem; /* backs xTaskNotifyGive / ulTaskNotifyTake */
    volatile uint32_t notify_val; /* bits accumulated by eSetBits / eSetValueWithOverwrite */
    void            *stack;      /* heap_caps_malloc'd stack, freed in vTaskDelete */
    uint32_t         stack_sz;   /* stack size in bytes (for high-water scan) */
    char             name[12];   /* task name (for watermark dump) */
    void            *tls[SHIM_TLS_MAX]; /* thread-local storage (pvTaskGetTLS) */
} shim_task_t;

/* Scan all task stacks for sentinel 0xA5A5A5A5 and print peak usage. */
void shim_dump_stack_watermarks(void);

/* Event group handle */
typedef struct {
    shim_hdr_t hdr;
    ID         flg_id;  /* tk_cre_flg ID */
} shim_evtgrp_t;

/* Software timer handle */
typedef struct {
    shim_hdr_t  hdr;
    ID          alm_id;     /* tk_cre_alm (one-shot) or 0 */
    ID          cyc_id;     /* tk_cre_cyc (periodic)  or 0 */
    uint32_t    period_ms;
    bool        auto_reload;
    bool        active;
    void       *timer_id;   /* pvTimerGetTimerID user value */
    void      (*callback)(void *timer_handle);
    char        name[16];
} shim_timer_t;

/* -------------------------------------------------------------------------
 * Stub macro — logs the unimplemented call and aborts.
 * During bring-up, hitting a SHIM_STUB means a symbol needs promoting.
 * ------------------------------------------------------------------------- */
#define SHIM_STUB(fn)  \
    do { tm_printf((UB*)"[shim] STUB: " #fn " not implemented\n"); abort(); } while(0)

/* -------------------------------------------------------------------------
 * ISR-context check
 *
 * µT-Kernel tracks hardware ISR nesting separately from the broader
 * task-independent nesting depth.  Use knl_isr_nest for FreeRTOS-compatible
 * ISR queries, and knl_taskindp for blocking/dispatch-forbidden checks.
 * It is declared IMPORT W (= extern int32_t) in the kernel internals and
 * is directly accessible since the shim links against the same image.
 * ------------------------------------------------------------------------- */
extern int32_t knl_taskindp;
extern int32_t knl_isr_nest;
extern INT     knl_dispatch_disabled; /* µT-Kernel dispatch-lock counter */
extern void   *knl_ctxtsk;            /* currently running task TCB (NULL = no task) */

static inline bool shim_in_isr(void)
{
    return knl_isr_nest > 0;
}

/* FreeRTOS-shim blocking guard: true when blocking/dispatch is forbidden by
 * the shim-visible state.  The native ESP32-C3 sysdepend in_ddsp() also treats
 * INTLEVEL_DI CPU-threshold masking as CPU-lock state. */
static inline bool shim_in_ddsp(void)
{
    unsigned long ms;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(ms));
    return (ms & MSTATUS_MIE) == 0 || knl_taskindp != 0 ||
           knl_dispatch_disabled != 0 || knl_ctxtsk == NULL;
}
