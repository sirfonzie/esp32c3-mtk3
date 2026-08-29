#pragma once
#include "FreeRTOS.h"

typedef void * EventGroupHandle_t;
typedef uint32_t EventBits_t;

EventGroupHandle_t xEventGroupCreate(void);
EventGroupHandle_t xEventGroupCreateStatic(void *static_eg);
void               vEventGroupDelete(EventGroupHandle_t eg);

EventBits_t xEventGroupSetBits(EventGroupHandle_t eg, EventBits_t bits);
BaseType_t  xEventGroupSetBitsFromISR(EventGroupHandle_t eg, EventBits_t bits,
                                      BaseType_t *pxHigherPriorityTaskWoken);
EventBits_t xEventGroupClearBits(EventGroupHandle_t eg, EventBits_t bits);
BaseType_t  xEventGroupClearBitsFromISR(EventGroupHandle_t eg, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t eg, EventBits_t bits,
                                BaseType_t clr_on_exit, BaseType_t wait_all,
                                TickType_t ticks);
EventBits_t xEventGroupGetBits(EventGroupHandle_t eg);
EventBits_t xEventGroupGetBitsFromISR(EventGroupHandle_t eg);
EventBits_t xEventGroupSync(EventGroupHandle_t eg, EventBits_t set,
                             EventBits_t wait, TickType_t ticks);
