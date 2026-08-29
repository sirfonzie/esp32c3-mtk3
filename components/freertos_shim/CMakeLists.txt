cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        shim_port.c
        shim_task.c
        shim_queue.c
        shim_sem.c
        shim_evtgrp.c
        shim_timer.c
        shim_stubs.c
        shim_ble_patch.c
    INCLUDE_DIRS include
    REQUIRES mtkernel esp_wifi lwip esp_netif esp_event esp_timer nvs_flash esp_hw_support esp_system bt)

# ---------------------------------------------------------------------------
# --wrap redirects: every FreeRTOS symbol the IDF network stack resolves to
# goes through our shim instead of the dead FreeRTOS kernel.  The list grows
# as stubs fire during bring-up; add new entries here as needed.
# ---------------------------------------------------------------------------
target_link_options(${COMPONENT_LIB} INTERFACE
    # Force-include one symbol from each shim object so the linker pulls the
    # whole object from the archive.  Without this, --wrap creates the
    # undefined __wrap_* references only when libesp_wifi.a is processed,
    # which is after the first scan of libfreertos_shim.a.
    "-Wl,-u,__wrap_xPortInIsrContext"
    "-Wl,-u,__wrap_xTaskCreatePinnedToCore"
    "-Wl,-u,__wrap_xQueueGenericCreate"
    "-Wl,-u,__wrap_xEventGroupCreate"
    "-Wl,-u,__wrap_xTimerCreate"
    "-Wl,-u,rtos_int_enter"     # force shim_stubs.c into archive scan (replaces portasm.S)
    "-Wl,-u,shim_ble_patch_modules_funcs" # pull shim_ble_patch.c into link (unique symbol, no conflict)
    # --- kernel object-table idempotency (second call from knl_main is NOP) ---
    "-Wl,--wrap=knl_init_object"
    # --- port / scheduler state ---
    "-Wl,--wrap=xPortInIsrContext"
    "-Wl,--wrap=vPortEnterCritical"
    "-Wl,--wrap=vPortExitCritical"
    "-Wl,--wrap=vPortEnterCriticalFromISR"
    "-Wl,--wrap=vPortExitCriticalFromISR"
    "-Wl,--wrap=xTaskGetSchedulerState"
    "-Wl,--wrap=xPortGetCoreID"
    # --- tasks ---
    "-Wl,--wrap=xTaskCreatePinnedToCore"
    "-Wl,--wrap=xTaskCreate"
    "-Wl,--wrap=vTaskDelete"
    "-Wl,--wrap=vTaskDelay"
    "-Wl,--wrap=vTaskDelayUntil"
    "-Wl,--wrap=xTaskGetTickCount"
    "-Wl,--wrap=xTaskGetTickCountFromISR"
    "-Wl,--wrap=xTaskGetCurrentTaskHandle"
    "-Wl,--wrap=uxTaskPriorityGet"
    "-Wl,--wrap=vTaskPrioritySet"
    "-Wl,--wrap=uxTaskGetStackHighWaterMark"
    "-Wl,--wrap=xTaskGetHandle"
    # --- thread-local storage (used by IDF pthread → wifi_thread_semphr) ---
    "-Wl,--wrap=pvTaskGetThreadLocalStoragePointer"
    "-Wl,--wrap=vTaskSetThreadLocalStoragePointer"
    "-Wl,--wrap=vTaskSetThreadLocalStoragePointerAndDelCallback"
    # --- task notifications (non-Generic names are inline macros → Generic is the linker symbol) ---
    "-Wl,--wrap=xTaskNotifyGive"
    "-Wl,--wrap=xTaskNotifyGiveFromISR"
    "-Wl,--wrap=ulTaskNotifyTake"
    "-Wl,--wrap=xTaskNotify"
    "-Wl,--wrap=xTaskNotifyFromISR"
    "-Wl,--wrap=xTaskNotifyWait"
    "-Wl,--wrap=ulTaskGenericNotifyTake"
    "-Wl,--wrap=xTaskGenericNotify"
    "-Wl,--wrap=xTaskGenericNotifyFromISR"
    "-Wl,--wrap=vTaskGenericNotifyGiveFromISR"
    "-Wl,--wrap=xTaskGenericNotifyWait"
    "-Wl,--wrap=xTaskGenericNotifyStateClear"
    "-Wl,--wrap=ulTaskGenericNotifyValueClear"
    # --- queues (also back semaphores — FreeRTOS semphr.h macros to xQueue*) ---
    # xQueueCreateCountingSemaphore is NOT a macro — it is a real function in queue.c that
    # calls xQueueGenericCreate internally (bypassing --wrap).  Wrap it directly so that
    # bt.c's semphr_create_wrapper (which calls xSemaphoreCreateCounting → this function)
    # produces a shim_sem_t instead of a raw FreeRTOS Queue_t.
    "-Wl,--wrap=xQueueCreateCountingSemaphore"
    "-Wl,--wrap=xQueueCreateCountingSemaphoreStatic"
    "-Wl,--wrap=xQueueGenericCreate"
    "-Wl,--wrap=xQueueGenericCreateStatic"
    "-Wl,--wrap=xQueueGenericSend"
    "-Wl,--wrap=xQueueGenericSendFromISR"
    "-Wl,--wrap=xQueueReceive"
    "-Wl,--wrap=xQueueReceiveFromISR"
    "-Wl,--wrap=xQueuePeek"
    "-Wl,--wrap=xQueuePeekFromISR"
    "-Wl,--wrap=xQueueSemaphoreTake"
    "-Wl,--wrap=xQueueGiveFromISR"
    "-Wl,--wrap=vQueueDelete"
    "-Wl,--wrap=uxQueueMessagesWaiting"
    "-Wl,--wrap=uxQueueMessagesWaitingFromISR"
    "-Wl,--wrap=uxQueueSpacesAvailable"
    "-Wl,--wrap=xQueueReset"
    "-Wl,--wrap=xQueueIsQueueEmptyFromISR"
    # --- mutexes (also routed through generic queue in FreeRTOS) ---
    "-Wl,--wrap=xQueueCreateMutex"
    "-Wl,--wrap=xQueueCreateMutexStatic"
    "-Wl,--wrap=xQueueTakeMutexRecursive"
    "-Wl,--wrap=xQueueGiveMutexRecursive"
    # --- event groups ---
    "-Wl,--wrap=xEventGroupCreate"
    "-Wl,--wrap=xEventGroupCreateStatic"
    "-Wl,--wrap=xEventGroupSetBits"
    "-Wl,--wrap=xEventGroupSetBitsFromISR"
    "-Wl,--wrap=xEventGroupClearBits"
    "-Wl,--wrap=xEventGroupClearBitsFromISR"
    "-Wl,--wrap=xEventGroupWaitBits"
    "-Wl,--wrap=xEventGroupGetBits"
    "-Wl,--wrap=xEventGroupGetBitsFromISR"
    "-Wl,--wrap=xEventGroupSync"
    "-Wl,--wrap=vEventGroupDelete"
    # --- software timers ---
    "-Wl,--wrap=xTimerCreate"
    "-Wl,--wrap=xTimerCreateStatic"
    "-Wl,--wrap=xTimerIsTimerActive"
    "-Wl,--wrap=xTimerStart"
    "-Wl,--wrap=xTimerStop"
    "-Wl,--wrap=xTimerReset"
    "-Wl,--wrap=xTimerChangePeriod"
    "-Wl,--wrap=xTimerDelete"
    "-Wl,--wrap=xTimerStartFromISR"
    "-Wl,--wrap=xTimerStopFromISR"
    "-Wl,--wrap=xTimerResetFromISR"
    "-Wl,--wrap=xTimerChangePeriodFromISR"
    "-Wl,--wrap=pvTimerGetTimerID"
    "-Wl,--wrap=vTimerSetTimerID"
    "-Wl,--wrap=pcTimerGetName"
    "-Wl,--wrap=xTimerGetPeriod"
    "-Wl,--wrap=xTimerGetExpiryTime"
    # --- spinlocks (IDF portMUX_TYPE) ---
    "-Wl,--wrap=spinlock_initialize"
    "-Wl,--wrap=spinlock_acquire"
    "-Wl,--wrap=spinlock_release"
    # --- heap (keep IDF heap_caps; these are FreeRTOS heap wrappers) ---
    "-Wl,--wrap=pvPortMalloc"
    "-Wl,--wrap=vPortFree"
    "-Wl,--wrap=pvPortCalloc"
    "-Wl,--wrap=pvPortRealloc"
    "-Wl,--wrap=xPortGetFreeHeapSize"
    "-Wl,--wrap=xPortGetMinimumEverFreeHeapSize"
)
