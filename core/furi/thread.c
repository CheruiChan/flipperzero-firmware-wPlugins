#include "thread.h"
#include "memmgr.h"
#include "memmgr_heap.h"
#include "check.h"
#include "common_defines.h"

#include <task.h>
#include <m-string.h>

#define THREAD_NOTIFY_INDEX 1 // Index 0 is used for stream buffers

struct FuriThread {
    FuriThreadState state;
    int32_t ret;

    FuriThreadCallback callback;
    void* context;

    FuriThreadStateCallback state_callback;
    void* state_context;

    char* name;
    configSTACK_DEPTH_TYPE stack_size;
    FuriThreadPriority priority;

    TaskHandle_t task_handle;
    bool heap_trace_enabled;
    size_t heap_size;
};

/** Catch threads that are trying to exit wrong way */
__attribute__((__noreturn__)) void furi_thread_catch() {
    asm volatile("nop"); // extra magic
    furi_crash("You are doing it wrong");
}

static void furi_thread_set_state(FuriThread* thread, FuriThreadState state) {
    furi_assert(thread);
    thread->state = state;
    if(thread->state_callback) {
        thread->state_callback(state, thread->state_context);
    }
}

static void furi_thread_body(void* context) {
    furi_assert(context);
    FuriThread* thread = context;

    furi_assert(thread->state == FuriThreadStateStarting);
    furi_thread_set_state(thread, FuriThreadStateRunning);

    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    if(thread->heap_trace_enabled == true) {
        memmgr_heap_enable_thread_trace((FuriThreadId)task_handle);
    }

    thread->ret = thread->callback(thread->context);

    if(thread->heap_trace_enabled == true) {
        osDelay(33);
        thread->heap_size = memmgr_heap_get_thread_memory((FuriThreadId)task_handle);
        memmgr_heap_disable_thread_trace((FuriThreadId)task_handle);
    }

    furi_assert(thread->state == FuriThreadStateRunning);
    furi_thread_set_state(thread, FuriThreadStateStopped);

    vTaskDelete(thread->task_handle);
    furi_thread_catch();
}

FuriThread* furi_thread_alloc() {
    FuriThread* thread = malloc(sizeof(FuriThread));

    return thread;
}

void furi_thread_free(FuriThread* thread) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);

    if(thread->name) free((void*)thread->name);
    free(thread);
}

void furi_thread_set_name(FuriThread* thread, const char* name) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    if(thread->name) free((void*)thread->name);
    thread->name = strdup(name);
}

void furi_thread_set_stack_size(FuriThread* thread, size_t stack_size) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    furi_assert(stack_size % 4 == 0);
    thread->stack_size = stack_size;
}

void furi_thread_set_callback(FuriThread* thread, FuriThreadCallback callback) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    thread->callback = callback;
}

void furi_thread_set_context(FuriThread* thread, void* context) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    thread->context = context;
}

void furi_thread_set_priority(FuriThread* thread, FuriThreadPriority priority) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    furi_assert(priority >= FuriThreadPriorityIdle && priority <= FuriThreadPriorityIsr);
    thread->priority = priority;
}

void furi_thread_set_state_callback(FuriThread* thread, FuriThreadStateCallback callback) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    thread->state_callback = callback;
}

void furi_thread_set_state_context(FuriThread* thread, void* context) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    thread->state_context = context;
}

FuriThreadState furi_thread_get_state(FuriThread* thread) {
    furi_assert(thread);
    return thread->state;
}

void furi_thread_start(FuriThread* thread) {
    furi_assert(thread);
    furi_assert(thread->callback);
    furi_assert(thread->state == FuriThreadStateStopped);
    furi_assert(thread->stack_size > 0 && thread->stack_size < 0xFFFF * 4);

    furi_thread_set_state(thread, FuriThreadStateStarting);

    BaseType_t ret = xTaskCreate(
        furi_thread_body,
        thread->name,
        thread->stack_size / 4,
        thread,
        thread->priority ? thread->priority : FuriThreadPriorityNormal,
        &thread->task_handle);

    furi_check(ret == pdPASS);
    furi_check(thread->task_handle);
}

bool furi_thread_join(FuriThread* thread) {
    furi_assert(thread);

    while(thread->state != FuriThreadStateStopped) {
        osDelay(10);
    }

    return osOK;
}

FuriThreadId furi_thread_get_id(FuriThread* thread) {
    furi_assert(thread);
    return thread->task_handle;
}

void furi_thread_enable_heap_trace(FuriThread* thread) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    furi_assert(thread->heap_trace_enabled == false);
    thread->heap_trace_enabled = true;
}

void furi_thread_disable_heap_trace(FuriThread* thread) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    furi_assert(thread->heap_trace_enabled == true);
    thread->heap_trace_enabled = false;
}

size_t furi_thread_get_heap_size(FuriThread* thread) {
    furi_assert(thread);
    furi_assert(thread->heap_trace_enabled == true);
    return thread->heap_size;
}

int32_t furi_thread_get_return_code(FuriThread* thread) {
    furi_assert(thread);
    furi_assert(thread->state == FuriThreadStateStopped);
    return thread->ret;
}

FuriThreadId furi_thread_get_current_id() {
    return xTaskGetCurrentTaskHandle();
}

void furi_thread_yield() {
    furi_assert(!FURI_IS_IRQ_MODE());
    taskYIELD();
}

/* Limits */
#define MAX_BITS_TASK_NOTIFY 31U
#define MAX_BITS_EVENT_GROUPS 24U

#define THREAD_FLAGS_INVALID_BITS (~((1UL << MAX_BITS_TASK_NOTIFY) - 1U))
#define EVENT_FLAGS_INVALID_BITS (~((1UL << MAX_BITS_EVENT_GROUPS) - 1U))

uint32_t furi_thread_flags_set(FuriThreadId thread_id, uint32_t flags) {
    TaskHandle_t hTask = (TaskHandle_t)thread_id;
    uint32_t rflags;
    BaseType_t yield;

    if((hTask == NULL) || ((flags & THREAD_FLAGS_INVALID_BITS) != 0U)) {
        rflags = (uint32_t)osErrorParameter;
    } else {
        rflags = (uint32_t)osError;

        if(FURI_IS_IRQ_MODE()) {
            yield = pdFALSE;

            (void)xTaskNotifyIndexedFromISR(hTask, THREAD_NOTIFY_INDEX, flags, eSetBits, &yield);
            (void)xTaskNotifyAndQueryIndexedFromISR(
                hTask, THREAD_NOTIFY_INDEX, 0, eNoAction, &rflags, NULL);

            portYIELD_FROM_ISR(yield);
        } else {
            (void)xTaskNotifyIndexed(hTask, THREAD_NOTIFY_INDEX, flags, eSetBits);
            (void)xTaskNotifyAndQueryIndexed(hTask, THREAD_NOTIFY_INDEX, 0, eNoAction, &rflags);
        }
    }
    /* Return flags after setting */
    return (rflags);
}

uint32_t furi_thread_flags_clear(uint32_t flags) {
    TaskHandle_t hTask;
    uint32_t rflags, cflags;

    if(FURI_IS_IRQ_MODE()) {
        rflags = (uint32_t)osErrorISR;
    } else if((flags & THREAD_FLAGS_INVALID_BITS) != 0U) {
        rflags = (uint32_t)osErrorParameter;
    } else {
        hTask = xTaskGetCurrentTaskHandle();

        if(xTaskNotifyAndQueryIndexed(hTask, THREAD_NOTIFY_INDEX, 0, eNoAction, &cflags) ==
           pdPASS) {
            rflags = cflags;
            cflags &= ~flags;

            if(xTaskNotifyIndexed(hTask, THREAD_NOTIFY_INDEX, cflags, eSetValueWithOverwrite) !=
               pdPASS) {
                rflags = (uint32_t)osError;
            }
        } else {
            rflags = (uint32_t)osError;
        }
    }

    /* Return flags before clearing */
    return (rflags);
}

uint32_t furi_thread_flags_get(void) {
    TaskHandle_t hTask;
    uint32_t rflags;

    if(FURI_IS_IRQ_MODE()) {
        rflags = (uint32_t)osErrorISR;
    } else {
        hTask = xTaskGetCurrentTaskHandle();

        if(xTaskNotifyAndQueryIndexed(hTask, THREAD_NOTIFY_INDEX, 0, eNoAction, &rflags) !=
           pdPASS) {
            rflags = (uint32_t)osError;
        }
    }

    return (rflags);
}

uint32_t furi_thread_flags_wait(uint32_t flags, uint32_t options, uint32_t timeout) {
    uint32_t rflags, nval;
    uint32_t clear;
    TickType_t t0, td, tout;
    BaseType_t rval;

    if(FURI_IS_IRQ_MODE()) {
        rflags = (uint32_t)osErrorISR;
    } else if((flags & THREAD_FLAGS_INVALID_BITS) != 0U) {
        rflags = (uint32_t)osErrorParameter;
    } else {
        if((options & osFlagsNoClear) == osFlagsNoClear) {
            clear = 0U;
        } else {
            clear = flags;
        }

        rflags = 0U;
        tout = timeout;

        t0 = xTaskGetTickCount();
        do {
            rval = xTaskNotifyWaitIndexed(THREAD_NOTIFY_INDEX, 0, clear, &nval, tout);

            if(rval == pdPASS) {
                rflags &= flags;
                rflags |= nval;

                if((options & osFlagsWaitAll) == osFlagsWaitAll) {
                    if((flags & rflags) == flags) {
                        break;
                    } else {
                        if(timeout == 0U) {
                            rflags = (uint32_t)osErrorResource;
                            break;
                        }
                    }
                } else {
                    if((flags & rflags) != 0) {
                        break;
                    } else {
                        if(timeout == 0U) {
                            rflags = (uint32_t)osErrorResource;
                            break;
                        }
                    }
                }

                /* Update timeout */
                td = xTaskGetTickCount() - t0;

                if(td > tout) {
                    tout = 0;
                } else {
                    tout -= td;
                }
            } else {
                if(timeout == 0) {
                    rflags = (uint32_t)osErrorResource;
                } else {
                    rflags = (uint32_t)osErrorTimeout;
                }
            }
        } while(rval != pdFAIL);
    }

    /* Return flags before clearing */
    return (rflags);
}

uint32_t furi_thread_enumerate(FuriThreadId* thread_array, uint32_t array_items) {
    uint32_t i, count;
    TaskStatus_t* task;

    if(FURI_IS_IRQ_MODE() || (thread_array == NULL) || (array_items == 0U)) {
        count = 0U;
    } else {
        vTaskSuspendAll();

        count = uxTaskGetNumberOfTasks();
        task = pvPortMalloc(count * sizeof(TaskStatus_t));

        if(task != NULL) {
            count = uxTaskGetSystemState(task, count, NULL);

            for(i = 0U; (i < count) && (i < array_items); i++) {
                thread_array[i] = (FuriThreadId)task[i].xHandle;
            }
            count = i;
        }
        (void)xTaskResumeAll();

        vPortFree(task);
    }

    return (count);
}

const char* furi_thread_get_name(FuriThreadId thread_id) {
    TaskHandle_t hTask = (TaskHandle_t)thread_id;
    const char* name;

    if(FURI_IS_IRQ_MODE() || (hTask == NULL)) {
        name = NULL;
    } else {
        name = pcTaskGetName(hTask);
    }

    return (name);
}

uint32_t furi_thread_get_stack_space(FuriThreadId thread_id) {
    TaskHandle_t hTask = (TaskHandle_t)thread_id;
    uint32_t sz;

    if(FURI_IS_IRQ_MODE() || (hTask == NULL)) {
        sz = 0U;
    } else {
        sz = (uint32_t)(uxTaskGetStackHighWaterMark(hTask) * sizeof(StackType_t));
    }

    return (sz);
}
