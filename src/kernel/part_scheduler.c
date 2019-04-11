#include <stm32f4xx_hal.h>
#include <stdbool.h>
#include <tgmath.h>

#include "drivers/time_get.h"

#include "kernel/context.h"
#include "kernel/error_handler.h"
#include "kernel/part_scheduler.h"

#include "../xml_data.h"

#include "window.h"
#include "ports.h"

static window_t schedule[MAX_NUM_WINDOWS];
static int32_t majorFrameMillis = -1;
static window_t* activeWindow;

//#define DISABLE_CONTEXT_SWITCH
#ifndef DISABLE_CONTEXT_SWITCH
/**
* @brief Handles SysTick interrupts. This should fire once every millisecond.
* This function also handles context switches. This function is called directly
* by the NVIC.
* Note: This function is declared as naked to tell GCC not to add the usual padding,
*       which is disastrous in a context switing function
* TODO: This function does not handle context switches from functions that use the FPU,
*			as the FPU registers are not all saved
*/
__attribute__((naked,noreturn))
void SysTick_Handler(void)
{
    uint32_t stackpointer = context_save();
    uint32_t exc_return_value;
    __asm volatile ("MOV %0, LR" : "=r" (exc_return_value));


    static process_t* active_process = NULL;
    if (active_process != NULL)                                                  // ALWAYS NULL
    {
        // Save the value of the stack pointer for later use.
        active_process->stackpointer = stackpointer;
        active_process->exc_return_value = (exc_return_value & 0xFF);
    }

    /* Get pointer to currently running partition. */
    partition_t *part = scheduler_partitionScheduler();

    active_process = scheduler_processScheduler(part);

    // Increase HAL ticks. This is by the HAL_Delay(int) function, so we need to do this.
    HAL_IncTick();
    TIME_Add_Count();

    // Restore the context and switch to it. This function never returns
    context_restoreAndSwitch(active_process->stackpointer, active_process->exc_return_value);
}
#else
void SysTick_Handler(void)
{
    HAL_IncTick();
    TIME_Add_Count();
}
#endif

partition_t* scheduler_partitionScheduler(void)
{
    static int8_t activeWindowIndex = -1;
    static uint32_t majorFrameStopMillis = 0;

    uint32_t posMajorFrameMillis = HAL_GetTick() % majorFrameSeconds;
    if(majorFrameStopMillis <= posMajorFrameMillis || posMajorFrameMillis == 0) {

        // Have we reached the end of a major frame?
        if(posMajorFrameMillis == 0)
        {
            // If so, we need to start a new one
            activeWindowIndex = -1;
        }
        // Get the next active window, possibly skipping over any unused windows
        do
        {
            activeWindow = &schedule[++activeWindowIndex];
        } while (activeWindow->partition == NULL);
        majorFrameStopMillis = activeWindow->frameStartTimeMillis + activeWindow->durationMillis;

        //curr_partition_id = activeWindow->partition->IDENTIFIER;
    }

    return activeWindow->partition;
}

process_t* scheduler_processScheduler(partition_t *part)
{
    size_t i;
    /* Set state of currently running process to READY. */
    //process_t *curr_process = &part->processes[part->index_running_process];
    //	if (curr_process->PROCESS_STATE == RUNNING) {
    //		curr_process->PROCESS_STATE = READY;
    //	}

    /* Get list of processes in partition. */
    process_t *processes = part->processes;

    /* Find a READY partition as default candidate for execution. */
    process_t *priority_process;
    for (i = 0; i < MAX_PROCESSES_PER_PARTITIONS; i++) {
        //printf("%d - %d - %d\n", indexActivePartition, i, processes[i].PROCESS_STATE);
        if (processes[i].PROCESS_STATE == READY) {
            priority_process = &processes[i];
            break;
        }
    }

    /* Attempt to find a better candidate by comparing to default. */
    for ( ; i < MAX_PROCESSES_PER_PARTITIONS; i++) {
        if (processes[i].PROCESS_STATE != READY)
        continue;

        if (processes[i].CURRENT_PRIORITY < priority_process->CURRENT_PRIORITY)
        continue;

        if (processes[i].tickStamp < priority_process->tickStamp) {
            priority_process = &processes[i];
            part->index_running_process = i;
        }
    }

    /* Set state of process to RUNNING. */
    //priority_process->PROCESS_STATE = RUNNING;

    /* Return pointer to highest priority candidate process */
    return priority_process;
}

partition_t* getActivePartition(void)
{
    return activeWindow->partition;
}

void process_stop_self(void)
{
    partition_t *partition = activeWindow->partition;
    process_t *proc = &partition->processes[partition->index_running_process];

    proc->PROCESS_STATE = DORMANT;
}

void scheduler_buildSchedule(void)
{
    bool doubleBreak = false;
    uint32_t i, j, windowIndex = 0, numTotalWindows = 0, numGeneratedWindows = 0;
    uint32_t desiredStartTime = 0, earliestNextStartTime = 50000;
    const partition_schedule* tmpPSchedule;
    const window_schedule* tmpWSchedule;
    partition_t* tmpPartition;

    // The goal: Create an ordered list of windows. Windows should be ordered
    // by the time inside a frame in which they run.
    // The problem: Our source data (from the XML) is not ordered.
    // The solution: Search through all the partitions and all the windows to
    // find the next. When we have the next, insert it into the list.
    // We know the start time and the duration of all windows. With this it
    // should be easy to find the next.
    // If we cannot find a window that start at
    // (window-1).startTime + (window-1).duration, we assume it's an implicit
    // idle, and we generate an idle window for that.

    // Count the total number of windows first
    const uint32_t numPartitionSchedules = (uint32_t) sizeof(partition_schedules) / sizeof(partition_schedule);
    for(i = 0; i < numPartitionSchedules; i++ )
    {
        tmpPSchedule = &partition_schedules[i];
        for(j = 0; j < tmpPSchedule->numWindows; j++)
        {
            numTotalWindows++;
        }
    }

    while(numGeneratedWindows < numTotalWindows)
    {
        // Reset pointer
        tmpWSchedule = NULL;

        // Iterate through all partition schedules
        for(i = 0, doubleBreak = false; i < numPartitionSchedules; i++ )
        {
            if(doubleBreak == true) break;
            tmpPSchedule = &partition_schedules[i];

            // Iterate through all windows in the partition
            for(j = 0; j < tmpPSchedule->numWindows; j++)
            {
                uint32_t windowStartTime = tmpPSchedule->window_arr[j].windowstartmilliseconds;
                if(windowStartTime == desiredStartTime)
                {
                    tmpWSchedule = &tmpPSchedule->window_arr[j];
                    doubleBreak = true;
                    break;
                }
                else
                {
                    // Is this start time after the desired start time (i.e. a window we haven't
                    // already put in the schedule), and is it the earliest we've otherwise seen so far?
                    if(windowStartTime > desiredStartTime && windowStartTime < earliestNextStartTime)
                    {
                        earliestNextStartTime = windowStartTime;
                    }
                }
            }
        }

        if(tmpWSchedule != NULL) // A matching window was found
        {
            // Find the partition by ID
            tmpPartition = NULL;
            const uint32_t numPartitions = (uint32_t) sizeof(partitions) / sizeof(partition_t);
            for(i = 0; i < numPartitions; i++)
            {
                if (partitions[i].id == tmpPSchedule->id)
                {
                    tmpPartition = &partitions[i];
                    break;
                }
            }

            // Create a window
            schedule[windowIndex++] = (window_t) {
                .partition = tmpPartition,
                .frameStartTimeMillis = tmpWSchedule->windowstartmilliseconds,
                .durationMillis = tmpWSchedule->windowdurationmilliseconds,
            };
            desiredStartTime = desiredStartTime + tmpWSchedule->windowdurationmilliseconds;
        }
        else // Generate an idle window
        {
            schedule[windowIndex++] = (window_t) {
                .partition = &partitions[0],
                .frameStartTimeMillis = desiredStartTime,
                .durationMillis = earliestNextStartTime - desiredStartTime,
            };
            desiredStartTime = earliestNextStartTime;
            earliestNextStartTime = 50000;
            numTotalWindows++;
        }

        numGeneratedWindows++;
        if(numGeneratedWindows > MAX_NUM_WINDOWS)
        {
            /* KERNEL PANIC! */
            Error_Handler();
        }
    }

    majorFrameMillis = majorFrameSeconds;
}
