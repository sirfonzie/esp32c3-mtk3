#pragma once
#include "FreeRTOS.h"

typedef void * TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

/* Task creation attributes (subset used by IDF) */
#define tskIDLE_PRIORITY     0U
#define tskNO_AFFINITY       (-1)

/* Task notification action values */
#define eNoAction            0
#define eSetBits             1
#define eIncrement           2
#define eSetValueWithOverwrite 3
#define eSetValueWithoutOverwrite 4
typedef uint32_t eNotifyAction;

/* --- declarations (resolved by shim_task.c via --wrap) --- */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
    uint32_t stack_words, void *param, UBaseType_t prio,
    TaskHandle_t *out, BaseType_t core);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
    uint32_t stack_words, void *param, UBaseType_t prio,
    TaskHandle_t *out);

void       vTaskDelete(TaskHandle_t h);
void       vTaskDelay(TickType_t ticks);
void       vTaskDelayUntil(TickType_t *prev, TickType_t incr);

TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);

TaskHandle_t xTaskGetCurrentTaskHandle(void);
TaskHandle_t xTaskGetHandle(const char *name);

BaseType_t xTaskGetSchedulerState(void);

UBaseType_t uxTaskPriorityGet(TaskHandle_t h);
void        vTaskPrioritySet(TaskHandle_t h, UBaseType_t prio);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h);

/* Task notifications */
BaseType_t  xTaskNotifyGive(TaskHandle_t h);
void        xTaskNotifyGiveFromISR(TaskHandle_t h, BaseType_t *pxHigherPriorityTaskWoken);
uint32_t    ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
BaseType_t  xTaskNotify(TaskHandle_t h, uint32_t val, eNotifyAction act);
BaseType_t  xTaskNotifyFromISR(TaskHandle_t h, uint32_t val, eNotifyAction act,
                                BaseType_t *pxHigherPriorityTaskWoken);
BaseType_t  xTaskNotifyWait(uint32_t bits_to_clr_on_entry, uint32_t bits_to_clr_on_exit,
                             uint32_t *pulNotificationValue, TickType_t ticks_to_wait);

/* Port */
BaseType_t  xPortGetCoreID(void);
