#include "freertos/shim_internal.h"
#include "freertos/event_groups.h"
#include "esp_rom_sys.h"

/*
 * Event groups → µT-Kernel event flags (tk_cre_flg).
 *
 * FreeRTOS EventBits_t is a uint32_t bitmask (top 8 bits reserved internally;
 * usable bits 0-23).  µT-Kernel UINT flag patterns are at least 16 bits
 * (32 bits on ESP32-C3 RV32).  Direct mapping works.
 *
 * Wait modes:
 *   wait_all=pdTRUE  → TWF_ANDW (all bits must be set)
 *   wait_all=pdFALSE → TWF_ORW  (any bit suffices)
 * µT-Kernel does not auto-clear; we clear manually after wai_flg returns.
 */

EventGroupHandle_t __wrap_xEventGroupCreate(void)
{
    shim_evtgrp_t *eg = heap_caps_malloc(sizeof(*eg), MALLOC_CAP_DEFAULT);
    if (!eg) return NULL;
    eg->hdr.tag = SHIM_TAG_EVTGRP;

    T_CFLG c = { .flgatr = TA_TFIFO | TA_WMUL, .iflgptn = 0 };
    eg->flg_id = tk_cre_flg(&c);
    if (eg->flg_id <= 0) { heap_caps_free(eg); return NULL; }
    return (EventGroupHandle_t)eg;
}

EventGroupHandle_t __wrap_xEventGroupCreateStatic(void *static_eg)
{
    (void)static_eg;
    return __wrap_xEventGroupCreate(); /* ignore static buffer */
}

void __wrap_vEventGroupDelete(EventGroupHandle_t eg)
{
    if (!eg) return;
    shim_evtgrp_t *p = eg;
    tk_del_flg(p->flg_id);
    heap_caps_free(p);
}

EventBits_t __wrap_xEventGroupSetBits(EventGroupHandle_t eg, EventBits_t bits)
{
    if (!eg) return 0;
    shim_evtgrp_t *p = eg;
    tk_set_flg(p->flg_id, (UINT)bits);
    /* Return current pattern — read via ref_flg */
    T_RFLG rflg;
    tk_ref_flg(p->flg_id, &rflg);
    return (EventBits_t)rflg.flgptn;
}

BaseType_t __wrap_xEventGroupSetBitsFromISR(EventGroupHandle_t eg, EventBits_t bits,
    BaseType_t *pw)
{
    if (!eg) return pdFALSE;
    shim_evtgrp_t *p = eg;
    ER er = tk_set_flg(p->flg_id, (UINT)bits);
    if (pw) *pw = pdFALSE;
    return (er == E_OK) ? pdTRUE : pdFALSE;
}

EventBits_t __wrap_xEventGroupClearBits(EventGroupHandle_t eg, EventBits_t bits)
{
    if (!eg) return 0;
    shim_evtgrp_t *p = eg;
    T_RFLG rflg;
    tk_ref_flg(p->flg_id, &rflg);
    EventBits_t before = (EventBits_t)rflg.flgptn;
    tk_clr_flg(p->flg_id, ~(UINT)bits);
    return before;
}

BaseType_t __wrap_xEventGroupClearBitsFromISR(EventGroupHandle_t eg, EventBits_t bits)
{
    if (!eg) return pdFALSE;
    shim_evtgrp_t *p = eg;
    tk_clr_flg(p->flg_id, ~(UINT)bits);
    return pdTRUE;
}

EventBits_t __wrap_xEventGroupWaitBits(EventGroupHandle_t eg, EventBits_t bits,
    BaseType_t clr_on_exit, BaseType_t wait_all, TickType_t ticks)
{
    if (!eg) return 0;
    shim_evtgrp_t *p = eg;

    UINT mode = wait_all ? TWF_ANDW : TWF_ORW;
    UINT result = 0;
    esp_rom_printf("[shim] wai_flg id=%d bits=0x%x tsk=%d\n",
                   (int)p->flg_id, (unsigned)bits, (int)tk_get_tid());
    ER er = tk_wai_flg(p->flg_id, (UINT)bits, mode, &result, shim_tmo(ticks));
    esp_rom_printf("[shim] wai_flg id=%d -> %d ptn=0x%x\n",
                   (int)p->flg_id, (int)er, (unsigned)result);

    if (er == E_OK && clr_on_exit)
        tk_clr_flg(p->flg_id, ~result);

    return (EventBits_t)result;
}

EventBits_t __wrap_xEventGroupGetBits(EventGroupHandle_t eg)
{
    if (!eg) return 0;
    shim_evtgrp_t *p = eg;
    T_RFLG rflg;
    if (tk_ref_flg(p->flg_id, &rflg) != E_OK) return 0;
    return (EventBits_t)rflg.flgptn;
}

EventBits_t __wrap_xEventGroupGetBitsFromISR(EventGroupHandle_t eg)
{
    return __wrap_xEventGroupGetBits(eg);
}

EventBits_t __wrap_xEventGroupSync(EventGroupHandle_t eg,
    EventBits_t set, EventBits_t wait, TickType_t ticks)
{
    __wrap_xEventGroupSetBits(eg, set);
    return __wrap_xEventGroupWaitBits(eg, wait, pdTRUE, pdTRUE, ticks);
}
