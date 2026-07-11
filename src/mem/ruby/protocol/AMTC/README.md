 Mapping (use this consistently)
Your intention	Use this
RdBlk Read (shared)	GETS
Read for write	GETX
Writeback	PUTX / PUTS
Data response	DATA_SHARED, DATA_OWNER
Control response	ACK


| Old style | AMTC / MOESI style |
| --------- | ------------------ |
| RdBlk     | GETS               |
| RdBlkM    | GETX               |
| WB        | PUTX               |

Final classification (VERY IMPORTANT)
Action	Message type
GETS / GETX	RequestMsg
WriteThrough	ResponseMsg
WriteThroughNoAlloc	ResponseMsg
Flush	ResponseMsg
Atomic (data-based)	ResponseMsg



Why this preserves semantics
Old (TriggerMsg)	New (Event)
Network queue	Local event
Delayed processing	Optional schedule()
Message-driven	Event-driven
Same condition check	Same condition check

| Legacy     | AMTC                     |
| ---------- | ------------------------ |
| NBSysWBAck | ACK                      |
| TDSysWBAck | ACK                      |
| NBSysResp  | DATA_SHARED / DATA_OWNER |



What you are doing (correct framing)
Phase 1: Minimal viable AMTC + CPU + basic GPU flow
Phase 2: Add back advanced GPU semantics (selectively)

So right now you are:

✔ simplifying protocol
✔ removing non-essential control paths
✔ validating core data/token flow
🧠 Keep a “deferred features” checklist

Create a section in your notes / thesis / README:

AMTC Integration – Deferred Features (Phase 2)
📌 What you removed (track this clearly)
1. Trigger Queue (TriggerMsg)
Purpose:
  Delayed self-events (retry / atomic completion)

Removed because:
  Replaced by direct trigger() / event-driven model

Re-add when:
  ✔ precise timing required
  ✔ atomic ordering complexity increases
2. UnblockMsg
Purpose:
  Explicit unblock of directory state

Removed because:
  AMTC uses implicit completion (tokens + ACK)

Re-add when:
  ✔ directory-level contention modeling needed
3. NBProbeRequestMsg (Probe path)
Purpose:
  Directory → cache invalidation / downgrade

Removed because:
  Early system uses:
    ✔ GETX for ownership
    ✔ writeback-driven coherence

Re-add when:
  ✔ CPU–GPU sharing conflicts appear
  ✔ multi-agent coherence stress
4. Coalescer callbacks
Removed:
  readCallback
  writeCallback
  invTCPCallback

Reason:
  VIPER GPU uses implicit completion model

Re-add when:
  ✔ deeper GPU pipeline modeling needed
5. State field (CoherenceState)
Replaced with:
  ✔ DATA_SHARED
  ✔ DATA_OWNER
  ✔ Tokens

Re-add when:
  ❌ likely NEVER needed (your design is cleaner)
6. writeMask (byte granularity)
Replaced with:
  ✔ sector_mask (AMTC)

Re-add when:
  ✔ sub-sector (byte-level) precision is required
🧠 Why this approach is powerful

You are doing:

Bottom-up protocol construction

Instead of:

Trying to make full VIPER work at once ❌
🔥 What you MUST ensure now

Even with simplification:

✔ No deadlock
✔ Requests eventually complete
✔ Tokens/data flow correctly
✔ CPU path still stable
🚀 Your next milestone

After compilation succeeds:

1. Run simple GPU workload
2. Observe:
   - Does it progress?
   - Any deadlock?
   - Missing responses?
🧠 When to reintroduce features

Only when you see:

❌ incorrect data sharing
❌ stale data
❌ GPU inconsistency
❌ starvation

Then:

→ reintroduce probe OR ordering logic
🎯 Suggested documentation style (for thesis)

You can literally write:

“To enable incremental validation of AMTC, we temporarily disabled
legacy control-driven mechanisms such as TriggerMsg, UnblockMsg,
and probe-based invalidations. These are reintroduced only when
required for correctness under heterogeneous CPU–GPU workloads.”

👉 This is strong research justification, not a hack.

👍 Final takeaway
You are not removing features randomly
You are staging complexity deliberately


How to decide what to keep

Use this rule:

If an event depends on:
  - WriteThrough / WriteFlush / Atomic* variants
  - Trigger/Unblock queues
  - Probe-specific flows

→ REMOVE or MERGE

If it maps cleanly to:
  - GETS / GETX
  - DATA arrival
  - ACK arrival
  - Replacement

→ KEEP (or rename)
📊 Your current events → AMTC mapping
🟢 Keep (with same or slightly cleaner names)
RdBlk            → keep (or rename to Req_GETS)
Data             → keep (response arrival)
Flush            → keep (triggered by WB data)
L2_Repl          → keep
InvCache         → optional keep (if you still use INV)
WBAck            → keep (ACK handling)
Bypass           → keep (GPU/SLC path if you need it)
🟡 Simplify / Merge
RdBypassEvict    → merge into RdBlk + flag (isSLCSet)
WrVicBlk         → REMOVE (write-through is now ResponseMsg)
WrVicBlkBack     → REMOVE
WrVicBlkEvict    → REMOVE

Atomic           → map to GETX (reuse same event)
AtomicPassOn     → REMOVE
AtomicWait       → REMOVE (unless you keep ordering)
AtomicDone       → REMOVE
AtomicNotDone    → REMOVE

👉 Replace all atomic variants with:

GETX → one event
🔴 Remove completely
PrbInv
PrbDowngrade
AtomicWait
AtomicDone
AtomicNotDone
WrVicBlk*
AtomicPassOn

These depend on:

❌ probes
❌ triggerQueue
❌ legacy atomic scheduling
❌ request-based write-through
🎯 Clean AMTC Event Set (recommended)

Here’s a ready-to-use minimal version:

enumeration(Event, desc="AMTC TCC Events") {

  // Requests (from TCP / L1)
  GETS,            desc="Read request";
  GETX,            desc="Write / atomic request";

  // Responses (from Directory / Memory / L2)
  Data,            desc="Data response (DATA_SHARED / DATA_OWNER)";
  Ack,             desc="Completion / ACK";

  // Internal cache behavior
  L2_Repl,         desc="Cache replacement";
  Flush,           desc="Writeback / flush handling";

  // Optional (keep only if used)
  Inv,             desc="Invalidate request";
  Bypass,          desc="Bypass L2 (GPU/SLC path)";
}
🧠 How this maps to your AMTC protocol
GETS  → load path
GETX  → store / atomic path
Data  → DATA_SHARED / DATA_OWNER
Ack   → completion / writeback ack
Flush → triggered by WB_SHARED_DATA
🔥 Important insight (this is thesis-quality)

You just reduced:

VIPER:
  ~15+ events

to:

AMTC:
  ~5–7 events

That’s:

✔ lower control complexity
✔ easier verification
✔ cleaner state machine
✔ matches “data-driven coherence”



removed in dir.sm




  in_port(responseNetwork_in, ResponseMsg, responseToDir) {
    if (responseNetwork_in.isReady(clockEdge())) {
      peek(responseNetwork_in, ResponseMsg) {
        assert(in_msg.Destination.isElement(machineID));
        Entry dir_entry := getDirectoryEntry(in_msg.addr);

        // Phase 2 Token Aggregation
        int future_t0 := dir_entry.TokensSec0 + in_msg.TokensSec0;
        int future_t1 := dir_entry.TokensSec1 + in_msg.TokensSec1;
        int required := max_tokens() / 2;

        // Has the directory reclaimed all tokens for both sectors?
        bool is_effectively_all_tokens := (future_t0 == required) && (future_t1 == required);

        if (is_effectively_all_tokens) {
           if (in_msg.Type == CoherenceResponseType:ACK_OWNER) {
            trigger(Event:Ack_Owner_All_Tokens, in_msg.addr, TBEs[in_msg.addr]);
          } else if (in_msg.Type == CoherenceResponseType:ACK) {
            trigger(Event:Ack_All_Tokens, in_msg.addr, TBEs[in_msg.addr]);
          } else {
            error("Invalid message");
          }
        } else {
          if (in_msg.Type == CoherenceResponseType:DATA_OWNER) {
            trigger(Event:Data_Owner, in_msg.addr, TBEs[in_msg.addr]);
          } else if ((in_msg.Type == CoherenceResponseType:ACK) ||
                     (in_msg.Type == CoherenceResponseType:DATA_SHARED)) {
            trigger(Event:Tokens, in_msg.addr, TBEs[in_msg.addr]);
          } else if (in_msg.Type == CoherenceResponseType:ACK_OWNER) {
            trigger(Event:Ack_Owner, in_msg.addr, TBEs[in_msg.addr]);
          } else {
            error("Invalid message");
          }
        }
      }
    }
  }


/// to do list for L2cache

Phase 2: Runtime Token Tracing
To catch orphan and negative tokens before they cause a silent failure, inject a tracer action into your state transitions.

1. Define the Tracer Action

C++
action(zz_traceTokenState, "zz", desc="Trace token anomalies") {
    if (is_valid(cache_entry)) {
        DPRINTF(RubySlicc, "ADDR=%#x S0=%d S1=%d state=%s\n",
                address,
                cache_entry.TokensSec0,
                cache_entry.TokensSec1,
                getState(tbe, cache_entry, address));

        // Catch orphans and underflows immediately
        if (cache_entry.TokensSec0 < 0 || cache_entry.TokensSec1 < 0) {
            panic("Token underflow detected at ADDR=%#x", address);
        }
        if ((cache_entry.TokensSec0 > 0 || cache_entry.TokensSec1 > 0) &&
             getState(tbe, cache_entry, address) == State:I) {
            panic("Orphan tokens detected in Invalid state at ADDR=%#x", address);
        }
    }
}
Call zz_traceTokenState at the end of every transition block that modifies tokens.

Phase 3: State Invariants Checker
SLICC allows you to define a setState function (or modify the default one). This is the absolute best place to enforce invariants, as it runs right before the cache block officially shifts states.

Add/Modify your state invariant logic:

C++
void amtc_checkInvariants(Entry cache_entry, State state, Addr address) {
    if (is_valid(cache_entry)) {
        // Invariant A: M state must hold all tokens
        if (state == State:M) {
            assert((cache_entry.TokensSec0 + cache_entry.TokensSec1) == max_tokens());
        }

        // Invariant B: I state must hold zero tokens
        if (state == State:I) {
            assert(cache_entry.TokensSec0 == 0);
            assert(cache_entry.TokensSec1 == 0);
        }

        // Invariant C: Valid bit checks (Assuming ValidSectors is a bitmask)
        if (state == State:M || state == State:O || state == State:S) {
            assert(cache_entry.ValidSectors > 0);
        }
    }
}
Phase 4: Infinite Retry Detection
SLICC state machines can easily get stuck in livelock during complex token scattering. To track this, modify your Entry structure and build a tripwire.

1. Modify the Entry Structure in SLICC:

C++
structure(Entry, desc="...", interface="AbstractCacheEntry") {
    // ... your existing fields ...
    int TokensSec0, default="0";
    int TokensSec1, default="0";

    // Retry tracking
    int retry_count, default="0";
    int reissue_count, default="0";
    int bounce_count, default="0";
}
2. The Hard Abort Action:

C++
action(abort_on_retry_limit, "ab", desc="Tripwire for infinite retry loops") {
    if (is_valid(cache_entry)) {
        if (cache_entry.retry_count > 10 ||
            cache_entry.reissue_count > 10 ||
            cache_entry.bounce_count > 10) {

            DPRINTF(RubySlicc, "LIVELOCK ABORT: ADDR=%#x retries=%d reissues=%d bounces=%d\n",
                    address, cache_entry.retry_count, cache_entry.reissue_count, cache_entry.bounce_count);

            panic("Infinite retry loop detected. Token state likely fragmented.");
        }
    }
}
Increment these counters in your respective bounce, retry, and timeout actions, followed immediately by a call to abort_on_retry_limit.

The Reality of the Global Token Sum Validator
You requested a "Global token sum validator (run every 1000 cycles)".

Candid Note: You cannot do this cleanly inside a SLICC .sm file. SLICC files compile into individual, isolated controller objects (L1, L2, Dir). An L1 controller does not have memory access to peek at the TokensSec0 of another L1 or L2 controller mid-simulation.

To achieve a true global token sum, you must write a C++ function inside the RubySystem class (in src/mem/ruby/system/RubySystem.cc) that iterates through the m_abstract_controls array.

C++ Global Validator Skeleton (RubySystem.cc):

C++
void RubySystem::globalTokenAudit() {
    int total_s0 = 0;
    int total_s1 = 0;

    for (int i = 0; i < m_abstract_controls.size(); i++) {
        // You will need to cast the abstract controller to your specific
        // generated controller types to access their cache arrays and TBEs.
        // Example logic:
        // total_s0 += m_abstract_controls[i]->countTokensS0();
    }

    if (total_s0 != (max_tokens() / 2) || total_s1 != (max_tokens() / 2)) {
        panic("GLOBAL TOKEN LEAK: S0=%d S1=%d", total_s0, total_s1);
    }
}
You would then schedule a gem5 Event in C++ to fire globalTokenAudit() every 1000 ticks. Building this C++ bridge is tedious, but it is the only way to mathematically prove 100% token conservation across the entire distributed fabric.

//L2 to do end



new:

Final architecture summary

Your best model is now:

Access                Type	Behavior
CPU read	            local/shared
GPU read	            local/shared
CPU read + GPU read	  shared-owner
CPU write	            migrate to CPU exclusive
GPU write	            migrate to GPU exclusive
idle GPU	            invisible to CPU fast path


  // ============================================================================
  // 2. UPGRADES & MIGRATIONS FROM CPU SHARED (CS)
  // ============================================================================
  transition(CS, CPU_GETS) {
    // Fast path: Another CPU core wants a copy. Forward locally.
    a_fwd_gets_locality_aware; u_set_cpu_shared; j_popIncomingRequestQueue;
  }
  transition(CS, GPU_GETS, CGS) {
    // Cross-Domain: GPU wants to share data the CPU holds
    a_sendTokens; qf_queueMemoryFetchRequest; u_set_gpu_shared; j_popIncomingRequestQueue;
  }
  transition(CS, CPU_GETX, CE) {
    // UPGRADE: CPU wants to write. No invalidation needed because GPU doesn't have it!
    a_sendTokens; qf_queueMemoryFetchRequest; u_set_cpu_exclusive; j_popIncomingRequestQueue;
  }
  transition(CS, GPU_GETX, GE) {
    // MIGRATION: GPU wants to write. Must invalidate CPU!
    a_inv_remote_domain_on_getx; a_sendTokens; qf_queueMemoryFetchRequest; u_set_gpu_exclusive; j_popIncomingRequestQueue;
  }
