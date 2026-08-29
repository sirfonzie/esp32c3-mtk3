#include "esp_attr.h"
#include <stdint.h>

/*
 * ke_event_env layout — ROM BSS at 0x3fcdfd90
 * [+0]      event_field: bitmask of pending events (bit N = event N pending)
 * [+4..+60] callbacks[0..14]: 15 handler function pointers
 * Confirmed by r_ke_event_init = memset(ke_event_env, 0, 64)
 */
typedef struct {
    uint32_t     event_field;
    void       (*callbacks[15])(void);
} ke_event_env_t;

extern ke_event_env_t ke_event_env;  /* ROM BSS at 0x3fcdfd90 */

/*
 * Safe replacement for ROM r_ke_event_schedule (0x40000df8).
 *
 * Covers dispatch-table code paths: silently clears any event whose callback
 * is NULL instead of crashing through r_assert_err → plf_funcs[158] = NULL.
 *
 * Installed at runtime via shim_ble_patch_modules_funcs() rather than as a
 * link-time symbol override (libbtdm_app.a is processed before
 * libfreertos_shim.a, making archive-order symbol override impossible).
 */
IRAM_ATTR static void ke_event_schedule_safe(void)
{
    while (ke_event_env.event_field) {
        int id = 31 - __builtin_clz(ke_event_env.event_field);
        if ((unsigned)id > 14u) {
            ke_event_env.event_field = 0;
            break;
        }
        void (*cb)(void) = ke_event_env.callbacks[id];
        if (cb) {
            cb();
        } else {
            ke_event_env.event_field &= ~(1u << id);
        }
    }
}

/*
 * Patch the dispatch table: r_modules_funcs_p[0x104/4] → ke_event_schedule_safe.
 * Must be called after btdm_controller_init() has populated r_modules_funcs_p
 * and called r_ke_event_init(), i.e. after nimble_port_init() returns.
 *
 *   r_modules_funcs_p is at ROM BSS 0x3fcdff88.
 *   Slot 0x104 = index 65 (confirmed from modules_funcs.o relocation table).
 *
 * This fixes the advertising-time MEPC=0 crash (ROM ke_event scheduler asserts
 * on a NULL event-6 callback).  The separate connection-time MEPC=0 crash had a
 * different root cause — ISR-context dispatch corruption, fixed in the port's
 * rtos_int_enter / __wrap_rtos_int_exit (knl_taskindp handling).
 */
void shim_ble_patch_modules_funcs(void)
{
    volatile uint32_t *const modules_funcs_p_cell = (volatile uint32_t *)0x3fcdff88u;
    uint32_t *modules_funcs = (uint32_t *)*modules_funcs_p_cell;
    if (!modules_funcs) {
        return;
    }
    modules_funcs[0x104 / 4] = (uint32_t)(uintptr_t)ke_event_schedule_safe;
}
