#include <string.h>
#include "freertos/shim_internal.h"
#include "freertos/queue.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"

/* Forward declarations */
QueueHandle_t __wrap_xQueueGenericCreate(uint32_t len, uint32_t item_sz, uint8_t type);
QueueHandle_t __wrap_xQueueGenericCreateStatic(uint32_t len, uint32_t item_sz,
    uint8_t *storage, void *sq, uint8_t type);
BaseType_t __wrap_xQueueSemaphoreTake(QueueHandle_t q, TickType_t ticks);
QueueHandle_t __wrap_xQueueCreateMutexStatic(uint8_t type, void *sq);
BaseType_t __wrap_xQueueGiveMutexRecursive(QueueHandle_t q);

/* -------------------------------------------------------------------------
 * Handle allocation
 * ------------------------------------------------------------------------- */
static void *alloc_handle(size_t sz, shim_tag_t tag)
{
    shim_hdr_t *h = heap_caps_malloc(sz, MALLOC_CAP_DEFAULT);
    if (h) { h->tag = tag; h->is_static = 0; }
    return h;
}

static void *init_static_handle(void *buf, shim_tag_t tag)
{
    shim_hdr_t *h = (shim_hdr_t *)buf;
    h->tag       = tag;
    h->is_static = 1;
    return h;
}

/* -------------------------------------------------------------------------
 * Ring-buffer data backing store
 *
 * Ring buffer data (cap * item_sz bytes) is allocated from the IDF internal
 * heap (heap_caps) — the same store FreeRTOS queues use via pvPortMalloc.
 *
 * History / rationale for the change:
 *   This previously used a fixed 4 KB µT-Kernel memory pool carved from the
 *   kernel's 32 KB heap, to keep IDF internal RAM contiguous so the
 *   16640-byte NimBLE host task stack could still find one free block after
 *   WiFi/BLE queue init.  That rationale was eliminated when the shim's
 *   xTaskCreate stack sizing was corrected (ESP-IDF stack sizes are BYTES,
 *   not words): the largest single allocation dropped ~4x to ~8 KB and ~76 KB
 *   of internal RAM was freed.  The fixed 4 KB pool was also a hard ceiling —
 *   a single ESP-NOW RX queue (16 x 258 B = 4128 B) could not fit it at all.
 *   Sourcing ring-buffer data from the general internal heap removes the
 *   ceiling and scales with the app.
 *
 *   Trade-off: ring buffers now interleave with stacks/buffers in the IDF
 *   heap, so a later large allocation could in principle fail due to
 *   fragmentation.  In practice the risk is low (largest single alloc ~8 KB
 *   vs ~70 KB free; queues are created once at init, not churned) and the
 *   failure mode is a graceful NULL return + visible "alloc failed" log, not
 *   a crash.  Removing the pool also returns 4 KB to the kernel heap.
 *
 * Allocation only happens at queue create (task context), never from an ISR;
 * the ISR-safe send/receive hot path does not allocate.
 * ------------------------------------------------------------------------- */
static void *rbuf_alloc(size_t sz)
{
    return heap_caps_malloc(sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
}

static void rbuf_free(void *blk)
{
    heap_caps_free(blk);
}

/* -------------------------------------------------------------------------
 * Ring-buffer queue helpers
 *
 * The queue uses two µT-Kernel semaphores as NOTIFICATION mechanisms only:
 *   nonempty: task receivers block here; signalled by every Send.
 *   nonfull:  task senders block here; signalled by every Receive.
 *
 * head/tail are authoritative.  Semaphore counts may lag (ISR paths don't
 * call tk_wai_sem), so every task path rechecks head/tail after waking.
 *
 * tk_sig_sem has no CHECK_DISPATCH guard → ISR-safe.
 * tk_wai_sem has CHECK_DISPATCH      → task context only.
 * DI/EI on RISC-V ESP are save/restore of mstatus.MIE; from ISR both
 * are no-ops (MIE already 0), so a single code path serves both contexts.
 * ------------------------------------------------------------------------- */

static int queue_init(shim_queue_t *p, uint32_t len, uint32_t item_sz)
{
    p->item_sz = item_sz;
    p->cap     = len;
    p->head    = 0;
    p->tail    = 0;
    p->data    = rbuf_alloc((size_t)(len * item_sz));
    if (!p->data) return -1;

    T_CSEM cn = { .sematr = TA_TFIFO, .isemcnt = 0,       .maxsem = (INT)len };
    T_CSEM cf = { .sematr = TA_TFIFO, .isemcnt = (INT)len, .maxsem = (INT)len };
    p->nonempty = tk_cre_sem(&cn);
    p->nonfull  = tk_cre_sem(&cf);
    if (p->nonempty <= 0 || p->nonfull <= 0) {
        rbuf_free(p->data); p->data = NULL;
        if (p->nonempty > 0) tk_del_sem(p->nonempty);
        if (p->nonfull  > 0) tk_del_sem(p->nonfull);
        return -1;
    }
    return 0;
}

/* Non-blocking ring-buffer push (DI/EI makes it safe from task or ISR). */
static BaseType_t queue_push(shim_queue_t *p, const void *item, bool overwrite)
{
    UINT saved;
    DI(saved);
    uint32_t next = (p->tail + 1) % p->cap;
    bool full = (next == p->head);
    if (full) {
        if (!overwrite) { EI(saved); return pdFALSE; }
        p->head = (p->head + 1) % p->cap; /* discard oldest */
        full = false;
    }
    memcpy(p->data + p->tail * p->item_sz, item, p->item_sz);
    p->tail = (p->tail + 1) % p->cap;
    EI(saved);
    if (!full) tk_sig_sem(p->nonempty, 1); /* ignored E_QOVR is harmless */
    return pdTRUE;
}

/* Non-blocking ring-buffer pop (DI/EI; ISR-safe). */
static BaseType_t queue_pop(shim_queue_t *p, void *buf)
{
    UINT saved;
    DI(saved);
    if (p->head == p->tail) { EI(saved); return pdFALSE; }
    if (buf) memcpy(buf, p->data + p->head * p->item_sz, p->item_sz);
    p->head = (p->head + 1) % p->cap;
    EI(saved);
    tk_sig_sem(p->nonfull, 1); /* wake a blocked sender if any */
    return pdTRUE;
}

/* -------------------------------------------------------------------------
 * xQueueCreateCountingSemaphore / xQueueCreateCountingSemaphoreStatic
 * ------------------------------------------------------------------------- */
QueueHandle_t __wrap_xQueueCreateCountingSemaphore(uint32_t max, uint32_t init)
{
    return __wrap_xQueueGenericCreate(max, init, queueQUEUE_TYPE_COUNTING_SEMAPHORE);
}

QueueHandle_t __wrap_xQueueCreateCountingSemaphoreStatic(uint32_t max, uint32_t init, void *sq)
{
    return __wrap_xQueueGenericCreateStatic(max, init, NULL, sq, queueQUEUE_TYPE_COUNTING_SEMAPHORE);
}

/* -------------------------------------------------------------------------
 * Semaphore init helper
 * ------------------------------------------------------------------------- */
static int sem_init(shim_sem_t *s, INT max_cnt, INT init_cnt)
{
    s->cnt     = init_cnt;
    s->max_cnt = max_cnt;
    /* Notification sem always starts at 0 regardless of init_cnt.
     * Fast-path Takes succeed without blocking when cnt>0.           */
    T_CSEM c = { .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = max_cnt };
    s->sem_id = tk_cre_sem(&c);
    return (s->sem_id > 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * xQueueGenericCreate
 * ------------------------------------------------------------------------- */
QueueHandle_t __wrap_xQueueGenericCreate(uint32_t len, uint32_t item_sz, uint8_t type)
{
    switch (type) {

    case queueQUEUE_TYPE_BASE: {
        if (item_sz == 0) goto binary_sem;
        shim_queue_t *q = alloc_handle(sizeof(*q), SHIM_TAG_QUEUE);
        if (!q) return NULL;
        if (queue_init(q, len, item_sz) < 0) {
            esp_rom_printf("[shim] xQueueCreate: ring buf alloc failed len=%u sz=%u\n",
                           (unsigned)len, (unsigned)item_sz);
            heap_caps_free(q); return NULL;
        }
        return (QueueHandle_t)q;
    }

    binary_sem:
    case queueQUEUE_TYPE_BINARY_SEMAPHORE: {
        shim_sem_t *s = alloc_handle(sizeof(*s), SHIM_TAG_SEM_BIN);
        if (!s) return NULL;
        if (sem_init(s, 1, 0) < 0) { heap_caps_free(s); return NULL; }
        return (QueueHandle_t)s;
    }

    case queueQUEUE_TYPE_COUNTING_SEMAPHORE: {
        shim_sem_t *s = alloc_handle(sizeof(*s), SHIM_TAG_SEM_CNT);
        if (!s) return NULL;
        /* len = max count, item_sz = initial count */
        if (sem_init(s, (INT)len, (INT)item_sz) < 0) { heap_caps_free(s); return NULL; }
        return (QueueHandle_t)s;
    }

    case queueQUEUE_TYPE_MUTEX: {
        shim_rmutex_t *rm = alloc_handle(sizeof(*rm), SHIM_TAG_RMUTEX);
        if (!rm) return NULL;
        T_CMTX c = { .mtxatr = TA_TFIFO | TA_INHERIT };
        rm->mtx_id = tk_cre_mtx(&c); rm->owner_task = 0; rm->depth = 0;
        if (rm->mtx_id <= 0) { heap_caps_free(rm); return NULL; }
        return (QueueHandle_t)rm;
    }

    case queueQUEUE_TYPE_RECURSIVE_MUTEX: {
        shim_rmutex_t *rm = alloc_handle(sizeof(*rm), SHIM_TAG_RMUTEX);
        if (!rm) return NULL;
        T_CMTX c = { .mtxatr = TA_TFIFO | TA_INHERIT };
        rm->mtx_id = tk_cre_mtx(&c); rm->owner_task = 0; rm->depth = 0;
        if (rm->mtx_id <= 0) { heap_caps_free(rm); return NULL; }
        return (QueueHandle_t)rm;
    }

    default:
        tm_printf((UB*)"[shim] xQueueGenericCreate: unknown type %u\n", (unsigned)type);
        return NULL;
    }
}

QueueHandle_t __wrap_xQueueGenericCreateStatic(uint32_t len, uint32_t item_sz,
    uint8_t *storage, void *sq, uint8_t type)
{
    (void)storage;
    if (!sq) return __wrap_xQueueGenericCreate(len, item_sz, type);

    switch (type) {
    case queueQUEUE_TYPE_BASE: {
        if (item_sz == 0) goto static_binary_sem;
        shim_queue_t *q = init_static_handle(sq, SHIM_TAG_QUEUE);
        if (queue_init(q, len, item_sz) < 0) return NULL;
        return (QueueHandle_t)sq;
    }
    static_binary_sem:
    case queueQUEUE_TYPE_BINARY_SEMAPHORE: {
        shim_sem_t *s = init_static_handle(sq, SHIM_TAG_SEM_BIN);
        if (sem_init(s, 1, 0) < 0) return NULL;
        return (QueueHandle_t)sq;
    }
    case queueQUEUE_TYPE_COUNTING_SEMAPHORE: {
        shim_sem_t *s = init_static_handle(sq, SHIM_TAG_SEM_CNT);
        if (sem_init(s, (INT)len, (INT)item_sz) < 0) return NULL;
        return (QueueHandle_t)sq;
    }
    case queueQUEUE_TYPE_MUTEX:
    case queueQUEUE_TYPE_RECURSIVE_MUTEX:
        return __wrap_xQueueCreateMutexStatic(type, sq);
    default:
        return __wrap_xQueueGenericCreate(len, item_sz, type);
    }
}

/* -------------------------------------------------------------------------
 * vQueueDelete
 * ------------------------------------------------------------------------- */
void __wrap_vQueueDelete(QueueHandle_t q)
{
    if (!q) return;
    shim_hdr_t *h = (shim_hdr_t *)q;
    bool is_static = h->is_static;
    switch (h->tag) {
    case SHIM_TAG_QUEUE: {
        shim_queue_t *p = q;
        if (p->nonempty > 0) tk_del_sem(p->nonempty);
        if (p->nonfull  > 0) tk_del_sem(p->nonfull);
        if (p->data) rbuf_free(p->data);
        break;
    }
    case SHIM_TAG_SEM_BIN:
    case SHIM_TAG_SEM_CNT: { shim_sem_t    *p = q; tk_del_sem(p->sem_id); break; }
    case SHIM_TAG_MUTEX:   { shim_mutex_t  *p = q; tk_del_mtx(p->mtx_id); break; }
    case SHIM_TAG_RMUTEX:  { shim_rmutex_t *p = q; tk_del_mtx(p->mtx_id); break; }
    default: break;
    }
    if (!is_static) heap_caps_free(q);
}

/* -------------------------------------------------------------------------
 * Semaphore Give helper — task or ISR context.
 *
 * Under DI (or native MIE=0 in ISR): increment cnt atomically.
 * Then tk_sig_sem (ISR-safe) wakes one blocked task receiver.
 * E_QOVR from tk_sig_sem is ignored; the sleeping task rechecks cnt.
 * ------------------------------------------------------------------------- */
static BaseType_t sem_give(shim_sem_t *s)
{
    UINT saved;
    DI(saved);
    if (s->cnt >= s->max_cnt) { EI(saved); return pdFALSE; }
    s->cnt++;
    EI(saved);
    tk_sig_sem(s->sem_id, 1); /* ISR-safe; E_QOVR harmless */
    return pdTRUE;
}

/* -------------------------------------------------------------------------
 * xQueueGenericSend — SEND_TO_BACK=0, SEND_TO_FRONT=1, OVERWRITE=2
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xQueueGenericSend(QueueHandle_t q, const void *item,
    TickType_t ticks, BaseType_t copy_pos)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;

    if (h->tag == SHIM_TAG_QUEUE) {
        shim_queue_t *p = q;

        if (copy_pos == queueOVERWRITE || shim_in_ddsp() || ticks == 0) {
            /* Non-blocking: ISR/critical-section/poll or overwrite */
            return queue_push(p, item, copy_pos == queueOVERWRITE);
        }

        /* Blocking send: wait for space, then push */
        TMO tmo = shim_tmo(ticks);
        while (true) {
            if (queue_push(p, item, false) == pdTRUE) return pdTRUE;
            ER er = tk_wai_sem(p->nonfull, 1, tmo);
            if (er != E_OK) return pdFALSE;
            /* Stale or real signal; recheck via loop */
        }
    }

    if (h->tag == SHIM_TAG_SEM_BIN || h->tag == SHIM_TAG_SEM_CNT)
        return sem_give((shim_sem_t *)q);

    if (h->tag == SHIM_TAG_MUTEX) {
        shim_mutex_t *m = q;
        return (tk_unl_mtx(m->mtx_id) == E_OK) ? pdTRUE : pdFALSE;
    }
    if (h->tag == SHIM_TAG_RMUTEX)
        return __wrap_xQueueGiveMutexRecursive(q);
    return pdFALSE;
}

/* -------------------------------------------------------------------------
 * xQueueGenericSendFromISR — non-blocking ISR send
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xQueueGenericSendFromISR(QueueHandle_t q, const void *item,
    BaseType_t *pw, BaseType_t copy_pos)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag == SHIM_TAG_QUEUE) {
        BaseType_t r = queue_push((shim_queue_t *)q, item,
                                  copy_pos == queueOVERWRITE);
        if (pw) *pw = pdFALSE;
        return r;
    }
    if (h->tag == SHIM_TAG_SEM_BIN || h->tag == SHIM_TAG_SEM_CNT) {
        BaseType_t r = sem_give((shim_sem_t *)q);
        if (pw) *pw = pdFALSE;
        return r;
    }
    return pdFALSE;
}

/* -------------------------------------------------------------------------
 * xQueueReceive — blocking task receive
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xQueueReceive(QueueHandle_t q, void *buf, TickType_t ticks)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;

    if (h->tag == SHIM_TAG_QUEUE) {
        shim_queue_t *p = q;

        if (shim_in_ddsp() || ticks == 0) {
            return queue_pop(p, buf);
        }

        TMO tmo = shim_tmo(ticks);
        while (true) {
            if (queue_pop(p, buf) == pdTRUE) return pdTRUE;
            ER er = tk_wai_sem(p->nonempty, 1, tmo);
            if (er != E_OK) return pdFALSE;
        }
    }
    return __wrap_xQueueSemaphoreTake(q, ticks);
}

/* -------------------------------------------------------------------------
 * xQueueReceiveFromISR — non-blocking ISR receive (THE FIX)
 *
 * Previously called tk_rcv_mbf(TMO_POL) which unconditionally returns E_CTX
 * from ISR context (CHECK_DISPATCH() fires on taskindp>0).  The ring buffer
 * and volatile cnt are directly accessible under MIE=0.
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xQueueReceiveFromISR(QueueHandle_t q, void *buf, BaseType_t *pw)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;

    if (h->tag == SHIM_TAG_QUEUE) {
        BaseType_t r = queue_pop((shim_queue_t *)q, buf);
        if (pw) *pw = pdFALSE;
        return r;
    }

    /* Semaphore TakeFromISR: atomic decrement of cnt (no tk_wai_sem needed) */
    if (h->tag == SHIM_TAG_SEM_BIN || h->tag == SHIM_TAG_SEM_CNT) {
        shim_sem_t *s = q;
        /* MIE=0 in ISR; DI/EI are no-ops but kept for correctness in task context */
        UINT saved;
        DI(saved);
        BaseType_t r = pdFALSE;
        if (s->cnt > 0) { s->cnt--; r = pdTRUE; }
        EI(saved);
        if (pw) *pw = pdFALSE;
        return r;
    }
    return pdFALSE;
}

/* -------------------------------------------------------------------------
 * xQueueSemaphoreTake — blocking task take
 *
 * Fast path: if cnt>0, decrement atomically (no kernel call).
 * Slow path: block on sem_id notification, recheck cnt on wake (handles
 *            spurious wakeups caused by ISR-context takes stealing cnt).
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xQueueSemaphoreTake(QueueHandle_t q, TickType_t ticks)
{
    if (!q) return pdFALSE;
    /* IDF VFS select cleanup passes a tag value, not a pointer */
    switch ((uint32_t)q) {
    case SHIM_TAG_QUEUE: case SHIM_TAG_SEM_BIN: case SHIM_TAG_SEM_CNT:
    case SHIM_TAG_MUTEX: case SHIM_TAG_RMUTEX:  case SHIM_TAG_TIMER:
    case SHIM_TAG_EVTGRP:
        return pdFALSE;
    default: break;
    }
    shim_hdr_t *h = (shim_hdr_t *)q;
    switch (h->tag) {
    case SHIM_TAG_SEM_BIN:
    case SHIM_TAG_SEM_CNT: {
        shim_sem_t *s = q;
        /* Fast path */
        {
            UINT saved;
            DI(saved);
            if (s->cnt > 0) {
                s->cnt--;
                EI(saved);
                /* Consume one stale notification if present */
                tk_wai_sem(s->sem_id, 1, TMO_POL);
                return pdTRUE;
            }
            EI(saved);
        }
        TMO tmo = shim_tmo(ticks);
        if (tmo == TMO_POL) return pdFALSE;
        /* Slow path: retry loop to handle spurious wakeups */
        while (true) {
            ER er = tk_wai_sem(s->sem_id, 1, tmo);
            if (er != E_OK) return pdFALSE;
            UINT saved;
            DI(saved);
            if (s->cnt > 0) { s->cnt--; EI(saved); return pdTRUE; }
            EI(saved);
            /* Spurious (ISR stole cnt between signal and our wake); retry */
        }
    }
    case SHIM_TAG_MUTEX: {
        shim_mutex_t *m = q;
        return (tk_loc_mtx(m->mtx_id, shim_tmo(ticks)) == E_OK) ? pdTRUE : pdFALSE;
    }
    case SHIM_TAG_RMUTEX: {
        shim_rmutex_t *rm = q;
        ID cur = tk_get_tid();
        if (rm->owner_task == cur) { rm->depth++; return pdTRUE; }
        ER er = tk_loc_mtx(rm->mtx_id, shim_tmo(ticks));
        if (er == E_OK) { rm->owner_task = cur; rm->depth = 1; }
        return (er == E_OK) ? pdTRUE : pdFALSE;
    }
    case SHIM_TAG_QUEUE: {
        shim_queue_t *p = q;
        if (shim_in_ddsp() || ticks == 0) {
            uint8_t tmp[p->item_sz > 0 ? p->item_sz : 1];
            return queue_pop(p, tmp);
        }
        TMO tmo = shim_tmo(ticks);
        uint8_t tmp[p->item_sz > 0 ? p->item_sz : 1];
        while (true) {
            if (queue_pop(p, tmp) == pdTRUE) return pdTRUE;
            ER er = tk_wai_sem(p->nonempty, 1, tmo);
            if (er != E_OK) return pdFALSE;
        }
    }
    default: return pdFALSE;
    }
}

BaseType_t __wrap_xQueueGiveFromISR(QueueHandle_t q, BaseType_t *pw)
{
    if (pw) *pw = pdFALSE;
    return __wrap_xQueueGenericSendFromISR(q, NULL, pw, queueSEND_TO_BACK);
}

/* -------------------------------------------------------------------------
 * Peek
 * ------------------------------------------------------------------------- */
BaseType_t __wrap_xQueuePeek(QueueHandle_t q, void *buf, TickType_t ticks)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag != SHIM_TAG_QUEUE) return pdFALSE;
    shim_queue_t *p = q;

    if (shim_in_ddsp() || ticks == 0) {
        UINT saved;
        DI(saved);
        if (p->head == p->tail) { EI(saved); return pdFALSE; }
        if (buf) memcpy(buf, p->data + p->head * p->item_sz, p->item_sz);
        EI(saved);
        return pdTRUE;
    }

    TMO tmo = shim_tmo(ticks);
    while (true) {
        UINT saved;
        DI(saved);
        if (p->head != p->tail) {
            if (buf) memcpy(buf, p->data + p->head * p->item_sz, p->item_sz);
            EI(saved);
            /* Put the notification credit back so a receiver can still wake */
            tk_sig_sem(p->nonempty, 1);
            return pdTRUE;
        }
        EI(saved);
        ER er = tk_wai_sem(p->nonempty, 1, tmo);
        if (er != E_OK) return pdFALSE;
    }
}

BaseType_t __wrap_xQueuePeekFromISR(QueueHandle_t q, void *buf)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag != SHIM_TAG_QUEUE) return pdFALSE;
    shim_queue_t *p = q;
    /* ISR: MIE=0 — direct ring buffer read without consuming */
    if (p->head == p->tail) return pdFALSE;
    if (buf) memcpy(buf, p->data + p->head * p->item_sz, p->item_sz);
    return pdTRUE;
}

/* -------------------------------------------------------------------------
 * Utilities
 * ------------------------------------------------------------------------- */
UBaseType_t __wrap_uxQueueMessagesWaiting(QueueHandle_t q)
{
    if (!q) return 0;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag == SHIM_TAG_QUEUE) {
        shim_queue_t *p = q;
        uint32_t t = p->tail, hd = p->head;
        return (UBaseType_t)((t >= hd) ? (t - hd) : (p->cap - hd + t));
    }
    if (h->tag == SHIM_TAG_SEM_BIN || h->tag == SHIM_TAG_SEM_CNT)
        return (UBaseType_t)((shim_sem_t *)q)->cnt;
    return 0;
}

UBaseType_t __wrap_uxQueueMessagesWaitingFromISR(QueueHandle_t q)
{
    return __wrap_uxQueueMessagesWaiting(q);
}

UBaseType_t __wrap_uxQueueSpacesAvailable(QueueHandle_t q)
{
    if (!q) return 0;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag == SHIM_TAG_QUEUE) {
        shim_queue_t *p = q;
        uint32_t used = __wrap_uxQueueMessagesWaiting(q);
        return (UBaseType_t)(p->cap > used ? p->cap - used - 1 : 0);
    }
    return 1; /* stub for other types */
}

BaseType_t __wrap_xQueueReset(QueueHandle_t q)
{
    if (!q) return pdTRUE;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag == SHIM_TAG_QUEUE) {
        shim_queue_t *p = q;
        UINT saved;
        DI(saved);
        p->head = p->tail = 0;
        EI(saved);
    }
    return pdTRUE;
}

/* Used by NimBLE npl_os_freertos.c: ble_npl_eventq_is_empty() */
BaseType_t __wrap_xQueueIsQueueEmptyFromISR(const QueueHandle_t q)
{
    if (!q) return pdTRUE;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag == SHIM_TAG_QUEUE) {
        shim_queue_t *p = (shim_queue_t *)q;
        return (p->head == p->tail) ? pdTRUE : pdFALSE;
    }
    if (h->tag == SHIM_TAG_SEM_BIN || h->tag == SHIM_TAG_SEM_CNT)
        return (((shim_sem_t *)q)->cnt == 0) ? pdTRUE : pdFALSE;
    return pdTRUE;
}

/* -------------------------------------------------------------------------
 * Mutex variants
 * ------------------------------------------------------------------------- */
QueueHandle_t __wrap_xQueueCreateMutex(uint8_t type)
{
    return __wrap_xQueueGenericCreate(1, 0, type);
}

QueueHandle_t __wrap_xQueueCreateMutexStatic(uint8_t type, void *sq)
{
    if (!sq) return __wrap_xQueueCreateMutex(type);

    if (type == queueQUEUE_TYPE_RECURSIVE_MUTEX) {
        shim_rmutex_t *rm = init_static_handle(sq, SHIM_TAG_RMUTEX);
        T_CMTX c = { .mtxatr = TA_TFIFO | TA_INHERIT };
        rm->mtx_id = tk_cre_mtx(&c); rm->owner_task = 0; rm->depth = 0;
        if (rm->mtx_id <= 0) return NULL;
        return (QueueHandle_t)rm;
    }
    shim_rmutex_t *rm2 = init_static_handle(sq, SHIM_TAG_RMUTEX);
    T_CMTX c2 = { .mtxatr = TA_TFIFO | TA_INHERIT };
    rm2->mtx_id = tk_cre_mtx(&c2); rm2->owner_task = 0; rm2->depth = 0;
    if (rm2->mtx_id <= 0) return NULL;
    return (QueueHandle_t)rm2;
}

BaseType_t __wrap_xQueueTakeMutexRecursive(QueueHandle_t q, TickType_t ticks)
{
    return __wrap_xQueueSemaphoreTake(q, ticks);
}

BaseType_t __wrap_xQueueGiveMutexRecursive(QueueHandle_t q)
{
    if (!q) return pdFALSE;
    shim_hdr_t *h = (shim_hdr_t *)q;
    if (h->tag == SHIM_TAG_RMUTEX) {
        shim_rmutex_t *rm = q;
        if (rm->depth <= 0) return pdFALSE;
        if (--rm->depth == 0) {
            rm->owner_task = 0;
            tk_unl_mtx(rm->mtx_id);
        }
        return pdTRUE;
    }
    return __wrap_xQueueGenericSend(q, NULL, 0, queueSEND_TO_BACK);
}
