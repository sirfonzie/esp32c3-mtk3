#pragma once
#include "FreeRTOS.h"

typedef void * QueueHandle_t;
typedef void * QueueSetHandle_t;
typedef void * QueueSetMemberHandle_t;

/* queueSEND_TO_BACK / FRONT / OVERWRITE match OSI constants */
#define queueSEND_TO_BACK      0
#define queueSEND_TO_FRONT     1
#define queueOVERWRITE         2

/* Creation flags (isMutex / isSemaphore): internal IDF use */
#define queueQUEUE_TYPE_BASE           0
#define queueQUEUE_TYPE_MUTEX          1
#define queueQUEUE_TYPE_COUNTING_SEMAPHORE 2
#define queueQUEUE_TYPE_BINARY_SEMAPHORE   3
#define queueQUEUE_TYPE_RECURSIVE_MUTEX    4

QueueHandle_t xQueueGenericCreate(uint32_t len, uint32_t item_sz, uint8_t type);
QueueHandle_t xQueueGenericCreateStatic(uint32_t len, uint32_t item_sz,
    uint8_t *storage, void *static_queue, uint8_t type);

BaseType_t xQueueGenericSend(QueueHandle_t q, const void *item,
    TickType_t ticks, BaseType_t copy_pos);
BaseType_t xQueueGenericSendFromISR(QueueHandle_t q, const void *item,
    BaseType_t *pxHigherPriorityTaskWoken, BaseType_t copy_pos);

BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t ticks);
BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *buf,
    BaseType_t *pxHigherPriorityTaskWoken);

BaseType_t xQueuePeek(QueueHandle_t q, void *buf, TickType_t ticks);
BaseType_t xQueuePeekFromISR(QueueHandle_t q, void *buf);

BaseType_t xQueueSemaphoreTake(QueueHandle_t q, TickType_t ticks);
BaseType_t xQueueGiveFromISR(QueueHandle_t q, BaseType_t *pxHigherPriorityTaskWoken);

void       vQueueDelete(QueueHandle_t q);

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t q);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
BaseType_t  xQueueReset(QueueHandle_t q);

/* Mutex variants */
QueueHandle_t xQueueCreateMutex(uint8_t type);
QueueHandle_t xQueueCreateMutexStatic(uint8_t type, void *static_queue);
BaseType_t    xQueueTakeMutexRecursive(QueueHandle_t mtx, TickType_t ticks);
BaseType_t    xQueueGiveMutexRecursive(QueueHandle_t mtx);

/* Convenience macros that IDF code sometimes uses */
#define xQueueCreate(len, isz)  xQueueGenericCreate((len),(isz),queueQUEUE_TYPE_BASE)
#define xQueueSend(q,i,t)       xQueueGenericSend((q),(i),(t),queueSEND_TO_BACK)
#define xQueueSendToBack(q,i,t) xQueueGenericSend((q),(i),(t),queueSEND_TO_BACK)
#define xQueueSendToFront(q,i,t) xQueueGenericSend((q),(i),(t),queueSEND_TO_FRONT)
#define xQueueSendFromISR(q,i,w) xQueueGenericSendFromISR((q),(i),(w),queueSEND_TO_BACK)
#define xQueueOverwrite(q,i)    xQueueGenericSend((q),(i),0,queueOVERWRITE)
#define xQueueOverwriteFromISR(q,i,w) xQueueGenericSendFromISR((q),(i),(w),queueOVERWRITE)
