#pragma once
#include "FreeRTOS.h"
#include "queue.h"

typedef QueueHandle_t SemaphoreHandle_t;

/* Binary semaphore */
#define xSemaphoreCreateBinary() \
    xQueueGenericCreate(1, 0, queueQUEUE_TYPE_BINARY_SEMAPHORE)

/* Counting semaphore */
#define xSemaphoreCreateCounting(max, init) \
    xQueueGenericCreate((max), 0, queueQUEUE_TYPE_COUNTING_SEMAPHORE)

/* Mutex */
#define xSemaphoreCreateMutex() \
    xQueueCreateMutex(queueQUEUE_TYPE_MUTEX)

/* Recursive mutex */
#define xSemaphoreCreateRecursiveMutex() \
    xQueueCreateMutex(queueQUEUE_TYPE_RECURSIVE_MUTEX)

/* Take / Give — route through queue layer (which dispatches on tag) */
#define xSemaphoreTake(sem, ticks)  xQueueSemaphoreTake((sem),(ticks))
#define xSemaphoreGive(sem)         xQueueGenericSend((sem),NULL,0,queueSEND_TO_BACK)
#define xSemaphoreGiveFromISR(sem,pw) xQueueGiveFromISR((sem),(pw))
#define xSemaphoreTakeFromISR(sem,pw) xQueueReceiveFromISR((sem),NULL,(pw))

#define xSemaphoreTakeRecursive(m,t) xQueueTakeMutexRecursive((m),(t))
#define xSemaphoreGiveRecursive(m)   xQueueGiveMutexRecursive(m)

#define vSemaphoreDelete(sem)  vQueueDelete(sem)
#define uxSemaphoreGetCount(sem) uxQueueMessagesWaiting(sem)
