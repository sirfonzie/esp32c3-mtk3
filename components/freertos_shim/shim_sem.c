#include "freertos/shim_internal.h"
#include "freertos/semphr.h"

/*
 * shim_sem.c — semaphore/mutex creation helpers.
 *
 * The actual take/give runtime is handled by shim_queue.c because FreeRTOS
 * routes everything through xQueue* under the hood (semphr.h macros expand to
 * xQueueSemaphoreTake, xQueueGenericSend, etc.).  This file exists to satisfy
 * any direct calls to xSemaphoreCreate* that don't expand through semphr.h
 * macros — in practice those are rare but the linker may pull them in.
 *
 * All work is delegated to xQueueGenericCreate in shim_queue.c.
 */

/* Nothing to implement here for now — all creation paths go through
 * xQueueGenericCreate (which shim_queue.c wraps) via the semphr.h macros.
 * If the linker reports unresolved xSemaphoreCreate* symbols, add direct
 * wrappers here. */
