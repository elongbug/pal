/*
 *******************************************************************************
 *
 * Copyright (c) 2016-2017 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include "core/hw/gfxip/gfx9/gfx9PerfCtrInfo.h"
#include "core/perfCounter.h"

namespace Pal
{
namespace Gfx9
{

class CmdStream;
class Device;

// =====================================================================================================================
// Provides Gfx9-specific functionality for global (i.e., "summary") performance counters.
class PerfCounter : public Pal::PerfCounter
{
public:
    PerfCounter(const Device& device, const PerfCounterInfo& info, uint32 slot);
    virtual ~PerfCounter() {}

    uint32 SetupSdmaSelectReg(
        regSDMA0_PERFMON_CNTL* pSdma0PerfmonCntl,
        regSDMA1_PERFMON_CNTL* pSdma1PerfmonCntl) const;

    uint32* WriteSetupCommands(
        CmdStream* pCmdStream,
        uint32*    pCmdSpace) const;

    uint32* WriteSampleCommands(
        gpusize     baseGpuVirtAddr,
        CmdStream*  pCmdStream,
        uint32*     pCmdSpace) const;

    // Returns true if the GPU block this counter samples from is indexed for reads and writes
    bool IsIndexed() const { return (m_flags.isIndexed != 0); }

private:
    uint32* WriteGrbmGfxIndex(CmdStream* pCmdStream, uint32* pCmdSpace) const;
    uint32* WriteGrbmGfxBroadcastSe(CmdStream* pCmdStream, uint32* pCmdSpace) const;

    uint32 InstanceIdToSe() const;
    uint32 InstanceIdToSh() const;
    uint32 InstanceIdToInstance() const;

    union Flags
    {
        struct
        {
            uint32  isIndexed :  1; // Set if the Block is indexed for ctr reads/writes
            uint32  reserved  : 31; // Reserved bits
        };
        uint32 u32All;
    };

    const Device& m_device;
    Flags         m_flags;

    uint32 m_numActiveRegs;                               // Number of active select registers
    uint32 m_selectReg[PerfCtrInfo::MaxPerfCtrSelectReg]; // Value of each performance counter select register.
    uint32 m_rsltCntlReg;                                 // Result control register for memory system blocks.
    uint32                      m_perfCountLoAddr;  // Register address of the low 32 bits of the perf counter
    uint32                      m_perfCountHiAddr;  // Register address of the high 32 bits of the perf counter
    ME_COPY_DATA_src_sel_enum   m_mePerfCntSrcSel;  // Source-select value to use for Graphics COPY_DATA PM4 commands
    MEC_COPY_DATA_src_sel_enum  m_mecPerfCntSrcSel; // Source-select value to use for Compute COPY_DATA PM4 commands

    PAL_DISALLOW_DEFAULT_CTOR(PerfCounter);
    PAL_DISALLOW_COPY_AND_ASSIGN(PerfCounter);
};

} // Gfx9
} // Pal
