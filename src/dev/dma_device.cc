/*
 * Copyright (c) 2012, 2015, 2017, 2019 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "dev/dma_device.hh"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/DMA.hh"
#include "debug/Drain.hh"
#include "sim/clocked_object.hh"
#include "sim/system.hh"

namespace gem5
{

namespace
{

constexpr Addr AmtcDmaTraceBase = 0x11b4000;
constexpr Addr AmtcDmaTraceMask = ~Addr(0x3ff);

static inline bool
amtcTraceDmaAddr(Addr addr)
{
    return (addr & AmtcDmaTraceMask) == AmtcDmaTraceBase;
}

static inline bool
amtcTraceDmaPort(const std::string &port_name)
{
    return port_name.find("hsapp.dma") != std::string::npos;
}

} // anonymous namespace

DmaPort::DmaPort(ClockedObject *dev, System *s, std::optional<uint32_t> sid,
                 std::optional<uint32_t> ssid)
    : RequestPort(dev->name() + ".dma"),
      device(dev),
      sys(s),
      requestorId(s->getRequestorId(dev)),
      sendEvent([this] { sendDma(); }, dev->name()),
      amtcDmaWatchdogEvent([this] { amtcDmaWatchdog(); },
                           dev->name() + ".amtcDmaWatchdog"),
      defaultSid(sid),
      defaultSSid(ssid),
      cacheLineSize(s->cacheLineSize())
{ }

void
DmaPort::amtcDmaWatchdog()
{
    if (!amtcDmaTraceValid) {
        return;
    }

    const Tick age = curTick() - amtcDmaTraceIssueTick;

    warn("AMTC_DMADEV_STUCK_PENDING tick=%llu port=%s addr=%#lx "
         "size=%u cmd=%s age=%llu pending=%u retryPending=%d "
         "inRetry=%p inRetry_addr=%#lx inRetry_size=%u inRetry_cmd=%s "
         "transmitList=%llu sendEvent=%d\n",
         (unsigned long long)curTick(), name().c_str(),
         (unsigned long)amtcDmaTraceAddr, amtcDmaTraceSize,
         amtcDmaTraceCmd.c_str(), (unsigned long long)age, pendingCount,
         retryPending, inRetry,
         inRetry ? (unsigned long)inRetry->getAddr() : 0UL,
         inRetry ? inRetry->getSize() : 0,
         inRetry ? inRetry->cmdString().c_str() : "none",
         (unsigned long long)transmitList.size(), sendEvent.scheduled());

    if ((pendingCount != 0 || retryPending || inRetry) &&
        !amtcDmaWatchdogEvent.scheduled()) {
        device->schedule(amtcDmaWatchdogEvent, curTick() + 5000000);
    }
}

void
DmaPort::handleRespPacket(PacketPtr pkt, Tick delay)
{
    // Should always see a response with a sender state.
    assert(pkt->isResponse());
    warn_if(pkt->isError(), "Response pkt error.");

    // Get the DMA sender state.
    auto *state = dynamic_cast<DmaReqState*>(pkt->senderState);
    assert(state);

    handleResp(state, pkt->getAddr(), pkt->req->getSize(), delay);

    delete pkt;
}

void
DmaPort::handleResp(DmaReqState *state, Addr addr, Addr size, Tick delay)
{
    assert(pendingCount != 0);
    pendingCount--;
    DPRINTF(DMA, "Received response %s for addr: %#x size: %d nb: %d,"  \
            " tot: %d sched %d\n",
            MemCmd(state->cmd).toString(), addr, size,
            state->numBytes, state->totBytes,
            state->completionEvent ?
            state->completionEvent->scheduled() : 0);

    if (amtcTraceDmaAddr(addr) || amtcTraceDmaPort(name())) {
        warn("AMTC_DMADEV_RESP_HANDLE tick=%llu port=%s cmd=%s addr=%#lx "
             "size=%lu pending_after_dec=%u numBytes_before=%lu totBytes=%lu "
             "completion_scheduled=%d\n",
             (unsigned long long)curTick(), name().c_str(),
             MemCmd(state->cmd).toString().c_str(), (unsigned long)addr,
             (unsigned long)size, pendingCount,
             (unsigned long)state->numBytes, (unsigned long)state->totBytes,
             state->completionEvent ? state->completionEvent->scheduled() : 0);
    }

    // Update the number of bytes received based on the request rather
    // than the packet as the latter could be rounded up to line sizes.
    state->numBytes += size;
    assert(state->totBytes >= state->numBytes);

    bool all_bytes = (state->totBytes == state->numBytes);
    if (state->aborted) {
        // If this request was aborted, check to see if its in flight accesses
        // have finished. There may be packets for more than one request in
        // flight at a time, so check for finished requests, or no more
        // packets.
        if (all_bytes || pendingCount == 0) {
            // If yes, signal its abort event (if any) and delete the state.
            if (state->abortEvent) {
                device->schedule(state->abortEvent, curTick());
            }
            delete state;
        }
    } else if (all_bytes) {
        // If we have reached the end of this DMA request, then signal the
        // completion and delete the sate.
        if (state->completionEvent) {
            delay += state->delay;
            device->schedule(state->completionEvent, curTick() + delay);
        }
        delete state;
    }

    // We might be drained at this point, if so signal the drain event.
    if (pendingCount == 0)
        signalDrainDone();
}

PacketPtr
DmaPort::DmaReqState::createPacket()
{
    RequestPtr req = std::make_shared<Request>(
            gen.addr(), gen.size(), flags, id);
    if (sid.has_value()) {
        req->setStreamId(sid.value());
    }
    if (ssid.has_value()) {
        req->setSubstreamId(ssid.value());
    }
    req->taskId(context_switch_task_id::DMA);

    PacketPtr pkt = new Packet(req, cmd);

    if (data)
        pkt->dataStatic(data + gen.complete());

    pkt->senderState = this;
    return pkt;
}

bool
DmaPort::recvTimingResp(PacketPtr pkt)
{
    const Addr resp_addr = pkt->getAddr();
    const unsigned resp_size = pkt->getSize();

    if (amtcTraceDmaAddr(resp_addr) || amtcTraceDmaPort(name())) {
        warn("AMTC_DMADEV_RECV_TIMING_RESP tick=%llu port=%s pkt=%p cmd=%s "
             "addr=%#lx size=%u is_resp=%d error=%d pending=%u "
             "trace_valid=%d trace_addr=%#lx trace_cmd=%s\n",
             (unsigned long long)curTick(), name().c_str(), pkt,
             pkt->cmdString().c_str(), (unsigned long)resp_addr, resp_size,
             pkt->isResponse(), pkt->isError(), pendingCount,
             amtcDmaTraceValid, (unsigned long)amtcDmaTraceAddr,
             amtcDmaTraceCmd.c_str());
    }

    // We shouldn't ever get a cacheable block in Modified state.
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));

    handleRespPacket(pkt);

    if (amtcDmaTraceValid &&
        (resp_addr == amtcDmaTraceAddr || pendingCount == 0)) {
        warn("AMTC_DMADEV_PENDING_CLEARED tick=%llu port=%s resp_addr=%#lx "
             "resp_size=%u trace_addr=%#lx trace_size=%u trace_cmd=%s "
             "pending=%u\n",
             (unsigned long long)curTick(), name().c_str(),
             (unsigned long)resp_addr, resp_size,
             (unsigned long)amtcDmaTraceAddr, amtcDmaTraceSize,
             amtcDmaTraceCmd.c_str(), pendingCount);

        amtcDmaTraceValid = false;
        if (amtcDmaWatchdogEvent.scheduled()) {
            device->deschedule(amtcDmaWatchdogEvent);
        }
    }

    return true;
}

DmaDevice::DmaDevice(const Params &p)
    : PioDevice(p), dmaPort(this, sys, p.sid, p.ssid)
{ }

void
DmaDevice::init()
{
    panic_if(!dmaPort.isConnected(),
             "DMA port of %s not connected to anything!", name());
    PioDevice::init();
}

DrainState
DmaPort::drain()
{
    if (pendingCount == 0) {
        return DrainState::Drained;
    } else {
        DPRINTF(Drain, "DmaPort not drained\n");
        return DrainState::Draining;
    }
}

void
DmaPort::recvReqRetry()
{
    const bool trace_retry = amtcTraceDmaPort(name()) ||
        (inRetry && amtcTraceDmaAddr(inRetry->getAddr())) ||
        (!transmitList.empty() && amtcTraceDmaAddr(transmitList.front()->gen.addr()));

    if (trace_retry) {
        warn("AMTC_DMADEV_RETRY tick=%llu port=%s retryPending_before=%d "
             "inRetry=%p inRetry_addr=%#lx inRetry_size=%u inRetry_cmd=%s "
             "front_addr=%#lx front_size=%llu transmitList=%llu pending=%u sendEvent=%d\n",
             (unsigned long long)curTick(), name().c_str(), retryPending,
             inRetry,
             inRetry ? (unsigned long)inRetry->getAddr() : 0UL,
             inRetry ? inRetry->getSize() : 0,
             inRetry ? inRetry->cmdString().c_str() : "none",
             !transmitList.empty() ? (unsigned long)transmitList.front()->gen.addr() : 0UL,
             !transmitList.empty() ? (unsigned long long)transmitList.front()->gen.size() : 0ULL,
             (unsigned long long)transmitList.size(), pendingCount, sendEvent.scheduled());
    }

    retryPending = false;
    if (transmitList.size())
        trySendTimingReq();
}

void
DmaPort::dmaAction(Packet::Command cmd, Addr addr, int size, Event *event,
                   uint8_t *data, std::optional<uint32_t> sid,
                   std::optional<uint32_t> ssid, Tick delay,
                   Request::Flags flag)
{
    DPRINTF(DMA, "Starting DMA for addr: %#x size: %d sched: %d\n", addr, size,
            event ? event->scheduled() : -1);

    if (amtcTraceDmaAddr(addr) || amtcTraceDmaPort(name()) || retryPending || inRetry) {
        warn("AMTC_DMADEV_START tick=%llu port=%s cmd=%s addr=%#lx size=%d "
             "event=%p event_scheduled=%d delay=%llu flags=%#lx "
             "retryPending=%d inRetry=%p inRetry_addr=%#lx inRetry_size=%u "
             "inRetry_cmd=%s front_addr=%#lx transmitList_before=%llu pending=%u "
             "sendEvent=%d\n",
             (unsigned long long)curTick(), name().c_str(),
             MemCmd(cmd).toString().c_str(), (unsigned long)addr, size,
             event, event ? event->scheduled() : -1,
             (unsigned long long)delay, (unsigned long)flag,
             retryPending, inRetry,
             inRetry ? (unsigned long)inRetry->getAddr() : 0UL,
             inRetry ? inRetry->getSize() : 0,
             inRetry ? inRetry->cmdString().c_str() : "none",
             !transmitList.empty() ? (unsigned long)transmitList.front()->gen.addr() : 0UL,
             (unsigned long long)transmitList.size(), pendingCount,
             sendEvent.scheduled());
    }

    // One DMA request sender state for every action, that is then
    // split into many requests and packets based on the block size,
    // i.e. cache line size.
    transmitList.push_back(
            new DmaReqState(cmd, addr, cacheLineSize, size,
                data, flag, requestorId, sid, ssid, event, delay));

    if (amtcTraceDmaAddr(addr) || amtcTraceDmaPort(name()) || retryPending || inRetry) {
        warn("AMTC_DMADEV_ENQUEUED tick=%llu port=%s addr=%#lx "
             "transmitList_after=%llu pending=%u retryPending=%d "
             "inRetry=%p inRetry_addr=%#lx inRetry_size=%u inRetry_cmd=%s "
             "front_addr=%#lx front_size=%llu\n",
             (unsigned long long)curTick(), name().c_str(),
             (unsigned long)addr, (unsigned long long)transmitList.size(),
             pendingCount, retryPending, inRetry,
             inRetry ? (unsigned long)inRetry->getAddr() : 0UL,
             inRetry ? inRetry->getSize() : 0,
             inRetry ? inRetry->cmdString().c_str() : "none",
             !transmitList.empty() ? (unsigned long)transmitList.front()->gen.addr() : 0UL,
             !transmitList.empty() ? (unsigned long long)transmitList.front()->gen.size() : 0ULL);
    }

    // In zero time, also initiate the sending of the packets for the request
    // we have just created. For atomic this involves actually completing all
    // the requests.
    sendDma();
}

void
DmaPort::dmaAction(Packet::Command cmd, Addr addr, int size, Event *event,
                   uint8_t *data, Tick delay, Request::Flags flag)
{
    dmaAction(cmd, addr, size, event, data,
              defaultSid, defaultSSid, delay, flag);
}

void
DmaPort::abortPending()
{
    if (inRetry) {
        delete inRetry;
        inRetry = nullptr;
    }

    if (pendingCount && !transmitList.empty()) {
        auto *state = transmitList.front();
        if (state->numBytes != state->gen.complete()) {
            // In flight packets refer to the transmission at the front of the
            // list, and not a transmission whose packets have all been sent
            // but not completed. Preserve the state so the packets don't have
            // dangling pointers.
            transmitList.pop_front();
            state->aborted = true;
        }
    }

    // Get rid of requests that haven't started yet.
    while (!transmitList.empty()) {
        auto *state = transmitList.front();
        if (state->abortEvent)
            device->schedule(state->abortEvent, curTick());
        delete state;
        transmitList.pop_front();
    }

    if (sendEvent.scheduled())
        device->deschedule(sendEvent);

    if (pendingCount == 0)
        signalDrainDone();

    amtcDmaTraceValid = false;
    if (amtcDmaWatchdogEvent.scheduled()) {
        device->deschedule(amtcDmaWatchdogEvent);
    }
}

void
DmaPort::trySendTimingReq()
{
    // Send the next packet for the first DMA request on the transmit list,
    // and schedule the following send if it is successful
    DmaReqState *state = transmitList.front();

    const bool from_retry = inRetry != nullptr;
    PacketPtr pkt = from_retry ? inRetry : state->createPacket();
    inRetry = nullptr;

    DPRINTF(DMA, "Trying to send %s addr %#x\n", pkt->cmdString(),
            pkt->getAddr());

    const bool trace_pkt = amtcTraceDmaAddr(pkt->getAddr()) ||
        amtcTraceDmaPort(name()) || from_retry || retryPending;
    const Addr trace_addr = pkt->getAddr();
    const unsigned trace_size = pkt->getSize();
    const std::string trace_cmd = pkt->cmdString();
    PacketPtr trace_pkt_ptr = pkt;

    if (trace_pkt) {
        warn("AMTC_DMADEV_SEND_BEFORE tick=%llu port=%s pkt=%p cmd=%s "
             "addr=%#lx size=%u fromRetry=%d gen_complete=%lu gen_size=%lu "
             "pending=%u retryPending=%d transmitList=%llu sendEvent=%d\n",
             (unsigned long long)curTick(), name().c_str(), trace_pkt_ptr,
             trace_cmd.c_str(), (unsigned long)trace_addr, trace_size,
             from_retry, (unsigned long)state->gen.complete(),
             (unsigned long)state->gen.size(), pendingCount, retryPending,
             (unsigned long long)transmitList.size(), sendEvent.scheduled());
    }

    // Check if this was the last packet now, since hypothetically the packet
    // response may come immediately, and state may be deleted.
    bool last = state->gen.last();
    const bool accepted = sendTimingReq(pkt);
    if (accepted) {
        pendingCount++;

        if (trace_pkt) {
            amtcDmaTraceValid = true;
            amtcDmaTraceAddr = trace_addr;
            amtcDmaTraceSize = trace_size;
            amtcDmaTraceCmd = trace_cmd;
            amtcDmaTraceIssueTick = curTick();

            if (!amtcDmaWatchdogEvent.scheduled()) {
                device->schedule(amtcDmaWatchdogEvent, curTick() + 5000000);
            }
        }
    } else {
        retryPending = true;
        inRetry = pkt;
    }

    if (trace_pkt) {
        warn("AMTC_DMADEV_SEND_AFTER tick=%llu port=%s pkt=%p addr=%#lx "
             "ok=%d last=%d pending=%u retryPending=%d inRetry=%p "
             "trace_valid=%d trace_addr=%#lx trace_cmd=%s "
             "transmitList=%llu sendEvent=%d\n",
             (unsigned long long)curTick(), name().c_str(), trace_pkt_ptr,
             (unsigned long)trace_addr, accepted, last, pendingCount,
             retryPending, inRetry, amtcDmaTraceValid,
             (unsigned long)amtcDmaTraceAddr, amtcDmaTraceCmd.c_str(),
             (unsigned long long)transmitList.size(), sendEvent.scheduled());
    }
    if (!retryPending) {
        state->gen.next();
        // If that was the last packet from this request, pop it from the list.
        if (last)
            transmitList.pop_front();
        DPRINTF(DMA, "-- Done\n");
        // If there is more to do, then do so.
        if (!transmitList.empty()) {
            // This should ultimately wait for as many cycles as the device
            // needs to send the packet, but currently the port does not have
            // any known width so simply wait a single cycle.
            device->schedule(sendEvent, device->clockEdge(Cycles(1)));
        }
    } else {
        DPRINTF(DMA, "-- Failed, waiting for retry\n");
    }

    DPRINTF(DMA, "TransmitList: %d, retryPending: %d\n",
            transmitList.size(), retryPending ? 1 : 0);
}

bool
DmaPort::sendAtomicReq(DmaReqState *state)
{
    PacketPtr pkt = state->createPacket();
    DPRINTF(DMA, "Sending  DMA for addr: %#x size: %d\n",
            state->gen.addr(), state->gen.size());
    pendingCount++;
    Tick lat = sendAtomic(pkt);

    // Check if we're done, since handleResp may delete state.
    bool done = !state->gen.next();
    handleRespPacket(pkt, lat);
    return done;
}

bool
DmaPort::sendAtomicBdReq(DmaReqState *state)
{
    bool done = false;
    pendingCount++;

    auto bd_it = memBackdoors.contains(state->gen.addr());
    if (bd_it == memBackdoors.end()) {
        // We don't have a backdoor for this address, so use a packet.

        PacketPtr pkt = state->createPacket();
        DPRINTF(DMA, "Sending DMA for addr: %#x size: %d\n",
                state->gen.addr(), state->gen.size());

        MemBackdoorPtr bd = nullptr;
        Tick lat = sendAtomicBackdoor(pkt, bd);

        // If we got a backdoor, record it.
        if (bd && memBackdoors.insert(bd->range(), bd) != memBackdoors.end()) {
            // Invalidation callback which finds this backdoor and removes it.
            auto callback = [this](const MemBackdoor &backdoor) {
                for (auto it = memBackdoors.begin();
                        it != memBackdoors.end(); it++) {
                    if (it->second == &backdoor) {
                        memBackdoors.erase(it);
                        return;
                    }
                }
                panic("Got invalidation for unknown memory backdoor.");
            };
            bd->addInvalidationCallback(callback);
        }

        // Check if we're done now, since handleResp may delete state.
        done = !state->gen.next();
        handleRespPacket(pkt, lat);
    } else {
        // We have a backdoor that can at least partially satisfy this request.
        DPRINTF(DMA, "Handling DMA for addr: %#x size %d through backdoor\n",
                state->gen.addr(), state->gen.size());

        const auto *bd = bd_it->second;
        // Offset of this access into the backdoor.
        const Addr offset = state->gen.addr() - bd->range().start();
        // How many bytes we still need.
        const Addr remaining = state->totBytes - state->gen.complete();
        // How many bytes this backdoor can provide, starting from offset.
        const Addr available = bd->range().size() - offset;

        // How many bytes we're going to handle through this backdoor.
        const Addr handled = std::min(remaining, available);

        // If there's a buffer for data, read/write it.
        if (state->data) {
            uint8_t *bd_data = bd->ptr() + offset;
            uint8_t *state_data = state->data + state->gen.complete();
            if (MemCmd(state->cmd).isRead())
                memcpy(state_data, bd_data, handled);
            else
                memcpy(bd_data, state_data, handled);
        }

        // Advance the chunk generator past this region of memory.
        state->gen.setNext(state->gen.addr() + handled);

        // Check if we're done now, since handleResp may delete state.
        done = !state->gen.next();
        handleResp(state, state->gen.addr(), handled);
    }

    return done;
}

void
DmaPort::sendDma()
{
    // Some kind of selection between access methods. More work is going to
    // have to be done to make switching actually work.
    assert(transmitList.size());

    const bool trace_front = amtcTraceDmaPort(name()) || retryPending || inRetry ||
        (!transmitList.empty() && amtcTraceDmaAddr(transmitList.front()->gen.addr()));

    if (trace_front) {
        warn("AMTC_DMADEV_SENDDMA_ENTER tick=%llu port=%s front_addr=%#lx "
             "front_size=%llu timing=%d atomic=%d retryPending=%d "
             "inRetry=%p inRetry_addr=%#lx inRetry_size=%u inRetry_cmd=%s "
             "sendEvent=%d transmitList=%llu pending=%u\n",
             (unsigned long long)curTick(), name().c_str(),
             !transmitList.empty() ? (unsigned long)transmitList.front()->gen.addr() : 0UL,
             !transmitList.empty() ? (unsigned long long)transmitList.front()->gen.size() : 0ULL,
             sys->isTimingMode(), sys->isAtomicMode(), retryPending, inRetry,
             inRetry ? (unsigned long)inRetry->getAddr() : 0UL,
             inRetry ? inRetry->getSize() : 0,
             inRetry ? inRetry->cmdString().c_str() : "none",
             sendEvent.scheduled(), (unsigned long long)transmitList.size(), pendingCount);
    }

    if (sys->isTimingMode()) {
        // If we are either waiting for a retry or are still waiting after
        // sending the last packet, then do not proceed.
        if (retryPending || sendEvent.scheduled()) {
            DPRINTF(DMA, "Can't send immediately, waiting to send\n");
            if (trace_front) {
                warn("AMTC_DMADEV_SENDDMA_BLOCKED tick=%llu port=%s "
                     "front_addr=%#lx front_size=%llu retryPending=%d "
                     "inRetry=%p inRetry_addr=%#lx inRetry_size=%u inRetry_cmd=%s "
                     "sendEvent=%d transmitList=%llu pending=%u\n",
                     (unsigned long long)curTick(), name().c_str(),
                     !transmitList.empty() ? (unsigned long)transmitList.front()->gen.addr() : 0UL,
                     !transmitList.empty() ? (unsigned long long)transmitList.front()->gen.size() : 0ULL,
                     retryPending, inRetry,
                     inRetry ? (unsigned long)inRetry->getAddr() : 0UL,
                     inRetry ? inRetry->getSize() : 0,
                     inRetry ? inRetry->cmdString().c_str() : "none",
                     sendEvent.scheduled(), (unsigned long long)transmitList.size(), pendingCount);
            }
            return;
        }

        trySendTimingReq();
    } else if (sys->isAtomicMode()) {
        const bool bypass = sys->bypassCaches();

        // Send everything there is to send in zero time.
        while (!transmitList.empty()) {
            DmaReqState *state = transmitList.front();
            transmitList.pop_front();

            bool done = state->gen.done();
            while (!done)
                done = bypass ? sendAtomicBdReq(state) : sendAtomicReq(state);
        }
    } else {
        panic("Unknown memory mode.");
    }
}

Port &
DmaDevice::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dma") {
        return dmaPort;
    }
    return PioDevice::getPort(if_name, idx);
}

DmaReadFifo::DmaReadFifo(DmaPort &_port, size_t size,
                         unsigned max_req_size,
                         unsigned max_pending,
                         Request::Flags flags)
    : maxReqSize(max_req_size), fifoSize(size),
      reqFlags(flags), port(_port), cacheLineSize(port.sys->cacheLineSize()),
      buffer(size)
{
    freeRequests.resize(max_pending);
    for (auto &e : freeRequests)
        e.reset(new DmaDoneEvent(this, max_req_size));

}

DmaReadFifo::~DmaReadFifo()
{
    for (auto &p : pendingRequests) {
        DmaDoneEvent *e(p.release());

        if (e->done()) {
            delete e;
        } else {
            // We can't kill in-flight DMAs, so we'll just transfer
            // ownership to the event queue so that they get freed
            // when they are done.
            e->kill();
        }
    }
}

void
DmaReadFifo::serialize(CheckpointOut &cp) const
{
    assert(pendingRequests.empty());

    SERIALIZE_CONTAINER(buffer);
    SERIALIZE_SCALAR(endAddr);
    SERIALIZE_SCALAR(nextAddr);
}

void
DmaReadFifo::unserialize(CheckpointIn &cp)
{
    UNSERIALIZE_CONTAINER(buffer);
    UNSERIALIZE_SCALAR(endAddr);
    UNSERIALIZE_SCALAR(nextAddr);
}

bool
DmaReadFifo::tryGet(uint8_t *dst, size_t len)
{
    if (buffer.size() >= len) {
        buffer.read(dst, len);
        resumeFill();
        return true;
    } else {
        return false;
    }
}

void
DmaReadFifo::get(uint8_t *dst, size_t len)
{
    panic_if(!tryGet(dst, len), "Buffer underrun in DmaReadFifo::get()");
}

void
DmaReadFifo::startFill(Addr start, size_t size)
{
    assert(atEndOfBlock());

    nextAddr = start;
    endAddr = start + size;
    resumeFill();
}

void
DmaReadFifo::stopFill()
{
    // Prevent new DMA requests by setting the next address to the end
    // address. Pending requests will still complete.
    nextAddr = endAddr;

    // Flag in-flight accesses as canceled. This prevents their data
    // from being written to the FIFO.
    for (auto &p : pendingRequests)
        p->cancel();
}

void
DmaReadFifo::resumeFill()
{
    // Don't try to fetch more data if we are draining. This ensures
    // that the DMA engine settles down before we checkpoint it.
    if (drainState() == DrainState::Draining)
        return;

    const bool old_eob(atEndOfBlock());

    if (port.sys->bypassCaches())
        resumeFillBypass();
    else
        resumeFillTiming();

    if (!old_eob && atEndOfBlock())
        onEndOfBlock();
}

void
DmaReadFifo::resumeFillBypass()
{
    const size_t fifo_space = buffer.capacity() - buffer.size();
    if (fifo_space >= cacheLineSize || buffer.capacity() < cacheLineSize) {
        const size_t block_remaining = endAddr - nextAddr;
        const size_t xfer_size = std::min(fifo_space, block_remaining);
        std::vector<uint8_t> tmp_buffer(xfer_size);

        assert(pendingRequests.empty());
        DPRINTF(DMA, "Direct bypass startAddr=%#x xfer_size=%#x " \
                "fifo_space=%#x block_remaining=%#x\n",
                nextAddr, xfer_size, fifo_space, block_remaining);

        port.dmaAction(MemCmd::ReadReq, nextAddr, xfer_size, nullptr,
                tmp_buffer.data(), 0, reqFlags);

        buffer.write(tmp_buffer.begin(), xfer_size);
        nextAddr += xfer_size;
    }
}

void
DmaReadFifo::resumeFillTiming()
{
    size_t size_pending(0);
    for (auto &e : pendingRequests)
        size_pending += e->requestSize();

    while (!freeRequests.empty() && !atEndOfBlock()) {
        const size_t req_size(std::min(maxReqSize, endAddr - nextAddr));
        if (buffer.size() + size_pending + req_size > fifoSize)
            break;

        DmaDoneEventUPtr event(std::move(freeRequests.front()));
        freeRequests.pop_front();
        assert(event);

        event->reset(req_size);
        port.dmaAction(MemCmd::ReadReq, nextAddr, req_size, event.get(),
                       event->data(), 0, reqFlags);
        nextAddr += req_size;
        size_pending += req_size;

        pendingRequests.emplace_back(std::move(event));
    }
}

void
DmaReadFifo::dmaDone()
{
    const bool old_active(isActive());

    handlePending();
    resumeFill();

    if (old_active && !isActive())
        onIdle();
}

void
DmaReadFifo::handlePending()
{
    while (!pendingRequests.empty() && pendingRequests.front()->done()) {
        // Get the first finished pending request
        DmaDoneEventUPtr event(std::move(pendingRequests.front()));
        pendingRequests.pop_front();

        if (!event->canceled())
            buffer.write(event->data(), event->requestSize());

        // Move the event to the list of free requests
        freeRequests.emplace_back(std::move(event));
    }

    if (pendingRequests.empty())
        signalDrainDone();
}

DrainState
DmaReadFifo::drain()
{
    return pendingRequests.empty() ?
        DrainState::Drained : DrainState::Draining;
}


DmaReadFifo::DmaDoneEvent::DmaDoneEvent(DmaReadFifo *_parent, size_t max_size)
    : parent(_parent), _data(max_size, 0)
{
}

void
DmaReadFifo::DmaDoneEvent::kill()
{
    parent = nullptr;
    setFlags(AutoDelete);
}

void
DmaReadFifo::DmaDoneEvent::cancel()
{
    _canceled = true;
}

void
DmaReadFifo::DmaDoneEvent::reset(size_t size)
{
    assert(size <= _data.size());
    _done = false;
    _canceled = false;
    _requestSize = size;
}

void
DmaReadFifo::DmaDoneEvent::process()
{
    if (!parent)
        return;

    assert(!_done);
    _done = true;
    parent->dmaDone();
}

} // namespace gem5
