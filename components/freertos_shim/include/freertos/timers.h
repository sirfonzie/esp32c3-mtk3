#pragma once
#include "FreeRTOS.h"

typedef void * TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t timer);

TimerHandle_t xTimerCreate(const char *name, TickType_t period,
    BaseType_t auto_reload, void *timer_id, TimerCallbackFunction_t cb);
TimerHandle_t xTimerCreateStatic(const char *name, TickType_t period,
    BaseType_t auto_reload, void *timer_id, TimerCallbackFunction_t cb,
    void *static_timer);

BaseType_t xTimerIsTimerActive(TimerHandle_t t);
BaseType_t xTimerStart(TimerHandle_t t, TickType_t ticks_to_wait);
BaseType_t xTimerStop(TimerHandle_t t, TickType_t ticks_to_wait);
BaseType_t xTimerReset(TimerHandle_t t, TickType_t ticks_to_wait);
BaseType_t xTimerChangePeriod(TimerHandle_t t, TickType_t new_period,
    TickType_t ticks_to_wait);
BaseType_t xTimerDelete(TimerHandle_t t, TickType_t ticks_to_wait);

BaseType_t xTimerStartFromISR(TimerHandle_t t, BaseType_t *pw);
BaseType_t xTimerStopFromISR(TimerHandle_t t, BaseType_t *pw);
BaseType_t xTimerResetFromISR(TimerHandle_t t, BaseType_t *pw);
BaseType_t xTimerChangePeriodFromISR(TimerHandle_t t, TickType_t p, BaseType_t *pw);

void *     pvTimerGetTimerID(TimerHandle_t t);
void       vTimerSetTimerID(TimerHandle_t t, void *id);
const char *pcTimerGetName(TimerHandle_t t);
TickType_t  xTimerGetPeriod(TimerHandle_t t);
TickType_t  xTimerGetExpiryTime(TimerHandle_t t);
