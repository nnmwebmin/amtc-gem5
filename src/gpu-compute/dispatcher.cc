/*
 * Copyright (c) 2011-2015,2018 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "gpu-compute/dispatcher.hh"

#include "debug/GPUAgentDisp.hh"
#include "debug/GPUDisp.hh"
#include "debug/GPUKernelInfo.hh"
#include "debug/GPUWgLatency.hh"
#include "gpu-compute/gpu_command_processor.hh"
#include "gpu-compute/hsa_queue_entry.hh"
#include "gpu-compute/shader.hh"
#include "gpu-compute/wavefront.hh"
#include "sim/sim_exit.hh"
#include "sim/syscall_emul_buf.hh"
#include "sim/system.hh"

namespace gem5
{

GPUDispatcher::GPUDispatcher(const Params &p)
    : SimObject(p), shader(nullptr), gpuCmdProc(nullptr),
      tickEvent([this]{ exec(); },
          "GPU Dispatcher tick", false, Event::CPU_Tick_Pri),
      dispatchActive(false), kernelExitEvents(p.kernel_exit_events),
      stats(this)
{
    schedule(&tickEvent, 0);
}

GPUDispatcher::~GPUDispatcher()
{
}

HSAQueueEntry*
GPUDispatcher::hsaTask(int disp_id)
{
    assert(hsaQueueEntries.find(disp_id) != hsaQueueEntries.end());
    return hsaQueueEntries[disp_id];
}

void
GPUDispatcher::setCommandProcessor(GPUCommandProcessor *gpu_cmd_proc)
{
    gpuCmdProc = gpu_cmd_proc;
}

void
GPUDispatcher::setShader(Shader *new_shader)
{
    shader = new_shader;
}

void
GPUDispatcher::serialize(CheckpointOut &cp) const
{
    Tick event_tick = 0;

    if (tickEvent.scheduled())
        event_tick = tickEvent.when();

    SERIALIZE_SCALAR(event_tick);
}

void
GPUDispatcher::unserialize(CheckpointIn &cp)
{
    Tick event_tick;

    if (tickEvent.scheduled())
        deschedule(&tickEvent);

    UNSERIALIZE_SCALAR(event_tick);

    if (event_tick) {
        schedule(&tickEvent, event_tick);
    }
}

/**
 * After all relevant HSA data structures have been traversed/extracted
 * from memory by the CP, dispatch() is called on the dispatcher. This will
 * schedule a dispatch event that, when triggered, will attempt to dispatch
 * the WGs associated with the given task to the CUs.
 */
void
GPUDispatcher::dispatch(HSAQueueEntry *task)
{
    ++stats.numKernelLaunched;
    warn("AMTC_DISPATCH_ENTER tick=%llu task=%p queue_id=%u dispatch_id=%d "
         "completion=%#llx grid=(%u,%u,%u) wg=(%u,%u,%u) "
         "execIds_before=%zu active_before=%d\n",
         (unsigned long long)curTick(),
         task,
         task->queueId(),
         task->dispatchId(),
         (unsigned long long)task->completionSignal(),
         task->gridSize(0),
         task->gridSize(1),
         task->gridSize(2),
         task->wgSize(0),
         task->wgSize(1),
         task->wgSize(2),
         execIds.size(),
         dispatchActive);

    DPRINTF(GPUDisp, "launching kernel: %s, dispatch ID: %d\n",
            task->kernelName(), task->dispatchId());
    DPRINTF(GPUAgentDisp, "launching kernel: %s, dispatch ID: %d\n",
            task->kernelName(), task->dispatchId());

    execIds.push(task->dispatchId());
    dispatchActive = true;
    hsaQueueEntries.emplace(task->dispatchId(), task);

    warn("AMTC_DISPATCH_QUEUED tick=%llu task=%p dispatch_id=%d "
         "execIds_after=%zu active=%d tickEvent_scheduled=%d\n",
         (unsigned long long)curTick(), task, task->dispatchId(),
         execIds.size(), dispatchActive, tickEvent.scheduled());

    if (!tickEvent.scheduled()) {
        Tick when = curTick() + shader->clockPeriod();
        warn("AMTC_DISPATCH_EVENT_SCHEDULE tick=%llu fire_at=%llu "
             "task=%p dispatch_id=%d\n",
             (unsigned long long)curTick(), (unsigned long long)when,
             task, task->dispatchId());
        schedule(&tickEvent, when);
    } else {
        warn("AMTC_DISPATCH_EVENT_ALREADY_SCHEDULED tick=%llu "
             "task=%p dispatch_id=%d\n",
             (unsigned long long)curTick(), task, task->dispatchId());
    }
}

void
GPUDispatcher::exec()
{
    int fail_count(0);
    int disp_count(0);

    warn("AMTC_DISPATCH_EXEC_ENTER tick=%llu execIds=%zu doneIds=%zu "
         "dispatchActive=%d tickEvent_scheduled=%d\n",
         (unsigned long long)curTick(), execIds.size(), doneIds.size(),
         dispatchActive, tickEvent.scheduled());

    /**
     * There are potentially multiple outstanding kernel launches.
     * It is possible that the workgroups in a different kernel
     * can fit on the GPU even if another kernel's workgroups cannot
     */
    DPRINTF(GPUDisp, "Launching %d Kernels\n", execIds.size());
    DPRINTF(GPUAgentDisp, "Launching %d Kernels\n", execIds.size());

    if (execIds.size() > 0) {
        ++stats.cyclesWaitingForDispatch;
    }

    /**
     * dispatch work cannot start until the kernel's invalidate is
     * completely finished; hence, kernel will always initiates
     * invalidate first and keeps waiting until inv done
     */
    while (execIds.size() > fail_count) {
        int exec_id = execIds.front();
        auto task = hsaQueueEntries[exec_id];
        bool launched(false);

        int inv_done_before = 0;
        if (task->outstandingInvs() >= 0) {
            inv_done_before = task->isInvDone();
        }

        warn("AMTC_DISPATCH_EXEC_TASK tick=%llu exec_id=%d task=%p "
             "queue_id=%u completion=%#llx outstandingInvs=%d "
             "outstandingWbs=%d invDone=%d dispComplete=%d "
             "wg_done=%d wg_total=%d fail_count=%d execIds=%zu "
             "tickEvent_scheduled=%d dispatchActive=%d "
             "note=invDone_guarded_to_avoid_negative_assert\n",
             (unsigned long long)curTick(), exec_id, task,
             task->queueId(),
             (unsigned long long)task->completionSignal(),
             task->outstandingInvs(), task->outstandingWbs(),
             inv_done_before, task->dispComplete(),
             task->numWgCompleted(), task->numWgTotal(),
             fail_count, execIds.size(), tickEvent.scheduled(),
             dispatchActive);

        // acq is needed before starting dispatch
        if (shader->impl_kern_launch_acq) {
            warn("AMTC_DISPATCH_PREPARE_INV_ENTER tick=%llu exec_id=%d "
                 "task=%p outstandingInvs_before=%d\n",
                 (unsigned long long)curTick(), exec_id, task,
                 task->outstandingInvs());
            // try to invalidate cache
            shader->prepareInvalidate(task);
            int inv_done_after = 0;
            if (task->outstandingInvs() >= 0) {
                inv_done_after = task->isInvDone();
            }
            warn("AMTC_DISPATCH_PREPARE_INV_EXIT tick=%llu exec_id=%d "
                 "task=%p outstandingInvs_after=%d inv_done_after=%d\n",
                 (unsigned long long)curTick(), exec_id, task,
                 task->outstandingInvs(), inv_done_after);
        } else {
            warn("AMTC_DISPATCH_SKIP_INV tick=%llu exec_id=%d task=%p\n",
                 (unsigned long long)curTick(), exec_id, task);
            // kern launch acquire is not set, skip invalidate
            task->markInvDone();
        }

        /**
         * invalidate is still ongoing, put the kernel on the queue to
         * retry later
         */
        int inv_done_now = 0;
        if (task->outstandingInvs() >= 0) {
            inv_done_now = task->isInvDone();
        }

        if (!inv_done_now){
            warn("AMTC_DISPATCH_WAIT_INV tick=%llu exec_id=%d task=%p "
                 "outstandingInvs=%d inv_done_now=%d fail_count_next=%d\n",
                 (unsigned long long)curTick(), exec_id, task,
                 task->outstandingInvs(), inv_done_now, fail_count + 1);
            execIds.push(exec_id);
            ++fail_count;

            DPRINTF(GPUDisp, "kernel %d failed to launch, due to [%d] pending"
                " invalidate requests\n", exec_id, task->outstandingInvs());

            // try the next kernel_id
            execIds.pop();
            continue;
        }

        // kernel invalidate is done, start workgroup dispatch
        while (!task->dispComplete()) {
            // update the thread context
            shader->updateContext(task->contextId());

            // attempt to dispatch workgroup
            DPRINTF(GPUWgLatency, "Attempt Kernel Launch cycle:%d kernel:%d\n",
                curTick(), exec_id);

            warn("AMTC_DISPATCH_WG_ATTEMPT tick=%llu exec_id=%d task=%p "
                 "queue_id=%u completion=%#llx wg_done=%d wg_total=%d "
                 "dispComplete=%d outstandingInvs=%d outstandingWbs=%d "
                 "launched_in_this_exec=%d fail_count=%d execIds=%zu\n",
                 (unsigned long long)curTick(), exec_id, task,
                 task->queueId(),
                 (unsigned long long)task->completionSignal(),
                 task->numWgCompleted(), task->numWgTotal(),
                 task->dispComplete(), task->outstandingInvs(),
                 task->outstandingWbs(), launched, fail_count,
                 execIds.size());

            if (!shader->dispatchWorkgroups(task)) {
                warn("AMTC_DISPATCH_WG_FAIL_DETAIL tick=%llu exec_id=%d "
                     "task=%p queue_id=%u completion=%#llx wg_done=%d "
                     "wg_total=%d dispComplete=%d outstandingInvs=%d "
                     "outstandingWbs=%d launched_in_this_exec=%d "
                     "disp_count=%d fail_count_next=%d execIds_before_push=%zu "
                     "tickEvent_scheduled=%d note=dispatchWorkgroups_returned_false\n",
                     (unsigned long long)curTick(), exec_id, task,
                     task->queueId(),
                     (unsigned long long)task->completionSignal(),
                     task->numWgCompleted(), task->numWgTotal(),
                     task->dispComplete(), task->outstandingInvs(),
                     task->outstandingWbs(), launched, disp_count,
                     fail_count + 1, execIds.size(), tickEvent.scheduled());
                warn("AMTC_DISPATCH_WG_FAIL tick=%llu exec_id=%d task=%p "
                     "wg_done=%d wg_total=%d fail_count_next=%d\n",
                     (unsigned long long)curTick(), exec_id, task,
                     task->numWgCompleted(), task->numWgTotal(),
                     fail_count + 1);
                /**
                 * if we failed try the next kernel,
                 * it may have smaller workgroups.
                 * put it on the queue to retry later
                 */
                DPRINTF(GPUDisp, "kernel %d failed to launch\n", exec_id);
                execIds.push(exec_id);
                warn("AMTC_DISPATCH_REQUEUE_AFTER_WG_FAIL tick=%llu "
                     "exec_id=%d task=%p execIds_after_push=%zu "
                     "fail_count_after=%d tickEvent_scheduled=%d "
                     "note=waiting_for_wg_completion_or_future_schedule\n",
                     (unsigned long long)curTick(), exec_id, task,
                     execIds.size(), fail_count + 1,
                     tickEvent.scheduled());
                ++fail_count;
                break;
            } else if (!launched) {
                warn("AMTC_DISPATCH_WG_LAUNCHED tick=%llu exec_id=%d "
                     "task=%p disp_count_next=%d wg_done=%d wg_total=%d\n",
                     (unsigned long long)curTick(), exec_id, task,
                     disp_count + 1, task->numWgCompleted(),
                     task->numWgTotal());
                launched = true;
                disp_count++;
                DPRINTF(GPUKernelInfo, "Launched kernel %d for WG %d\n",
                            exec_id, disp_count);
            }
        }

        // try the next kernel_id
        execIds.pop();
    }

    warn("AMTC_DISPATCH_EXEC_EXIT tick=%llu disp_count=%d fail_count=%d "
         "execIds_remaining=%zu doneIds=%zu tickEvent_scheduled=%d "
         "dispatchActive=%d\n",
         (unsigned long long)curTick(), disp_count, fail_count,
         execIds.size(), doneIds.size(), tickEvent.scheduled(),
         dispatchActive);

    if (execIds.size() > 0 && !tickEvent.scheduled()) {
        warn("AMTC_DISPATCH_STALLED_NO_EVENT tick=%llu "
             "execIds_remaining=%zu doneIds=%zu disp_count=%d "
             "fail_count=%d note=no_dispatch_event_scheduled_after_requeue;"
             " expect_notifyWgCompl_or_external_schedule_to_wake_dispatcher\n",
             (unsigned long long)curTick(), execIds.size(),
             doneIds.size(), disp_count, fail_count);
    }

    if (disp_count == 0 && fail_count > 0 && execIds.size() > 0) {
        int dbg_exec_id = execIds.front();
        auto dbg_task = hsaQueueEntries[dbg_exec_id];
        warn("AMTC_DISPATCH_NO_LAUNCH_PROGRESS tick=%llu "
             "front_exec_id=%d task=%p queue_id=%u completion=%#llx "
             "wg_done=%d wg_total=%d dispComplete=%d outstandingInvs=%d "
             "outstandingWbs=%d fail_count=%d execIds=%zu\n",
             (unsigned long long)curTick(), dbg_exec_id, dbg_task,
             dbg_task->queueId(),
             (unsigned long long)dbg_task->completionSignal(),
             dbg_task->numWgCompleted(), dbg_task->numWgTotal(),
             dbg_task->dispComplete(), dbg_task->outstandingInvs(),
             dbg_task->outstandingWbs(), fail_count, execIds.size());
    }

    DPRINTF(GPUDisp, "Returning %d Kernels\n", doneIds.size());
    DPRINTF(GPUWgLatency, "Kernel Wgs dispatched: %d | %d failures\n",
            disp_count, fail_count);

    while (doneIds.size()) {
        DPRINTF(GPUDisp, "Kernel %d completed\n", doneIds.front());
        doneIds.pop();
    }
}

bool
GPUDispatcher::isReachingKernelEnd(Wavefront *wf)
{
    int kern_id = wf->kernId;
    assert(hsaQueueEntries.find(kern_id) != hsaQueueEntries.end());
    auto task = hsaQueueEntries[kern_id];
    assert(task->dispatchId() == kern_id);

    /**
     * whether the next workgroup is the final one in the kernel,
     * +1 as we check first before taking action
     */
    return (task->numWgCompleted() + 1 == task->numWgTotal());
}

/**
 * update the counter of oustanding inv requests for the kernel
 * kern_id: kernel id
 * val: +1/-1, increment or decrement the counter (default: -1)
 */
void
GPUDispatcher::updateInvCounter(int kern_id, int val) {
    assert(val == -1 || val == 1);

    auto task = hsaQueueEntries[kern_id];
    int before = task->outstandingInvs();
    task->updateOutstandingInvs(val);
    int after = task->outstandingInvs();
    int inv_done = 0;
    if (after >= 0) {
        inv_done = task->isInvDone();
    }
    warn("AMTC_DISPATCH_INV_COUNTER tick=%llu kern_id=%d val=%d "
         "before=%d after=%d inv_done=%d tickEvent_scheduled=%d\n",
         (unsigned long long)curTick(), kern_id, val, before,
         after, inv_done, tickEvent.scheduled());

    // kernel invalidate is done, schedule dispatch work
    if (inv_done && !tickEvent.scheduled()) {
        Tick when = curTick() + shader->clockPeriod();
        warn("AMTC_DISPATCH_INV_DONE_SCHEDULE tick=%llu fire_at=%llu "
             "kern_id=%d\n",
             (unsigned long long)curTick(), (unsigned long long)when,
             kern_id);
        schedule(&tickEvent, when);
    }
}

/**
 * update the counter of oustanding wb requests for the kernel
 * kern_id: kernel id
 * val: +1/-1, increment or decrement the counter (default: -1)
 *
 * return true if all wbs are done for the kernel
 */
bool
GPUDispatcher::updateWbCounter(int kern_id, int val) {
    assert(val == -1 || val == 1);

    auto task = hsaQueueEntries[kern_id];
    int before = task->outstandingWbs();
    task->updateOutstandingWbs(val);
    warn("AMTC_DISPATCH_WB_COUNTER tick=%llu kern_id=%d val=%d "
         "before=%d after=%d\n",
         (unsigned long long)curTick(), kern_id, val, before,
         task->outstandingWbs());

    // true: WB is done, false: WB is still ongoing
    return (task->outstandingWbs() == 0);
}

/**
 * get kernel's outstanding cache writeback requests
 */
int
GPUDispatcher::getOutstandingWbs(int kernId) {
    auto task = hsaQueueEntries[kernId];

    return task->outstandingWbs();
}

/**
 * When an end program instruction detects that the last WF in
 * a WG has completed it will call this method on the dispatcher.
 * If we detect that this is the last WG for the given task, then
 * we ring the completion signal, which is used by the CPU to
 * synchronize with the GPU. The HSAPP is also notified that the
 * task has completed so it can be removed from its task queues.
 */
void
GPUDispatcher::notifyWgCompl(Wavefront *wf)
{
    int kern_id = wf->kernId;
    DPRINTF(GPUDisp, "notify WgCompl %d\n", wf->wgId);
    auto task = hsaQueueEntries[kern_id];
    assert(task->dispatchId() == kern_id);
    warn("AMTC_DISPATCH_WG_COMPLETE_ENTER tick=%llu kern_id=%d wg=%d "
         "cu=%d task=%p wg_done_before=%d wg_total=%d\n",
         (unsigned long long)curTick(), kern_id, wf->wgId,
         wf->computeUnit->cu_id, task, task->numWgCompleted(),
         task->numWgTotal());
    task->notifyWgCompleted();
    warn("AMTC_DISPATCH_WG_COMPLETE_AFTER tick=%llu kern_id=%d wg=%d "
         "task=%p wg_done_after=%d wg_total=%d dispComplete=%d "
         "outstandingInvs=%d outstandingWbs=%d completion=%#llx\n",
         (unsigned long long)curTick(), kern_id, wf->wgId, task,
         task->numWgCompleted(), task->numWgTotal(),
         task->dispComplete(), task->outstandingInvs(),
         task->outstandingWbs(),
         (unsigned long long)task->completionSignal());

    DPRINTF(GPUWgLatency, "WG Complete cycle:%d wg:%d kernel:%d cu:%d\n",
        curTick(), wf->wgId, kern_id, wf->computeUnit->cu_id);

    if (task->numWgCompleted() == task->numWgTotal()) {
        warn("AMTC_DISPATCH_KERNEL_DONE tick=%llu kern_id=%d task=%p "
             "queue_id=%u completion=%#llx\n",
             (unsigned long long)curTick(), kern_id, task,
             task->queueId(),
             (unsigned long long)task->completionSignal());
        // Notify the HSA PP that this kernel is complete
        warn("AMTC_DISPATCH_CALL_FINISHPKT tick=%llu kern_id=%d "
             "queue_id=%u pkt=%p\n",
             (unsigned long long)curTick(), kern_id, task->queueId(),
             task->dispPktPtr());
        gpuCmdProc->hsaPacketProc()
            .finishPkt(task->dispPktPtr(), task->queueId());
        if (task->completionSignal()) {
            DPRINTF(GPUDisp, "HSA AQL Kernel Complete with completion "
                    "signal! Addr: %d\n", task->completionSignal());

            gpuCmdProc->sendCompletionSignal(task->completionSignal());
        } else {
            DPRINTF(GPUDisp, "HSA AQL Kernel Complete! No completion "
                "signal\n");
        }

        DPRINTF(GPUWgLatency, "Kernel Complete ticks:%d kernel:%d\n",
                curTick(), kern_id);
        DPRINTF(GPUKernelInfo, "Completed kernel %d\n", kern_id);

        if (kernelExitEvents) {
            shader->requestKernelExitEvent(task->completionSignal());
        }
    }

    if (!tickEvent.scheduled()) {
        Tick when = curTick() + shader->clockPeriod();
        warn("AMTC_DISPATCH_WG_COMPLETE_SCHEDULE tick=%llu fire_at=%llu "
             "kern_id=%d execIds=%zu doneIds=%zu\n",
             (unsigned long long)curTick(), (unsigned long long)when,
             kern_id, execIds.size(), doneIds.size());
        schedule(&tickEvent, when);
    } else {
        warn("AMTC_DISPATCH_WG_COMPLETE_SCHEDULE_SKIPPED tick=%llu "
             "kern_id=%d already_scheduled=1 execIds=%zu doneIds=%zu\n",
             (unsigned long long)curTick(), kern_id,
             execIds.size(), doneIds.size());
    }
}

void
GPUDispatcher::scheduleDispatch()
{
    if (!tickEvent.scheduled()) {
        Tick when = curTick() + shader->clockPeriod();
        warn("AMTC_DISPATCH_EXTERNAL_SCHEDULE tick=%llu fire_at=%llu\n",
             (unsigned long long)curTick(), (unsigned long long)when);
        schedule(&tickEvent, when);
    } else {
        warn("AMTC_DISPATCH_EXTERNAL_SCHEDULE_IGNORED tick=%llu "
             "already_scheduled=1\n",
             (unsigned long long)curTick());
    }
}

GPUDispatcher::GPUDispatcherStats::GPUDispatcherStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numKernelLaunched, "number of kernel launched"),
      ADD_STAT(cyclesWaitingForDispatch, "number of cycles with outstanding "
               "wavefronts that are waiting to be dispatched")
{
}

} // namespace gem5
