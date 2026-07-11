# Copyright (c) 2015 Advanced Micro Devices, Inc.
# Copyright (c) 2026 Narayana Murty N.
# All rights reserved.
#
# configs/ruby/APU_AMTC.py
# Heterogeneous Sector Token Coherence (AMTC) Ruby Topology Script

import math

from common import (
    FileSystemConfig,
    MemConfig,
    ObjectList,
)

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath

from .Ruby import (
    create_topology,
    send_evicts,
)

addToPath("../")

from topologies.Cluster import Cluster
from topologies.Crossbar import Crossbar

from m5.objects import (
    RubyCache,
    RubySequencer,
    VIPERCoalescer,
)


class CntrlBase:
    _seqs = 0

    @classmethod
    def seqCount(cls):
        CntrlBase._seqs += 1
        return CntrlBase._seqs - 1

    _cntrls = 0

    @classmethod
    def cntrlCount(cls):
        CntrlBase._cntrls += 1
        return CntrlBase._cntrls - 1

    _version = 0

    @classmethod
    def versionCount(cls):
        cls._version += 1
        return cls._version - 1


class L1Cache(RubyCache):
    resourceStalls = False
    dataArrayBanks = 2
    tagArrayBanks = 2
    dataAccessLatency = 1
    tagAccessLatency = 1

    def create(self, size, assoc, options):
        self.size = MemorySize(size)
        self.assoc = assoc
        self.replacement_policy = TreePLRURP()


class L2Cache(RubyCache):
    resourceStalls = False
    assoc = 16
    dataArrayBanks = 16
    tagArrayBanks = 16

    def create(self, size, assoc, options):
        self.size = MemorySize(size)
        self.assoc = assoc
        self.replacement_policy = TreePLRURP()


class L1Cntrl(APU_AMTC_L1Cache_Controller, CntrlBase):
    def create(self, options, ruby_system, system):
        self.version = self.versionCount()

        self.L1Icache = L1Cache()
        self.L1Icache.create(options.l1i_size, options.l1i_assoc, options)
        self.L1Dcache = L1Cache()
        self.L1Dcache.create(options.l1d_size, options.l1d_assoc, options)

        self.sequencer = RubySequencer(ruby_system=ruby_system)
        self.sequencer.version = self.seqCount()
        self.sequencer.dcache = self.L1Dcache
        self.sequencer.ruby_system = ruby_system
        self.sequencer.is_cpu_sequencer = True

        self.send_evictions = True
        self.N_tokens = options.num_cpus + 1

        # AMTC Specific Parameters
        self.retry_threshold = options.l1_retries
        self.fixed_timeout_latency = options.timeout_latency
        self.dynamic_timeout_enabled = not options.disable_dyn_timeouts
        self.no_mig_atomic = not options.allow_atomic_migration

        self.ruby_system = ruby_system
        self.l2_select_num_bits = int(math.log(options.num_l2caches, 2))

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency


class L2Cntrl(APU_AMTC_L2Cache_Controller, CntrlBase):
    def create(self, options, ruby_system, system):
        self.version = self.versionCount()
        self.L2cache = L2Cache()
        self.L2cache.create(options.l2_size, options.l2_assoc, options)

        self.N_tokens = options.num_cpus + 1
        self.transitions_per_cycle = 32
        self.clk_domain = ruby_system.clk_domain
        self.ruby_system = ruby_system

        # --- FIX THE ADDRESS FILTERING ---
        l2_bits = int(math.log(options.num_l2caches, 2))
        block_size_bits = int(math.log(options.cacheline_size, 2))

        self.L2cache = RubyCache(
            size=options.l2_size,
            assoc=options.l2_assoc,
            start_index_bit=(block_size_bits + l2_bits),
        )

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency


class TCPCache(RubyCache):
    size = "16KiB"
    assoc = 16
    dataArrayBanks = 16
    tagArrayBanks = 16
    dataAccessLatency = 4
    tagAccessLatency = 1

    def create(self, options):
        self.size = MemorySize(options.tcp_size)
        self.assoc = options.tcp_assoc
        self.resourceStalls = options.no_tcc_resource_stalls
        if hasattr(options, "tcp_rp"):
            self.replacement_policy = ObjectList.rp_list.get(options.tcp_rp)()


class TCPCntrl(APU_AMTC_TCP_Controller, CntrlBase):
    def create(self, options, ruby_system, system):
        self.version = self.versionCount()

        self.L1cache = TCPCache(
            tagAccessLatency=options.TCP_latency,
            dataAccessLatency=options.TCP_latency,
        )
        self.L1cache.resourceStalls = options.no_resource_stalls
        self.L1cache.dataArrayBanks = options.tcp_num_banks
        self.L1cache.tagArrayBanks = options.tcp_num_banks
        self.L1cache.create(options)
        self.issue_latency = 1
        self.mandatory_queue_latency = options.mandatory_queue_latency

        self.coalescer = VIPERCoalescer(ruby_system=ruby_system)
        self.coalescer.version = self.seqCount()
        self.coalescer.icache = self.L1cache
        self.coalescer.dcache = self.L1cache
        self.coalescer.ruby_system = ruby_system
        self.coalescer.support_inst_reqs = False
        self.coalescer.is_cpu_sequencer = False
        if options.tcp_deadlock_threshold:
            self.coalescer.deadlock_threshold = options.tcp_deadlock_threshold
        self.coalescer.max_coalesces_per_cycle = (
            options.max_coalesces_per_cycle
        )

        self.sequencer = RubySequencer(ruby_system=ruby_system)
        self.sequencer.version = self.seqCount()
        self.sequencer.dcache = self.L1cache
        self.sequencer.ruby_system = ruby_system
        self.sequencer.is_cpu_sequencer = True

        self.use_seq_not_coal = False
        self.ruby_system = ruby_system
        if hasattr(options, "gpu_clock") and hasattr(options, "gpu_voltage"):
            self.clk_domain = SrcClockDomain(
                clock=options.gpu_clock,
                voltage_domain=VoltageDomain(voltage=options.gpu_voltage),
            )

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency

    def createCP(self, options, ruby_system, system):
        self.version = self.versionCount()

        self.L1cache = TCPCache(
            tagAccessLatency=options.TCP_latency,
            dataAccessLatency=options.TCP_latency,
        )
        self.L1cache.resourceStalls = options.no_resource_stalls
        self.L1cache.create(options)
        self.issue_latency = 1

        self.coalescer = VIPERCoalescer(ruby_system=ruby_system)
        self.coalescer.version = self.seqCount()
        self.coalescer.icache = self.L1cache
        self.coalescer.dcache = self.L1cache
        self.coalescer.ruby_system = ruby_system
        self.coalescer.support_inst_reqs = False
        self.coalescer.is_cpu_sequencer = False

        self.sequencer = RubySequencer(ruby_system=ruby_system)
        self.sequencer.version = self.seqCount()
        self.sequencer.dcache = self.L1cache
        self.sequencer.ruby_system = ruby_system
        self.sequencer.is_cpu_sequencer = True

        self.use_seq_not_coal = True
        self.ruby_system = ruby_system

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency


class SQCCache(RubyCache):
    dataArrayBanks = 8
    tagArrayBanks = 8
    dataAccessLatency = 1
    tagAccessLatency = 1

    def create(self, options):
        self.size = MemorySize(options.sqc_size)
        self.assoc = options.sqc_assoc
        if hasattr(options, "sqc_rp"):
            self.replacement_policy = ObjectList.rp_list.get(options.sqc_rp)()


class SQCCntrl(APU_AMTC_SQC_Controller, CntrlBase):
    def create(self, options, ruby_system, system):
        self.version = self.versionCount()

        self.L1cache = SQCCache()
        self.L1cache.create(options)
        self.L1cache.resourceStalls = options.no_resource_stalls

        self.sequencer = VIPERSequencer()

        self.sequencer.version = self.seqCount()
        self.sequencer.dcache = self.L1cache
        self.sequencer.ruby_system = ruby_system
        self.sequencer.support_data_reqs = False
        self.sequencer.is_cpu_sequencer = False
        if options.sqc_deadlock_threshold:
            self.sequencer.deadlock_threshold = options.sqc_deadlock_threshold

        self.ruby_system = ruby_system
        if hasattr(options, "gpu_clock") and hasattr(options, "gpu_voltage"):
            self.clk_domain = SrcClockDomain(
                clock=options.gpu_clock,
                voltage_domain=VoltageDomain(voltage=options.gpu_voltage),
            )

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency


class TCC(RubyCache):
    size = MemorySize("256KiB")
    assoc = 16
    dataAccessLatency = 8
    tagAccessLatency = 2
    resourceStalls = True

    def create(self, options):
        self.assoc = options.tcc_assoc
        self.atomicLatency = options.atomic_alu_latency
        self.atomicALUs = options.tcc_num_atomic_alus
        if hasattr(options, "bw_scalor") and options.bw_scalor > 0:
            s = options.num_compute_units
            tcc_size = s * 128
            tcc_size = str(tcc_size) + "KiB"
            self.size = MemorySize(tcc_size)
            self.dataArrayBanks = 64
            self.tagArrayBanks = 64
        else:
            self.size = MemorySize(options.tcc_size)
            self.dataArrayBanks = 256 / options.num_tccs
            self.tagArrayBanks = 256 / options.num_tccs
        self.size.value = self.size.value / options.num_tccs
        if (self.size.value / int(self.assoc)) < 128:
            self.size.value = int(128 * self.assoc)
        self.start_index_bit = math.log(options.cacheline_size, 2) + math.log(
            options.num_tccs, 2
        )
        if hasattr(options, "tcc_rp"):
            self.replacement_policy = ObjectList.rp_list.get(options.tcc_rp)()


class TCCCntrl(APU_AMTC_TCC_Controller, CntrlBase):
    def create(self, options, ruby_system, system):
        self.version = self.versionCount()
        self.L2cache = TCC(
            tagAccessLatency=options.tcc_tag_access_latency,
            dataAccessLatency=options.tcc_data_access_latency,
        )
        self.L2cache.create(options)
        self.L2cache.resourceStalls = options.no_tcc_resource_stalls

        self.ruby_system = ruby_system
        if hasattr(options, "gpu_clock") and hasattr(options, "gpu_voltage"):
            self.clk_domain = SrcClockDomain(
                clock=options.gpu_clock,
                voltage_domain=VoltageDomain(voltage=options.gpu_voltage),
            )

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency


class DirCntrl(APU_AMTC_Directory_Controller, CntrlBase):
    def create(self, options, dir_ranges, ruby_system, system):
        self.version = self.versionCount()

        self.addr_ranges = dir_ranges
        self.directory = RubyDirectoryMemory(
            block_size=ruby_system.block_size_bytes
        )
        self.number_of_TBEs = options.num_tbes

        self.transitions_per_cycle = 32
        self.ruby_system = ruby_system
        self.l2_select_num_bits = int(math.log(options.num_l2caches, 2))

        if options.recycle_latency:
            self.recycle_latency = options.recycle_latency

    def connectWireBuffers(
        self,
        req_to_dir,
        resp_to_dir,
        l3_unblock_to_dir,
        req_to_l3,
        probe_to_l3,
        resp_to_l3,
    ):
        self.reqToDir = req_to_dir
        self.respToDir = resp_to_dir
        self.l3UnblockToDir = l3_unblock_to_dir
        self.reqToL3 = req_to_l3
        self.probeToL3 = probe_to_l3
        self.respToL3 = resp_to_l3


def define_options(parser):
    # Base AMTC Options
    parser.add_argument(
        "--l1-retries",
        type=int,
        default=1,
        help="Token_CMP: # of l1 retries before going persistent",
    )
    parser.add_argument(
        "--timeout-latency",
        type=int,
        default=300,
        help="Token_CMP: cycles until issuing again",
    )
    parser.add_argument(
        "--disable-dyn-timeouts",
        action="store_true",
        help="Token_CMP: disable dyanimc timeouts",
    )
    parser.add_argument(
        "--allow-atomic-migration",
        action="store_true",
        help="allow migratory sharing",
    )

    # VIPER GPU Options
    parser.add_argument("--num-subcaches", type=int, default=4)
    #parser.add_argument("--l3-data-latency", type=int, default=20)
    #parser.add_argument("--l3-tag-latency", type=int, default=15)
    parser.add_argument("--cpu-to-dir-latency", type=int, default=20)
    parser.add_argument("--gpu-to-dir-latency", type=int, default=20)
    parser.add_argument(
        "--no-resource-stalls", action="store_false", default=True
    )
    parser.add_argument(
        "--no-tcc-resource-stalls", action="store_false", default=True
    )
    parser.add_argument("--use-L3-on-WT", action="store_true", default=False)
    parser.add_argument("--num-tbes", type=int, default=256)
    parser.add_argument("--l2-latency", type=int, default=50)
    parser.add_argument("--num-tccs", type=int, default=1)
    parser.add_argument("--sqc-size", type=str, default="32KiB")
    parser.add_argument("--sqc-assoc", type=int, default=8)
    parser.add_argument("--sqc-deadlock-threshold", type=int, default=500000)
    parser.add_argument("--WB_L1", action="store_true", default=False)
    parser.add_argument("--WB_L2", action="store_true", default=False)
    parser.add_argument("--TCP_latency", type=int, default=4)
    parser.add_argument("--mandatory_queue_latency", type=int, default=1)
    parser.add_argument("--TCC_latency", type=int, default=16)
    parser.add_argument("--tcc-size", type=str, default="256KiB")
    parser.add_argument("--tcc-assoc", type=int, default=16)
    parser.add_argument("--tcp-size", type=str, default="16KiB")
    parser.add_argument("--tcp-assoc", type=int, default=16)
    parser.add_argument("--tcp-deadlock-threshold", type=int, default=500000)
    parser.add_argument("--max-coalesces-per-cycle", type=int, default=1)
    parser.add_argument("--noL1", action="store_true", default=False)
    parser.add_argument("--glc-atomic-latency", type=int, default=1)
    parser.add_argument("--atomic-alu-latency", type=int, default=0)
    parser.add_argument("--tcc-num-atomic-alus", type=int, default=64)
    parser.add_argument("--tcp-num-banks", type=int, default=16)
    parser.add_argument("--tcc-num-banks", type=int, default=16)
    parser.add_argument("--tcc-tag-access-latency", type=int, default=2)
    parser.add_argument("--tcc-data-access-latency", type=int, default=8)


def construct_l1s(options, system, ruby_system, network):
    cpu_sequencers = []
    l1_cntrl_nodes = []
    num_l2_bits = int(math.log(options.num_dirs, 2))

    for i in range(options.num_cpus):
        l1_cntrl = L1Cntrl()
        l1_cntrl.create(options, ruby_system, system)
        l1_cntrl.l2_select_num_bits = num_l2_bits

        exec("ruby_system.l1_cntrl%d = l1_cntrl" % i)
        cpu_sequencers.append(l1_cntrl.sequencer)

        l1_cntrl.requestFromL1Cache = MessageBuffer()
        l1_cntrl.requestFromL1Cache.out_port = network.in_port
        l1_cntrl.responseFromL1Cache = MessageBuffer()
        l1_cntrl.responseFromL1Cache.out_port = network.in_port
        l1_cntrl.persistentFromL1Cache = MessageBuffer()
        l1_cntrl.persistentFromL1Cache.out_port = network.in_port

        l1_cntrl.requestToL1Cache = MessageBuffer()
        l1_cntrl.requestToL1Cache.in_port = network.out_port
        l1_cntrl.responseToL1Cache = MessageBuffer()
        l1_cntrl.responseToL1Cache.in_port = network.out_port
        l1_cntrl.persistentToL1Cache = MessageBuffer()
        l1_cntrl.persistentToL1Cache.in_port = network.out_port

        l1_cntrl.mandatoryQueue = MessageBuffer()
        l1_cntrl_nodes.append(l1_cntrl)

    return (cpu_sequencers, l1_cntrl_nodes)


def construct_l2s(options, system, ruby_system, network):
    l2_cntrl_nodes = []

    for i in range(options.num_cpus):
        l2_cntrl = L2Cntrl()
        l2_cntrl.create(options, ruby_system, system)
        exec("ruby_system.l2_cntrl%d = l2_cntrl" % i)

        l2_cntrl.L1RequestFromL2Cache = MessageBuffer()
        l2_cntrl.L1RequestFromL2Cache.out_port = network.in_port
        l2_cntrl.GlobalRequestFromL2Cache = MessageBuffer()
        l2_cntrl.GlobalRequestFromL2Cache.out_port = network.in_port
        l2_cntrl.responseFromL2Cache = MessageBuffer()
        l2_cntrl.responseFromL2Cache.out_port = network.in_port

        l2_cntrl.L1RequestToL2Cache = MessageBuffer()
        l2_cntrl.L1RequestToL2Cache.in_port = network.out_port
        l2_cntrl.GlobalRequestToL2Cache = MessageBuffer()
        l2_cntrl.GlobalRequestToL2Cache.in_port = network.out_port
        l2_cntrl.responseToL2Cache = MessageBuffer()
        l2_cntrl.responseToL2Cache.in_port = network.out_port
        l2_cntrl.triggerQueue = MessageBuffer(ordered=True) 

        exec("ruby_system.l2_cntrl%d = l2_cntrl" % i)
        l2_cntrl_nodes.append(l2_cntrl)

    return l2_cntrl_nodes


def construct_dirs(options, system, ruby_system, network):
    dir_cntrl_nodes = []

    if options.numa_high_bit:
        numa_bit = options.numa_high_bit
    else:
        dir_bits = int(math.log(options.num_dirs, 2))
        block_size_bits = int(math.log(options.cacheline_size, 2))
        numa_bit = block_size_bits + dir_bits - 1

    for i in range(options.num_dirs):
        dir_ranges = []
        for r in system.mem_ranges:
            addr_range = m5.objects.AddrRange(
                r.start,
                size=r.size(),
                intlvHighBit=numa_bit,
                intlvBits=dir_bits,
                intlvMatch=i,
            )
            dir_ranges.append(addr_range)

        dir_cntrl = DirCntrl()
        dir_cntrl.l2_select_num_bits = int(math.log(options.num_dirs, 2))
        dir_cntrl.create(options, dir_ranges, ruby_system, system)
        dir_cntrl.number_of_TBEs = options.num_tbes

        # Standard Requests
        dir_cntrl.requestToDir = MessageBuffer()
        dir_cntrl.requestToDir.in_port = network.out_port
        dir_cntrl.responseToDir = MessageBuffer()
        dir_cntrl.responseToDir.in_port = network.out_port

        dir_cntrl.persistentToDir = MessageBuffer()
        dir_cntrl.persistentToDir.in_port = network.out_port

        dir_cntrl.requestFromDir = MessageBuffer()
        dir_cntrl.requestFromDir.out_port = network.in_port
        dir_cntrl.responseFromDir = MessageBuffer()
        dir_cntrl.responseFromDir.out_port = network.in_port
        dir_cntrl.persistentFromDir = MessageBuffer()
        dir_cntrl.persistentFromDir.out_port = network.in_port

        # CORRECTED MEMORY PORTS: Stripped of network attachments
        dir_cntrl.requestToMemory = MessageBuffer()
        dir_cntrl.responseFromMemory = MessageBuffer()

        # CORRECTED DMA PORTS: Both require ordered=True
        dir_cntrl.dmaRequestToDir = MessageBuffer(ordered=True)
        dir_cntrl.dmaRequestToDir.in_port = network.out_port
        dir_cntrl.dmaResponseFromDir = MessageBuffer(ordered=True)
        dir_cntrl.dmaResponseFromDir.out_port = network.in_port
        dir_cntrl.triggerQueue = MessageBuffer(ordered=True)

        # AMTC: TCC/NB forwarded RequestMsg path to Directory on vnet 5
        dir_cntrl.requestToDirFromNB = MessageBuffer()
        dir_cntrl.requestToDirFromNB.in_port = network.out_port

        exec("ruby_system.dir_cntrl%d = dir_cntrl" % i)
        dir_cntrl_nodes.append(dir_cntrl)

    return dir_cntrl_nodes


def construct_gpudirs(options, system, ruby_system, network):
    dir_cntrl_nodes = []
    mem_ctrls = []

    xor_low_bit = 0

    # For an odd number of CPUs, still create the right number of controllers
    TCC_bits = int(math.log(options.num_tccs, 2))

    dir_bits = int(math.log(options.dgpu_num_dirs, 2))
    block_size_bits = int(math.log(options.cacheline_size, 2))
    numa_bit = block_size_bits + dir_bits - 1

    gpu_mem_range = AddrRange(0, size=options.dgpu_mem_size)
    for i in range(options.dgpu_num_dirs):
        addr_range = m5.objects.AddrRange(
            gpu_mem_range.start,
            size=gpu_mem_range.size(),
            intlvHighBit=numa_bit,
            intlvBits=dir_bits,
            intlvMatch=i,
            xorHighBit=xor_low_bit,
        )

        dir_cntrl = DirCntrl()
        dir_cntrl.l2_select_num_bits = int(math.log(options.dgpu_num_dirs, 2))
        dir_cntrl.create(options, [addr_range], ruby_system, system)
        dir_cntrl.number_of_TBEs = options.num_tbes
        # dir_cntrl.useL3OnWT = False
        # dir_cntrl.L2isWB = options.WB_L2

        # Connect the Directory controller to the ruby network
        # Connect the Directory controller to the ruby network
        dir_cntrl.requestToDir = MessageBuffer()
        dir_cntrl.requestToDir.in_port = network.out_port

        dir_cntrl.responseToDir = MessageBuffer()
        dir_cntrl.responseToDir.in_port = network.out_port

        dir_cntrl.persistentToDir = MessageBuffer()
        dir_cntrl.persistentToDir.in_port = network.out_port

        dir_cntrl.requestFromDir = MessageBuffer()
        dir_cntrl.requestFromDir.out_port = network.in_port

        dir_cntrl.responseFromDir = MessageBuffer()
        dir_cntrl.responseFromDir.out_port = network.in_port

        dir_cntrl.persistentFromDir = MessageBuffer()
        dir_cntrl.persistentFromDir.out_port = network.in_port

        dir_cntrl.requestToMemory = MessageBuffer()
        dir_cntrl.requestToMemory.out_port = network.in_port

        dir_cntrl.responseFromMemory = MessageBuffer()
        dir_cntrl.responseFromMemory.in_port = network.out_port

        dir_cntrl.dmaRequestToDir = MessageBuffer(ordered=True)
        dir_cntrl.dmaRequestToDir.in_port = network.out_port

        dir_cntrl.dmaResponseFromDir = MessageBuffer()
        dir_cntrl.dmaResponseFromDir.out_port = network.in_port

        dir_cntrl.requestToMemory = MessageBuffer()
        dir_cntrl.responseFromMemory = MessageBuffer()

        # Create memory controllers too
        mem_type = ObjectList.mem_list.get(options.dgpu_mem_type)

        if options.hbm_ctrl:
            # If HBM controller is enabled,
            # set up dram interfaces for two pseudo channels per HBMCtrl
            dram_intf = MemConfig.create_mem_intf(
                mem_type,
                gpu_mem_range,
                2 * i,
                int(math.log(options.dgpu_num_dirs, 2)) + 1,
                options.cacheline_size,
                xor_low_bit,
            )
            dram_intf_2 = MemConfig.create_mem_intf(
                mem_type,
                gpu_mem_range,
                2 * i + 1,
                int(math.log(options.dgpu_num_dirs, 2)) + 1,
                options.cacheline_size,
                xor_low_bit,
            )
        else:
            dram_intf = MemConfig.create_mem_intf(
                mem_type,
                gpu_mem_range,
                i,
                int(math.log(options.dgpu_num_dirs, 2)),
                options.cacheline_size,
                xor_low_bit,
            )

        if issubclass(mem_type, DRAMInterface):
            if options.hbm_ctrl:
                mem_ctrl = m5.objects.HBMCtrl(
                    dram=dram_intf, dram_2=dram_intf_2
                )
            else:
                mem_ctrl = m5.objects.MemCtrl(dram=dram_intf)
        else:
            mem_ctrl = dram_intf

        mem_ctrl.port = dir_cntrl.memory_out_port
        mem_ctrl.dram.enable_dram_powerdown = False

        if options.hbm_ctrl:
            dir_cntrl.addr_ranges = [dram_intf.range, dram_intf_2.range]
        else:
            dir_cntrl.addr_ranges = dram_intf.range
        # Append
        exec("ruby_system.gpu_dir_cntrl%d = dir_cntrl" % i)
        dir_cntrl_nodes.append(dir_cntrl)
        mem_ctrls.append(mem_ctrl)

    system.gpu_mem_ctrls = mem_ctrls

    return dir_cntrl_nodes, mem_ctrls


def construct_tcps(options, system, ruby_system, network):
    tcp_sequencers = []
    tcp_cntrl_nodes = []
    TCC_bits = int(math.log(options.num_tccs, 2))

    for i in range(options.num_compute_units):
        tcp_cntrl = TCPCntrl(
            TCC_select_num_bits=TCC_bits, issue_latency=1, number_of_TBEs=2560
        )
        tcp_cntrl.create(options, ruby_system, system)
        tcp_cntrl.WB = options.WB_L1
        tcp_cntrl.disableL1 = options.noL1
        tcp_cntrl.L1cache.tagAccessLatency = options.TCP_latency
        tcp_cntrl.L1cache.dataAccessLatency = options.TCP_latency

        exec("ruby_system.tcp_cntrl%d = tcp_cntrl" % i)

        tcp_sequencers.append(tcp_cntrl.coalescer)
        tcp_cntrl_nodes.append(tcp_cntrl)

        tcp_cntrl.requestFromTCP = MessageBuffer(ordered=True)
        tcp_cntrl.requestFromTCP.out_port = network.in_port
        tcp_cntrl.responseFromTCP = MessageBuffer(ordered=True)
        tcp_cntrl.responseFromTCP.out_port = network.in_port
        tcp_cntrl.unblockFromCore = MessageBuffer()
        tcp_cntrl.unblockFromCore.out_port = network.in_port

        tcp_cntrl.probeToTCP = MessageBuffer(ordered=True)
        tcp_cntrl.probeToTCP.in_port = network.out_port
        tcp_cntrl.responseToTCP = MessageBuffer(ordered=True)
        tcp_cntrl.responseToTCP.in_port = network.out_port

        tcp_cntrl.mandatoryQueue = MessageBuffer()

    return (tcp_sequencers, tcp_cntrl_nodes)


def construct_sqcs(options, system, ruby_system, network):
    sqc_sequencers = []
    sqc_cntrl_nodes = []
    TCC_bits = int(math.log(options.num_tccs, 2))

    for i in range(options.num_sqc):
        sqc_cntrl = SQCCntrl(TCC_select_num_bits=TCC_bits)
        sqc_cntrl.create(options, ruby_system, system)

        exec("ruby_system.sqc_cntrl%d = sqc_cntrl" % i)

        sqc_sequencers.append(sqc_cntrl.sequencer)
        sqc_cntrl_nodes.append(sqc_cntrl)

        sqc_cntrl.requestFromSQC = MessageBuffer(ordered=True)
        sqc_cntrl.requestFromSQC.out_port = network.in_port
        sqc_cntrl.probeToSQC = MessageBuffer(ordered=True)
        sqc_cntrl.probeToSQC.in_port = network.out_port
        sqc_cntrl.responseToSQC = MessageBuffer(ordered=True)
        sqc_cntrl.responseToSQC.in_port = network.out_port
        sqc_cntrl.mandatoryQueue = MessageBuffer()

    return (sqc_sequencers, sqc_cntrl_nodes)


def construct_scalars(options, system, ruby_system, network):
    scalar_sequencers = []
    scalar_cntrl_nodes = []
    TCC_bits = int(math.log(options.num_tccs, 2))

    for i in range(options.num_scalar_cache):
        scalar_cntrl = SQCCntrl(TCC_select_num_bits=TCC_bits)
        scalar_cntrl.create(options, ruby_system, system)

        exec("ruby_system.scalar_cntrl%d = scalar_cntrl" % i)

        scalar_sequencers.append(scalar_cntrl.sequencer)
        scalar_cntrl_nodes.append(scalar_cntrl)

        scalar_cntrl.requestFromSQC = MessageBuffer(ordered=True)
        scalar_cntrl.requestFromSQC.out_port = network.in_port
        scalar_cntrl.probeToSQC = MessageBuffer(ordered=True)
        scalar_cntrl.probeToSQC.in_port = network.out_port
        scalar_cntrl.responseToSQC = MessageBuffer(ordered=True)
        scalar_cntrl.responseToSQC.in_port = network.out_port
        scalar_cntrl.mandatoryQueue = MessageBuffer()

    return (scalar_sequencers, scalar_cntrl_nodes)


def construct_cmdprocs(options, system, ruby_system, network):
    cmdproc_sequencers = []
    cmdproc_cntrl_nodes = []
    TCC_bits = int(math.log(options.num_tccs, 2))

    for i in range(options.num_cp):
        tcp_ID = options.num_compute_units + i
        sqc_ID = options.num_sqc + i

        tcp_cntrl = TCPCntrl(
            TCC_select_num_bits=TCC_bits, issue_latency=1, number_of_TBEs=2560
        )
        tcp_cntrl.createCP(options, ruby_system, system)
        tcp_cntrl.WB = options.WB_L1
        tcp_cntrl.disableL1 = options.noL1
        tcp_cntrl.L1cache.tagAccessLatency = options.TCP_latency
        tcp_cntrl.L1cache.dataAccessLatency = options.TCP_latency

        exec("ruby_system.tcp_cntrl%d = tcp_cntrl" % tcp_ID)

        cmdproc_sequencers.append(tcp_cntrl.sequencer)
        cmdproc_cntrl_nodes.append(tcp_cntrl)

        tcp_cntrl.requestFromTCP = MessageBuffer(ordered=True)
        tcp_cntrl.requestFromTCP.out_port = network.in_port
        tcp_cntrl.responseFromTCP = MessageBuffer(ordered=True)
        tcp_cntrl.responseFromTCP.out_port = network.in_port
        tcp_cntrl.unblockFromCore = MessageBuffer(ordered=True)
        tcp_cntrl.unblockFromCore.out_port = network.in_port

        tcp_cntrl.probeToTCP = MessageBuffer(ordered=True)
        tcp_cntrl.probeToTCP.in_port = network.out_port
        tcp_cntrl.responseToTCP = MessageBuffer(ordered=True)
        tcp_cntrl.responseToTCP.in_port = network.out_port
        tcp_cntrl.mandatoryQueue = MessageBuffer()

        sqc_cntrl = SQCCntrl(TCC_select_num_bits=TCC_bits)
        sqc_cntrl.create(options, ruby_system, system)

        exec("ruby_system.sqc_cntrl%d = sqc_cntrl" % sqc_ID)

        cmdproc_sequencers.append(sqc_cntrl.sequencer)
        cmdproc_cntrl_nodes.append(sqc_cntrl)

    return (cmdproc_sequencers, cmdproc_cntrl_nodes)


def construct_tccs(options, system, ruby_system, network):
    tcc_cntrl_nodes = []

    for i in range(options.num_tccs):
        tcc_cntrl = TCCCntrl(l2_response_latency=options.TCC_latency)
        tcc_cntrl.create(options, ruby_system, system)
        tcc_cntrl.l2_request_latency = options.gpu_to_dir_latency
        tcc_cntrl.l2_response_latency = options.TCC_latency
        tcc_cntrl.glc_atomic_latency = options.glc_atomic_latency
        tcc_cntrl_nodes.append(tcc_cntrl)
        tcc_cntrl.WB = options.WB_L2
        tcc_cntrl.number_of_TBEs = 2560 * options.num_compute_units

        tcc_cntrl.requestFromTCP = MessageBuffer(ordered=True)
        tcc_cntrl.requestFromTCP.in_port = network.out_port
        tcc_cntrl.responseToCore = MessageBuffer(ordered=True)
        tcc_cntrl.responseToCore.out_port = network.in_port
        tcc_cntrl.probeFromNB = MessageBuffer()
        tcc_cntrl.probeFromNB.in_port = network.out_port
        tcc_cntrl.responseFromNB = MessageBuffer()
        tcc_cntrl.responseFromNB.in_port = network.out_port
        # AMTC: owner/stale-owner responses from CPU L1 to TCC on vnet 4
        tcc_cntrl.ownerResponseFromL1 = MessageBuffer()
        tcc_cntrl.ownerResponseFromL1.in_port = network.out_port

        tcc_cntrl.requestToNB = MessageBuffer(ordered=True)
        tcc_cntrl.requestToNB.out_port = network.in_port
        tcc_cntrl.responseToNB = MessageBuffer()
        tcc_cntrl.responseToNB.out_port = network.in_port
        tcc_cntrl.unblockToNB = MessageBuffer()
        tcc_cntrl.unblockToNB.out_port = network.in_port
        tcc_cntrl.triggerQueue = MessageBuffer(ordered=True)
        # Update to the new Funnel port name:
        tcc_cntrl.L1RequestToL2Cache = MessageBuffer(ordered=True)
        tcc_cntrl.L1RequestToL2Cache.out_port = network.in_port

        exec("ruby_system.tcc_cntrl%d = tcc_cntrl" % i)

    return tcc_cntrl_nodes


def create_system(
    options, full_system, system, dma_devices, bootmem, ruby_system, cpus
):
    # if buildEnv["PROTOCOL"] != "AMTC":
    #     panic("This script requires the AMTC protocol to be built.")

    cpu_sequencers = []

    crossbar_bw = None
    mainCluster = None
    cpuCluster = None
    gpuCluster = None

    if hasattr(options, "bw_scalor") and options.bw_scalor > 0:
        crossbar_bw = 16 * options.num_compute_units * options.bw_scalor
        mainCluster = Cluster(intBW=crossbar_bw)
        cpuCluster = Cluster(extBW=crossbar_bw, intBW=crossbar_bw)
        gpuCluster = Cluster(extBW=crossbar_bw, intBW=crossbar_bw)
    else:
        mainCluster = Cluster(intBW=8)
        cpuCluster = Cluster(extBW=8, intBW=8)
        gpuCluster = Cluster(extBW=8, intBW=8)

    # Create CPU directory controllers
    dir_cntrl_nodes = construct_dirs(
        options, system, ruby_system, ruby_system.network
    )
    for dir_cntrl in dir_cntrl_nodes:
        mainCluster.add(dir_cntrl)

    # Create CPU L1 Token Controllers
    (l1_sequencers, l1_cntrl_nodes) = construct_l1s(
        options, system, ruby_system, ruby_system.network
    )
    cpu_sequencers.extend(l1_sequencers)
    for l1_cntrl in l1_cntrl_nodes:
        cpuCluster.add(l1_cntrl)

    # Create CPU L2 Token Controllers
    l2_cntrl_nodes = construct_l2s(
        options, system, ruby_system, ruby_system.network
    )
    for l2_cntrl in l2_cntrl_nodes:
        cpuCluster.add(l2_cntrl)

    if not full_system:
        for i in range((options.num_cpus + 1) // 2):
            FileSystemConfig.register_cpu(
                physical_package_id=0,
                core_siblings=range(options.num_cpus),
                core_id=i * 2,
                thread_siblings=[],
            )
            FileSystemConfig.register_cpu(
                physical_package_id=0,
                core_siblings=range(options.num_cpus),
                core_id=i * 2 + 1,
                thread_siblings=[],
            )
            FileSystemConfig.register_cache(
                level=0,
                idu_type="Instruction",
                size=options.l1i_size,
                line_size=options.cacheline_size,
                assoc=options.l1i_assoc,
                cpus=[i * 2, i * 2 + 1],
            )
            FileSystemConfig.register_cache(
                level=0,
                idu_type="Data",
                size=options.l1d_size,
                line_size=options.cacheline_size,
                assoc=options.l1d_assoc,
                cpus=[i * 2],
            )
            FileSystemConfig.register_cache(
                level=0,
                idu_type="Data",
                size=options.l1d_size,
                line_size=options.cacheline_size,
                assoc=options.l1d_assoc,
                cpus=[i * 2 + 1],
            )
            FileSystemConfig.register_cache(
                level=1,
                idu_type="Unified",
                size=options.l2_size,
                line_size=options.cacheline_size,
                assoc=options.l2_assoc,
                cpus=[i * 2, i * 2 + 1],
            )

        for i in range(options.num_dirs):
            FileSystemConfig.register_cache(
                level=2,
                idu_type="Unified",
                size=options.l3_size,
                line_size=options.cacheline_size,
                assoc=options.l3_assoc,
                cpus=[n for n in range(options.num_cpus)],
            )

    # Create TCPs
    (tcp_sequencers, tcp_cntrl_nodes) = construct_tcps(
        options, system, ruby_system, ruby_system.network
    )
    cpu_sequencers.extend(tcp_sequencers)
    for tcp_cntrl in tcp_cntrl_nodes:
        gpuCluster.add(tcp_cntrl)

    # Create SQCs
    (sqc_sequencers, sqc_cntrl_nodes) = construct_sqcs(
        options, system, ruby_system, ruby_system.network
    )
    cpu_sequencers.extend(sqc_sequencers)
    for sqc_cntrl in sqc_cntrl_nodes:
        gpuCluster.add(sqc_cntrl)

    # Create Scalars
    (scalar_sequencers, scalar_cntrl_nodes) = construct_scalars(
        options, system, ruby_system, ruby_system.network
    )
    cpu_sequencers.extend(scalar_sequencers)
    for scalar_cntrl in scalar_cntrl_nodes:
        gpuCluster.add(scalar_cntrl)

    # Create command processors
    (cmdproc_sequencers, cmdproc_cntrl_nodes) = construct_cmdprocs(
        options, system, ruby_system, ruby_system.network
    )
    cpu_sequencers.extend(cmdproc_sequencers)
    for cmdproc_cntrl in cmdproc_cntrl_nodes:
        gpuCluster.add(cmdproc_cntrl)

    # Create TCCs
    tcc_cntrl_nodes = construct_tccs(
        options, system, ruby_system, ruby_system.network
    )
    for tcc_cntrl in tcc_cntrl_nodes:
        gpuCluster.add(tcc_cntrl)

    # Create DMA Controllers
    for i, dma_device in enumerate(dma_devices):
        dma_seq = DMASequencer(version=i, ruby_system=ruby_system)
        dma_cntrl = APU_AMTC_DMA_Controller(
            version=i, dma_sequencer=dma_seq, ruby_system=ruby_system
        )
        exec("system.dma_cntrl%d = dma_cntrl" % i)

        if not hasattr(dma_device, "type"):
            exec("system.dma_cntrl%d.dma_sequencer.in_ports = dma_device" % i)
        elif dma_device.type == "MemTest":
            exec(
                "system.dma_cntrl%d.dma_sequencer.in_ports = dma_devices.test"
                % i
            )
        else:
            exec(
                "system.dma_cntrl%d.dma_sequencer.in_ports = dma_device.dma"
                % i
            )

        dma_cntrl.reqToDirectory = MessageBuffer(buffer_size=0)
        dma_cntrl.reqToDirectory.out_port = ruby_system.network.in_port
        dma_cntrl.responseFromDir = MessageBuffer(buffer_size=0)
        dma_cntrl.responseFromDir.in_port = ruby_system.network.out_port
        dma_cntrl.mandatoryQueue = MessageBuffer(buffer_size=0)
        gpuCluster.add(dma_cntrl)

    mainCluster.add(cpuCluster)
    mainCluster.add(gpuCluster)

    # Set virtual networks to 6 as required by AMTC
    ruby_system.network.number_of_virtual_networks = 11

    # Return the full list of sequencers to the frontend python script
    return (cpu_sequencers, dir_cntrl_nodes, mainCluster)
