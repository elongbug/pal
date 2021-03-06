/*
 *******************************************************************************
 *
 * Copyright (c) 2015-2017 Advanced Micro Devices, Inc. All rights reserved.
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

#include "core/hw/gfxip/gfx6/gfx6CmdStream.h"
#include "core/hw/gfxip/gfx6/gfx6Device.h"
#include "core/hw/gfxip/gfx6/gfx6Pm4Optimizer.h"
#include "core/hw/gfxip/gfx6/gfx6UserDataTableImpl.h"
#include "palDequeImpl.h"
#include "palLinearAllocator.h"

using namespace Util;

namespace Pal
{
namespace Gfx6
{

// =====================================================================================================================
// Helper function for determining the command buffer chain size (in DWORD's). This value can be affected by workarounds
// for hardware issues.
static PAL_INLINE uint32 GetChainSizeInDwords(
    const Device& device,
    bool          isNested)
{
    uint32 chainSize = CmdUtil::GetChainSizeInDwords();

    const Gfx6PalSettings& settings = device.Settings();
    if (isNested && (device.WaCpIb2ChainingUnsupported() != false))
    {
        // Some GPU's do not support chaining between the chunks of an IB2. This means that we cannot use chaining
        // for nested command buffers on these chips.  When executing a nested command buffer using IB2's on these
        // GPU's, we will use a separate IB2 packet for each chunk rather than issuing a single IB2 for the head
        // chunk.
        chainSize = 0;
    }

    return chainSize;
}

// =====================================================================================================================
CmdStream::CmdStream(
    const Device&  device,
    ICmdAllocator* pCmdAllocator,
    EngineType     engineType,
    SubQueueType   subqueueType,
    bool           isNested,
    bool           disablePreemption)
    :
    Pal::GfxCmdStream(device,
                      pCmdAllocator,
                      engineType,
                      subqueueType,
                      GetChainSizeInDwords(device, isNested),
                      device.CmdUtil().GetMinNopSizeInDwords(),
                      CmdUtil::GetCondIndirectBufferSize(),
                      isNested,
                      disablePreemption),
    m_cmdUtil(device.CmdUtil()),
    m_pPm4Optimizer(nullptr)
{
}

// =====================================================================================================================
Result CmdStream::Begin(
    CmdStreamBeginFlags     flags,
    VirtualLinearAllocator* pMemAllocator)
{
    // We simply can't enable PM4 optimization without an allocator because we need to dynamically allocate a
    // Pm4Optimizer. We also shouldn't optimize CE streams because Pm4Optimizer has no optimizations for them.
    flags.optimizeCommands &= (pMemAllocator != nullptr) && (IsConstantEngine() == false);

    Result result = GfxCmdStream::Begin(flags, pMemAllocator);

    if ((result == Result::Success) && (m_flags.optimizeCommands == 1))
    {
        // Allocate a temporary PM4 optimizer to use during command building.
        m_pPm4Optimizer = PAL_NEW(Pm4Optimizer, m_pMemAllocator, AllocInternal)(static_cast<const Device&>(m_device));

        if (m_pPm4Optimizer == nullptr)
        {
            result = Result::ErrorOutOfMemory;
        }
    }

    return result;
}

// =====================================================================================================================
void CmdStream::CleanupTempObjects()
{
    // Clean up the temporary PM4 optimizer object.
    if (m_pMemAllocator != nullptr)
    {
        PAL_SAFE_DELETE(m_pPm4Optimizer, m_pMemAllocator);
    }
}

// =====================================================================================================================
// Copies the given PM4 image into the given buffer. The PM4 optimizer may strip out extra packets.
// Returns a pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WritePm4Image(
    size_t      sizeInDwords,
    const void* pImage,
    uint32*     pCmdSpace)
{
    if (m_flags.optModeImmediate == 1)
    {
        uint32 optSize = static_cast<uint32>(sizeInDwords);
        m_pPm4Optimizer->OptimizePm4Commands(static_cast<const uint32*>(pImage), pCmdSpace, &optSize);
        pCmdSpace += optSize;
    }
    else
    {
        memcpy(pCmdSpace, pImage, sizeInDwords * sizeof(uint32));
        pCmdSpace += sizeInDwords;
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Builds a PM4 packet to modify the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <bool pm4OptImmediate>
uint32* CmdStream::WriteContextRegRmw(
    uint32  regAddr,
    uint32  regMask,
    uint32  regData,
    uint32* pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if ((pm4OptImmediate == false) ||
        m_pPm4Optimizer->MustKeepContextRegRmw(regAddr, regMask, regData))
    {
        pCmdSpace += m_cmdUtil.BuildContextRegRmw(regAddr, regMask, regData, pCmdSpace);
    }

    return pCmdSpace;
}

template
uint32* CmdStream::WriteContextRegRmw<true>(
    uint32  regAddr,
    uint32  regMask,
    uint32  regData,
    uint32* pCmdSpace);
template
uint32* CmdStream::WriteContextRegRmw<false>(
    uint32  regAddr,
    uint32  regMask,
    uint32  regData,
    uint32* pCmdSpace);

// =====================================================================================================================
// Wrapper for real WriteContextRegRmw() when it isn't known whether the immediate pm4 optimizer is enabled.
uint32* CmdStream::WriteContextRegRmw(
    uint32  regAddr,
    uint32  regMask,
    uint32  regData,
    uint32* pCmdSpace)
{
    if (m_flags.optModeImmediate == 0)
    {
        pCmdSpace = WriteContextRegRmw<false>(regAddr, regMask, regData, pCmdSpace);
    }
    else
    {
        pCmdSpace = WriteContextRegRmw<true>(regAddr, regMask, regData, pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <bool pm4OptImmediate>
uint32* CmdStream::WriteSetIaMultiVgtParam(
    regIA_MULTI_VGT_PARAM iaMultiVgtParam,
    uint32*               pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if ((pm4OptImmediate == false) ||
        m_pPm4Optimizer->MustKeepSetContextReg(mmIA_MULTI_VGT_PARAM, iaMultiVgtParam.u32All))
    {
        const size_t totalDwords =
            m_cmdUtil.BuildSetOneContextReg(mmIA_MULTI_VGT_PARAM, pCmdSpace, SET_CONTEXT_INDEX_MULTI_VGT_PARAM);

        pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = iaMultiVgtParam.u32All;
        pCmdSpace += totalDwords;
    }

    return pCmdSpace;
}

template
uint32* CmdStream::WriteSetIaMultiVgtParam<true>(
    regIA_MULTI_VGT_PARAM iaMultiVgtParam,
    uint32*               pCmdSpace);

template
uint32* CmdStream::WriteSetIaMultiVgtParam<false>(
    regIA_MULTI_VGT_PARAM iaMultiVgtParam,
    uint32*               pCmdSpace);

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <bool pm4OptImmediate>
uint32* CmdStream::WriteSetVgtLsHsConfig(
    regVGT_LS_HS_CONFIG vgtLsHsConfig,
    uint32*             pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if ((pm4OptImmediate == false) ||
        m_pPm4Optimizer->MustKeepSetContextReg(mmVGT_LS_HS_CONFIG, vgtLsHsConfig.u32All))
    {
        const size_t totalDwords =
            m_cmdUtil.BuildSetOneContextReg(mmVGT_LS_HS_CONFIG, pCmdSpace, SET_CONTEXT_INDEX_VGT_LS_HS_CONFIG);

        pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = vgtLsHsConfig.u32All;
        pCmdSpace += totalDwords;
    }

    return pCmdSpace;
}

template
uint32* CmdStream::WriteSetVgtLsHsConfig<true>(
    regVGT_LS_HS_CONFIG vgtLsHsConfig,
    uint32*             pCmdSpace);

template
uint32* CmdStream::WriteSetVgtLsHsConfig<false>(
    regVGT_LS_HS_CONFIG vgtLsHsConfig,
    uint32*             pCmdSpace);

// =====================================================================================================================
// Builds a PM4 packet to the given register and returns a pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WriteSetPaScRasterConfig(
    regPA_SC_RASTER_CONFIG paScRasterConfig,
    uint32*             pCmdSpace)
{
    if (m_device.Parent()->ChipProperties().gfx6.rbReconfigureEnabled)
    {
        const size_t totalDwords =
            m_cmdUtil.BuildSetOneContextReg(mmPA_SC_RASTER_CONFIG, pCmdSpace, SET_CONTEXT_INDEX_PA_SC_RASTER_CONFIG);

        pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = paScRasterConfig.u32All;
        pCmdSpace += totalDwords;
    }
    else
    {
        pCmdSpace = WriteSetOneContextReg(mmPA_SC_RASTER_CONFIG,
                                          paScRasterConfig.u32All,
                                          pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WriteSetOneConfigReg(
    uint32  regAddr,
    uint32  regData,
    uint32* pCmdSpace)
{
    const size_t totalDwords = m_cmdUtil.BuildSetOneConfigReg(regAddr, pCmdSpace);
    pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = regData;

    return pCmdSpace + totalDwords;
}

// =====================================================================================================================
// Builds a PM4 packet to set the given set of sequential config registers.  Returns a pointer to the next unused DWORD
// in pCmdSpace.
uint32* CmdStream::WriteSetSeqConfigRegs(
    uint32      startRegAddr,
    uint32      endRegAddr,
    const void* pData,
    uint32*     pCmdSpace)
{
    const size_t totalDwords = m_cmdUtil.BuildSetSeqConfigRegs(startRegAddr, endRegAddr, pCmdSpace);

    memcpy(&pCmdSpace[PM4_CMD_SET_DATA_DWORDS], pData, totalDwords * sizeof(uint32) - sizeof(PM4CMDSETDATA));

    return (pCmdSpace + totalDwords);
}

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <bool pm4OptImmediate>
uint32* CmdStream::WriteSetOneContextReg(
    uint32  regAddr,
    uint32  regData,
    uint32* pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if ((pm4OptImmediate == false) ||
        m_pPm4Optimizer->MustKeepSetContextReg(regAddr, regData))
    {
        const size_t totalDwords = m_cmdUtil.BuildSetOneContextReg(regAddr, pCmdSpace);

        pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = regData;
        pCmdSpace += totalDwords;
    }

    return pCmdSpace;
}

template
uint32* CmdStream::WriteSetOneContextReg<true>(
    uint32  regAddr,
    uint32  regData,
    uint32* pCmdSpace);
template
uint32* CmdStream::WriteSetOneContextReg<false>(
    uint32  regAddr,
    uint32  regData,
    uint32* pCmdSpace);

// =====================================================================================================================
// Wrapper for the real WriteSetOneContextReg() when it isn't known whether the immediate pm4 optimizer is enabled.
uint32* CmdStream::WriteSetOneContextReg(
    uint32  regAddr,
    uint32  regData,
    uint32* pCmdSpace)
{
    if (m_flags.optModeImmediate == 0)
    {
        pCmdSpace = WriteSetOneContextReg<false>(regAddr, regData, pCmdSpace);
    }
    else
    {
        pCmdSpace = WriteSetOneContextReg<true>(regAddr, regData, pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Writes a register for performance counters. (Some performance counter reg's are protected and others aren't). Returns
// the size of the PM4 command written, in DWORDs.
uint32* CmdStream::WriteSetOnePerfCtrReg(
    uint32  regAddr,
    uint32  value,
    uint32* pCmdSpace) // [out] Build the PM4 packet in this buffer.
{
    uint32* pReturnVal = nullptr;

    if (m_cmdUtil.IsPrivilegedConfigReg(regAddr))
    {
        // Protected register: use our COPY_DATA backdoor to write the register.
        pReturnVal = WriteSetOnePrivilegedConfigReg(regAddr, value, pCmdSpace);
    }
    else
    {
        // Non-protected register: use a normal SET_DATA command.
        pReturnVal = WriteSetOneConfigReg(regAddr, value, pCmdSpace);
    }

    return pReturnVal;
}

// =====================================================================================================================
// Writes a config register using a COPY_DATA packet. This is a back-door we have to write privileged registers which
// cannot be set using a SET_DATA packet. Returns the size of the PM4 command written, in DWORDs.
uint32* CmdStream::WriteSetOnePrivilegedConfigReg(
    uint32  regAddr,
    uint32  value,
    uint32* pCmdSpace) // [out] Build the PM4 packet in this buffer.
{
    // Note: On Gfx7+, all privileged registers need to be written with the DST_SYS_PERF_COUNTER dest-select. On Gfx6,
    // only certain MC registers require this.
    const uint32 dstSelect = (m_cmdUtil.IsPrivilegedConfigReg(regAddr)
                                ? COPY_DATA_SEL_DST_SYS_PERF_COUNTER
                                : COPY_DATA_SEL_REG);

    return pCmdSpace + m_cmdUtil.BuildCopyData(dstSelect,
                                               regAddr,
                                               COPY_DATA_SEL_SRC_IMME_DATA,
                                               value,
                                               COPY_DATA_SEL_COUNT_1DW,
                                               COPY_DATA_ENGINE_ME,
                                               COPY_DATA_WR_CONFIRM_NO_WAIT,
                                               pCmdSpace);
}

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <PM4ShaderType shaderType, bool pm4OptImmediate>
uint32* CmdStream::WriteSetOneShReg(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if ((pm4OptImmediate == false) ||
        m_pPm4Optimizer->MustKeepSetShReg(regAddr, regData))
    {
        const size_t totalDwords = m_cmdUtil.BuildSetOneShReg(regAddr, shaderType, pCmdSpace);

        pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = regData;
        pCmdSpace += totalDwords;
    }

    return pCmdSpace;
}

template
uint32* CmdStream::WriteSetOneShReg<ShaderGraphics, true>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);
template
uint32* CmdStream::WriteSetOneShReg<ShaderGraphics, false>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);
template
uint32* CmdStream::WriteSetOneShReg<ShaderCompute, true>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);
template
uint32* CmdStream::WriteSetOneShReg<ShaderCompute, false>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);

// =====================================================================================================================
// Wrapper for the real WriteSetOneShReg() for when the caller doesn't know if the immediate pm4 optimizer is enabled.
template <PM4ShaderType shaderType>
uint32* CmdStream::WriteSetOneShReg(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace)
{
    if (m_flags.optModeImmediate)
    {
        pCmdSpace = WriteSetOneShReg<shaderType, true>(regAddr, regData, pCmdSpace);
    }
    else
    {
        pCmdSpace = WriteSetOneShReg<shaderType, false>(regAddr, regData, pCmdSpace);
    }

    return pCmdSpace;
}

// Instantiate the template for the linker.
template
uint32* CmdStream::WriteSetOneShReg<ShaderGraphics>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);
template
uint32* CmdStream::WriteSetOneShReg<ShaderCompute>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);

// =====================================================================================================================
// Builds a PM4 packet to set the given registers unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <bool pm4OptImmediate>
uint32* CmdStream::WriteSetSeqContextRegs(
    uint32      startRegAddr,
    uint32      endRegAddr,
    const void* pData,
    uint32*     pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if (pm4OptImmediate)
    {
        PM4CMDSETDATA setData;
        m_cmdUtil.BuildSetSeqContextRegs(startRegAddr, endRegAddr, &setData);

        pCmdSpace = m_pPm4Optimizer->WriteOptimizedSetSeqContextRegs(setData,
                                                                     static_cast<const uint32*>(pData),
                                                                     pCmdSpace);
    }
    else
    {
        const size_t totalDwords = m_cmdUtil.BuildSetSeqContextRegs(startRegAddr, endRegAddr, pCmdSpace);

        memcpy(&pCmdSpace[PM4_CMD_SET_DATA_DWORDS], pData, totalDwords * sizeof(uint32) - sizeof(PM4CMDSETDATA));
        pCmdSpace += totalDwords;
    }

    return pCmdSpace;
}

template
uint32* CmdStream::WriteSetSeqContextRegs<true>(
    uint32      startRegAddr,
    uint32      endRegAddr,
    const void* pData,
    uint32*     pCmdSpace);
template
uint32* CmdStream::WriteSetSeqContextRegs<false>(
    uint32      startRegAddr,
    uint32      endRegAddr,
    const void* pData,
    uint32*     pCmdSpace);

// =====================================================================================================================
// Wrapper for the real WriteSetSeqContextRegs() for when the caller doesn't know if the immediate mode pm4 optimizer
// is enabled.
uint32* CmdStream::WriteSetSeqContextRegs(
    uint32      startRegAddr,
    uint32      endRegAddr,
    const void* pData,
    uint32*     pCmdSpace)
{
    if (m_flags.optModeImmediate == 1)
    {
        pCmdSpace = WriteSetSeqContextRegs<true>(startRegAddr, endRegAddr, pData, pCmdSpace);
    }
    else
    {
        pCmdSpace = WriteSetSeqContextRegs<false>(startRegAddr, endRegAddr, pData, pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Builds a PM4 packet to set the given registers unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WriteSetSeqShRegs(
    uint32        startRegAddr,
    uint32        endRegAddr,
    PM4ShaderType shaderType,
    const void*   pData,
    uint32*       pCmdSpace)
{
    if (m_flags.optModeImmediate == 1)
    {
        PM4CMDSETDATA setData;
        m_cmdUtil.BuildSetSeqShRegs(startRegAddr, endRegAddr, shaderType, &setData);

        pCmdSpace = m_pPm4Optimizer->WriteOptimizedSetSeqShRegs(setData,
                                                                static_cast<const uint32*>(pData),
                                                                pCmdSpace);
    }
    else
    {
        const size_t totalDwords = m_cmdUtil.BuildSetSeqShRegs(startRegAddr, endRegAddr, shaderType, pCmdSpace);

        memcpy(&pCmdSpace[PM4_CMD_SET_DATA_DWORDS], pData, totalDwords * sizeof(uint32) - sizeof(PM4CMDSETDATA));
        pCmdSpace += totalDwords;
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
template <PM4ShaderType shaderType, bool pm4OptImmediate>
uint32* CmdStream::WriteSetShRegDataOffset(
    uint32        regAddr,
    uint32        dataOffset,
    uint32*       pCmdSpace)
{
    PAL_ASSERT(m_flags.optModeImmediate == pm4OptImmediate);

    if (pm4OptImmediate)
    {
        PM4CMDSETSHREGOFFSET setShRegOffset;

        const size_t totalDwords = m_cmdUtil.BuildSetShRegDataOffset(regAddr,
                                                                     shaderType,
                                                                     dataOffset,
                                                                     &setShRegOffset);
        pCmdSpace = m_pPm4Optimizer->WriteOptimizedSetShShRegOffset(setShRegOffset, totalDwords, pCmdSpace);
    }
    else
    {
        pCmdSpace += m_cmdUtil.BuildSetShRegDataOffset(regAddr,
                                                       shaderType,
                                                       dataOffset,
                                                       pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Wrapper for the real WriteSetShRegDataOffset() for when the caller doesn't know if the immediate pm4 optimizer is
// enabled.
template <PM4ShaderType shaderType>
uint32* CmdStream::WriteSetShRegDataOffset(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace)
{
    if (m_flags.optModeImmediate)
    {
        pCmdSpace = WriteSetShRegDataOffset<shaderType, true>(regAddr, regData, pCmdSpace);
    }
    else
    {
        pCmdSpace = WriteSetShRegDataOffset<shaderType, false>(regAddr, regData, pCmdSpace);
    }

    return pCmdSpace;
}

// Instantiate the template for the linker.
template
uint32* CmdStream::WriteSetShRegDataOffset<ShaderGraphics>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);
template
uint32* CmdStream::WriteSetShRegDataOffset<ShaderCompute>(
    uint32        regAddr,
    uint32        regData,
    uint32*       pCmdSpace);

// =====================================================================================================================
// Helper function to route to fast path for writing one user-data entry to an SPI register or to the general path for
// writing many.
uint32* CmdStream::WriteUserDataRegisters(
    const UserDataEntryMap& entryMap,
    const UserDataArgs*     pUserDataArgs,
    PM4ShaderType           shaderType,
    uint32*                 pCmdSpace)
{
    if (pUserDataArgs->entryCount == 1)
    {
        pCmdSpace = WriteUserDataRegistersOne(entryMap, pUserDataArgs, shaderType, pCmdSpace);
    }
    else
    {
        pCmdSpace = WriteUserDataRegistersMany(entryMap, pUserDataArgs, shaderType, pCmdSpace);
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Helper function to write one user-data entry which has been remapped to a SPI user-data register. Returns a
// pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WriteUserDataRegistersOne(
    const UserDataEntryMap& entryMap,
    const UserDataArgs*     pUserDataArgs,
    PM4ShaderType           shaderType,
    uint32*                 pCmdSpace)
{
    uint32 regAddr = entryMap.regAddr[pUserDataArgs->firstEntry];

    if (regAddr != UserDataNotMapped)
    {
        if (m_flags.optModeImmediate == 1)
        {
            PM4CMDSETDATA setData;

            const size_t totalDwords = m_cmdUtil.BuildSetSeqShRegs(regAddr, regAddr, shaderType, &setData);
            PAL_ASSERT(totalDwords == (1 + PM4_CMD_SET_DATA_DWORDS));

            pCmdSpace = m_pPm4Optimizer->WriteOptimizedSetSeqShRegs(setData,
                                                                    &(pUserDataArgs->pEntryValues[0]),
                                                                    pCmdSpace);
        }
        else
        {
            uint32*const pCmdPayload = (pCmdSpace + PM4_CMD_SET_DATA_DWORDS);

            const size_t totalDwords = m_cmdUtil.BuildSetSeqShRegs(regAddr, regAddr, shaderType, pCmdSpace);
            PAL_ASSERT(totalDwords == (1 + PM4_CMD_SET_DATA_DWORDS));

            pCmdPayload[0] = pUserDataArgs->pEntryValues[0];

            // The packet is complete and will not be optimized, fix-up pCmdSpace and we're done.
            PAL_ASSERT(totalDwords == (1 + PM4_CMD_SET_DATA_DWORDS));
            pCmdSpace += totalDwords;
        }
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Helper function to write a group of user-data entries which have been remapped to SPI user-data registers. Returns a
// pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WriteUserDataRegistersMany(
    const UserDataEntryMap& entryMap,
    const UserDataArgs*     pUserDataArgs,
    PM4ShaderType           shaderType,
    uint32*                 pCmdSpace)
{
    // Virtualized user-data entries are always remapped to a consecutive sequence of SPI user-data registers. Because
    // the entries are remapped to consecutive registers, we can always assume that this call will result in a sequence
    // of zero or more SPI registers being written.
    //
    // NOTE: We'll track the last register address written and the count of registers written rather than the starting
    // register address to prevent unnecessary branching in the loop below.

    uint32 firstEntry          = pUserDataArgs->firstEntry;
    uint32 entryCount          = pUserDataArgs->entryCount;
    const uint32* pEntryValues = pUserDataArgs->pEntryValues;

    uint32 endRegAddr = 0;
    uint32 count      = 0;

    // This loop will copy all of the mapped user-data entries' values into the data buffer following the PM4 command
    // header.  If we are using the optimizer, we need to write into cacheable memory because the optimizer will read
    // from the data.

    uint32 scratchMem[MaxUserDataEntries];

    uint32*const pCmdPayload = (m_flags.optModeImmediate == 1) ? scratchMem : (pCmdSpace + PM4_CMD_SET_DATA_DWORDS);

    for (uint32 e = 0; e < entryCount; ++e)
    {
        const uint32 currRegAddr = entryMap.regAddr[e + firstEntry];
        if (currRegAddr != UserDataNotMapped)
        {
            pCmdPayload[count] = pEntryValues[e];

            PAL_ASSERT((endRegAddr == 0) || (endRegAddr == (currRegAddr - 1)));
            endRegAddr = currRegAddr;
            ++count;
        }
    }

    PAL_ASSERT(count <= MaxUserDataEntries);

    if (count >= 1)
    {
        // If we copied any registers at all to the output buffer, we need to assemble the correct packet for setting
        // a group of sequential SPI user-data registers.

        if (m_flags.optModeImmediate == 1)
        {
            PM4CMDSETDATA setData;

            m_cmdUtil.BuildSetSeqShRegs((endRegAddr - count + 1), endRegAddr, shaderType, &setData);

            pCmdSpace = m_pPm4Optimizer->WriteOptimizedSetSeqShRegs(setData, pCmdPayload, pCmdSpace);
        }
        else
        {
            const size_t totalDwords = m_cmdUtil.BuildSetSeqShRegs((endRegAddr - count + 1),
                                                                   endRegAddr,
                                                                   shaderType,
                                                                   pCmdSpace);

            // The packet is complete and will not be optimized, fix-up pCmdSpace and we're done.
            PAL_ASSERT(totalDwords == (count + PM4_CMD_SET_DATA_DWORDS));
            pCmdSpace += totalDwords;
        }
    }

    return pCmdSpace;
}

// =====================================================================================================================
// Helper function to write one indirect user-data entry which has been remapped to a SPI user-data register. Returns a
// pointer to the next unused DWORD in pCmdSpace.
template<PM4ShaderType shaderType>
uint32* CmdStream::WriteUserDataRegisterOffset(
    const UserDataEntryMap& entryMap,
    const UserDataArgs*     pUserDataArgs,
    uint32*                 pCmdSpace)
{
    uint32 regAddr = entryMap.regAddr[pUserDataArgs->firstEntry];

    if (regAddr != UserDataNotMapped)
    {
        if (m_flags.optModeImmediate == 1)
        {
            pCmdSpace = WriteSetShRegDataOffset<shaderType, true>(regAddr, pUserDataArgs->pEntryValues[0], pCmdSpace);
        }
        else
        {
            pCmdSpace = WriteSetShRegDataOffset<shaderType, false>(regAddr, pUserDataArgs->pEntryValues[0], pCmdSpace);
        }
    }

    return pCmdSpace;
}

// Instantiate the template for the linker
template
uint32* CmdStream::WriteUserDataRegisterOffset<ShaderGraphics>(
    const UserDataEntryMap& entryMap,
    const UserDataArgs*     pUserDataArgs,
    uint32*                 pCmdSpace);
template
uint32* CmdStream::WriteUserDataRegisterOffset<ShaderCompute>(
    const UserDataEntryMap& entryMap,
    const UserDataArgs*     pUserDataArgs,
    uint32*                 pCmdSpace);

// =====================================================================================================================
// Builds a PM4 packet to set the given register unless the PM4 optimizer indicates that it is redundant.
// Returns a pointer to the next unused DWORD in pCmdSpace.
uint32* CmdStream::WriteSetVgtPrimitiveType(
    regVGT_PRIMITIVE_TYPE vgtPrimitiveType,
    uint32*               pCmdSpace)
{
    const bool   isGfx7plus  = m_device.Parent()->ChipProperties().gfxLevel >= GfxIpLevel::GfxIp7;
    const uint32 regAddr     = isGfx7plus ? mmVGT_PRIMITIVE_TYPE__CI__VI : mmVGT_PRIMITIVE_TYPE__SI;
    const size_t totalDwords = m_cmdUtil.BuildSetOneConfigReg(regAddr, pCmdSpace, SET_UCONFIG_INDEX_PRIM_TYPE);
    pCmdSpace[PM4_CMD_SET_DATA_DWORDS] = vgtPrimitiveType.u32All;

    return pCmdSpace + totalDwords;
}

// =====================================================================================================================
// If immediate mode optimizations are active, tell the optimizer to invalidate its copy of this particular SH register.
void CmdStream::NotifyIndirectShRegWrite(
    uint32 regAddr)
{
    if (m_flags.optModeImmediate == 1)
    {
        m_pPm4Optimizer->SetShRegInvalid(regAddr);
    }
}

// =====================================================================================================================
// Inserts a conditional indirect buffer packet into the specified address
size_t CmdStream::BuildCondIndirectBuffer(
    CompareFunc compareFunc,
    gpusize     compareGpuAddr,
    uint64      data,
    uint64      mask,
    uint32*     pPacket
    ) const
{
    return m_cmdUtil.BuildCondIndirectBuffer(compareFunc, compareGpuAddr, data, mask, IsConstantEngine(), pPacket);
}

// =====================================================================================================================
// Inserts an indirect buffer packet into the specified address
size_t CmdStream::BuildIndirectBuffer(
    gpusize  ibAddr,
    uint32   ibSize,
    bool     preemptionEnabled,
    bool     chain,
    uint32*  pPacket
    ) const
{
    return m_cmdUtil.BuildIndirectBuffer(ibAddr, ibSize, chain, IsConstantEngine(), preemptionEnabled, pPacket);
}

// =====================================================================================================================
// Update the address contained within indirect buffer packets associated with the current command block
void CmdStream::PatchCondIndirectBuffer(
    ChainPatch*  pPatch,
    gpusize      address,
    uint32       ibSizeDwords
    ) const
{
    PM4CMDCONDINDIRECTBUFFER* pCondIndirectBuffer = static_cast<PM4CMDCONDINDIRECTBUFFER*>(pPatch->pPacket);

    switch (pPatch->type)
    {
    case ChainPatchType::CondIndirectBufferPass:
        // The PM4 spec says that the first IB base/size are used if the conditional passes.
        pCondIndirectBuffer->ibBase1Lo = LowPart(address);
        pCondIndirectBuffer->ibBase1Hi = HighPart(address);
        pCondIndirectBuffer->ibSize1   = ibSizeDwords;
        break;

    case ChainPatchType::CondIndirectBufferFail:
        // The PM4 spec says that the second IB base/size are used if the conditional fails.
        pCondIndirectBuffer->ibBase2Lo = LowPart(address);
        pCondIndirectBuffer->ibBase2Hi = HighPart(address);
        pCondIndirectBuffer->ibSize2   = ibSizeDwords;
        break;

    default:
        // Other patch types should be handled by the base class
        PAL_ASSERT_ALWAYS();
        break;
    } // end switch
}

// =====================================================================================================================
// Simply apply the generic PM4 image optimizer to the given commands. See the comment in cmdStream.h for more details.
bool CmdStream::OptimizedCommit(
    const uint32* pSrcBuffer,
    uint32*       pDstBuffer,
    uint32*       pNumDwords)
{
    m_pPm4Optimizer->OptimizePm4Commands(pSrcBuffer, pDstBuffer, pNumDwords);

    return true;
}

// =====================================================================================================================
// Ends the final command block in the current chunk and inserts a chaining packet to chain that block to so other
// command block (perhaps in an external command stream at submit time).
void CmdStream::EndCurrentChunk(
    bool atEndOfStream)
{
    if (m_flags.optModeFinalized == 1)
    {
        // "Finalized" mode optimizations must be done now because we must know the final command size before we end the
        // current command block; otherwise we must patch chaining commands in the optimizer which will be hard.
        //
        // By accessing the chunk address and size directly, we implicitly assume that PM4 optimization will be disabled
        // whenever start placing multiple command blocks in a single command chunk.
        PAL_ASSERT(CmdBlockOffset() == 0);

        auto*const   pChunk   = m_chunkList.Back();
        uint32*const pCmdAddr = pChunk->GetRmwWriteAddr();
        uint32*const pCmdSize = pChunk->GetRmwUsedDwords();

        m_pPm4Optimizer->OptimizePm4Commands(pCmdAddr, pCmdAddr, pCmdSize);
    }

    // The body of the old command block is complete so we can end it. Our block postamble is a basic chaining packet.
    uint32*const pChainPacket = EndCommandBlock(m_chainIbSpaceInDwords, true);

    if (m_chainIbSpaceInDwords > 0)
    {
        if (atEndOfStream)
        {
            // Let the GfxCmdStream handle the special chain at the end of each command stream.
            UpdateTailChainLocation(pChainPacket);
        }
        else
        {
            // Fill the chain packet with a NOP and ask for it to be replaced with a real chain to the new chunk.
            m_cmdUtil.BuildNop(m_chainIbSpaceInDwords, pChainPacket);
            AddChainPatch(ChainPatchType::IndirectBuffer, pChainPacket);
        }
    }
}

// =====================================================================================================================
// Marks current PM4 optimizer state as invalid. This is expected to be called after nested command buffer execute.
void CmdStream::NotifyNestedCmdBufferExecute()
{
    if (m_flags.optModeImmediate == 1)
    {
        // The command buffer PM4 optimizer has no knowledge of nested command buffer state.
        // Reset PM4 optimizer state so that subsequent PM4 state does not get incorrectly optimized out.
        m_pPm4Optimizer->Reset();
    }
}

} // Gfx6
} // Pal
