/*
* Copyright (c) 2017, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     codechal_encode_tracked_buffer.cpp
//! \brief    Class to manage tracked buffer used in encoder
//!

#include "codechal_encode_tracked_buffer.h"
#include "codechal_encoder_base.h"

MOS_STATUS CodechalEncodeTrackedBuffer::AllocateForCurrFrame()
{
    CODEC_REF_LIST* currRefList = m_encoder->m_currRefList;

    // in case of resolution change, defer-deallocate remaining 3 buffers from last session
    if (m_trackedBufCountResize)
    {
        ReleaseBufferOnResChange();
        m_trackedBufCountResize--;
    }

    // update the last 3 buffer index, find a new slot for current frame
    m_trackedBufAnteIdx = m_trackedBufPenuIdx;
    m_trackedBufPenuIdx = m_trackedBufCurrIdx;
    m_trackedBufCurrIdx = LookUpBufIndex(currRefList->RefList, currRefList->ucNumRef, currRefList->bUsedAsRef);

    CODECHAL_ENCODE_CHK_COND_RETURN(m_trackedBufCurrIdx >= CODEC_NUM_TRACKED_BUFFERS, "No tracked buffer is available!");

    // wait to re-use once # of non-ref slots being used reaches 3
    m_waitForTrackedBuffer = (m_trackedBufCurrIdx >= CODEC_NUM_REF_BUFFERS && m_trackedBufCountNonRef >= CODEC_NUM_NON_REF_BUFFERS);

    CODECHAL_ENCODE_NORMALMESSAGE("currFrame = %d, currRef = %d, ucNumRef = %d, usedAsRef = %d, tracked buf index = %d",
        m_encoder->m_currOriginalPic.FrameIdx, m_encoder->m_currReconstructedPic.FrameIdx,
        currRefList->ucNumRef, currRefList->bUsedAsRef, m_trackedBufCurrIdx);

    if (m_allocateMbCode)
    {
        LookUpBufIndexMbCode();
        CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateMbCodeResources(m_mbCodeCurrIdx));

        // for non-AVC codec, MbCode and MvData surface are combined, this function won't be called
        if (m_encoder->m_mvDataSize)
        {
            CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateMvDataResources(m_trackedBufCurrIdx));
        }
    }

    // allocate MV temporal buffer
    AllocateMvTemporalBuffer(m_trackedBufCurrIdx);

    // allocate VDEnc downscaled recon surface
    if (m_encoder->m_vdencEnabled)
    {
        CODECHAL_ENCODE_CHK_STATUS_RETURN(AllocateDsReconSurfacesVdenc(m_trackedBufCurrIdx));
    }

    return MOS_STATUS_SUCCESS;
}

/*
* When resolution changes, tracked buffers used by earlier submitted frames may not have finished execution,
* destruction of these in-the-fly buffers need to be deferred after execution completes
* We make a resonable assumption that the number of unfinished frames should not exceed 3, and free all other
* existing buffers except the last 3 used
* Inside LookUpBufIndex(), the counter is checked and decremented, each time freeing
* one of the remaining 3 buffers in previous encode session/sequence (with old resolution)
* The net result is that 3 frames into the new encode session/sequence, all tracked buffers have been re-allocated
* according to the new resolution
*/
void CodechalEncodeTrackedBuffer::Resize()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    // free existing allocations except last 3 slots
    m_trackedBufCountResize = CODEC_NUM_NON_REF_BUFFERS;
    for (uint8_t i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        if (m_trackedBufAnteIdx != i && m_trackedBufPenuIdx != i && m_trackedBufCurrIdx != i)
        {
            if (m_mbCodeIsTracked)
            {
                ReleaseMbCode(i);
            }
            ReleaseMvData(i);
            ReleaseDsRecon(i);
#ifndef _FULL_OPEN_SOURCE
            ReleaseSurfaceDS(i);
#endif
            // this slot can now be re-used
            m_trackedBuffer[i].ucSurfIndex7bits = PICTURE_MAX_7BITS;
        }
        else
        {
            m_trackedBuffer[i].ucSurfIndex7bits = PICTURE_RESIZE;
        }
    }

    return;
}

void CodechalEncodeTrackedBuffer::ResetUsedForCurrFrame()
{
    for (auto i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        m_trackedBuffer[i].bUsedforCurFrame = false;
    }
}

uint8_t CodechalEncodeTrackedBuffer::PreencLookUpBufIndex(
    uint8_t         frameIdx,
    bool            *inCache)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    *inCache = false;
    uint8_t j = frameIdx % CODEC_NUM_TRACKED_BUFFERS;
    uint8_t emptyEntry = CODEC_NUM_TRACKED_BUFFERS;

    for (auto i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        if (m_trackedBuffer[j].ucSurfIndex7bits == frameIdx)
        {
            //this frame is already in cache
            *inCache = true;
            m_trackedBuffer[j].bUsedforCurFrame = true;

            return emptyEntry = j;
        }
        j = (j + 1) % CODEC_NUM_TRACKED_BUFFERS;
    }

    j = frameIdx % CODEC_NUM_TRACKED_BUFFERS;
    for (auto i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        if (!m_trackedBuffer[j].bUsedforCurFrame)
        {
            //find the first empty entry
            emptyEntry = j;
            break;
        }
        j = (j + 1) % CODEC_NUM_TRACKED_BUFFERS;
    }

    if (emptyEntry < CODEC_NUM_TRACKED_BUFFERS)
    {
        m_trackedBuffer[emptyEntry].ucSurfIndex7bits = frameIdx;
        m_trackedBuffer[emptyEntry].bUsedforCurFrame = true;
    }

    return emptyEntry;
}

uint8_t CodechalEncodeTrackedBuffer::LookUpBufIndex(
    PCODEC_PICTURE refList,
    uint8_t        numRefFrame,
    bool           usedAsRef)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    uint8_t index = PICTURE_MAX_7BITS;
    if (usedAsRef && numRefFrame <= CODEC_MAX_NUM_REF_FRAME && !m_encoder->m_gopIsIdrFrameOnly)
    {
        uint8_t refPicIdx[CODEC_MAX_NUM_REF_FRAME];
        bool notFound = true;

        for (auto i = 0; i < numRefFrame; i++)
        {
            refPicIdx[i] = refList[i].FrameIdx;
        }

        // find the first empty slot to re-use
        for (uint8_t i = 0; i < CODEC_NUM_REF_BUFFERS; i++)
        {
            PCODEC_TRACKED_BUFFER trackedBuffer = &m_trackedBuffer[i];
            uint8_t refFrameIdx = trackedBuffer->ucSurfIndex7bits;

            if (refFrameIdx != PICTURE_MAX_7BITS && refFrameIdx != PICTURE_RESIZE)
            {
                // check whether this ref frame is still active
                uint8_t j = 0;
                for (j = 0; j < numRefFrame; j++)
                {
                    if (refFrameIdx == refPicIdx[j])
                        break;
                }

                if (j == numRefFrame)
                {
                    // this ref frame is no longer active, can be re-used
                    trackedBuffer->ucSurfIndex7bits = PICTURE_MAX_7BITS;
                }
            }

            if (notFound && PICTURE_MAX_7BITS == trackedBuffer->ucSurfIndex7bits)
            {
                index = i;
                notFound = false;
                continue;
            }
        }
    }
    else
    {
        if (!m_encoder->m_waitForPak)
        {
            m_trackedBufCountNonRef += m_trackedBufCountNonRef < CODEC_NUM_NON_REF_BUFFERS;
            CODECHAL_ENCODE_NORMALMESSAGE("Tracked buffer count = %d", m_trackedBufCountNonRef);
        }
        else
        {
            m_trackedBufCountNonRef = 0;
        }

        m_trackedBufNonRefIdx = (m_trackedBufNonRefIdx + 1) % CODEC_NUM_NON_REF_BUFFERS;
        index = CODEC_NUM_REF_BUFFERS + m_trackedBufNonRefIdx;
    }

    if (index < CODEC_NUM_TRACKED_BUFFERS)
    {
        m_trackedBuffer[index].ucSurfIndex7bits = m_encoder->m_currReconstructedPic.FrameIdx;
    }

    return index;
}

void CodechalEncodeTrackedBuffer::ReleaseBufferOnResChange()
{
    if ((m_trackedBufAnteIdx != m_trackedBufPenuIdx) &&
        (m_trackedBufAnteIdx != m_trackedBufCurrIdx))
    {
        ReleaseMbCode(m_trackedBufAnteIdx);
        ReleaseMvData(m_trackedBufAnteIdx);
        ReleaseDsRecon(m_trackedBufAnteIdx);
#ifndef _FULL_OPEN_SOURCE
        ReleaseSurfaceDS(m_trackedBufAnteIdx);
#endif
        m_trackedBuffer[m_trackedBufAnteIdx].ucSurfIndex7bits = PICTURE_MAX_7BITS;
        CODECHAL_ENCODE_NORMALMESSAGE("Tracked buffer = %d re-allocated", m_trackedBufAnteIdx);
    }
}

MOS_STATUS CodechalEncodeTrackedBuffer::AllocateMbCodeResources(uint8_t bufIndex)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    CODECHAL_ENCODE_CHK_COND_RETURN(
        bufIndex >= CODEC_NUM_TRACKED_BUFFERS,
        "No MbCode buffer is available!");

    // early exit if already allocated
    if (m_trackedBufCurrMbCode = (MOS_RESOURCE*)m_allocator->GetResource(m_standard, mbCodeBuffer, bufIndex))
    {
        return MOS_STATUS_SUCCESS;
    }

    // Must reserve at least 8 cachelines after MI_BATCH_BUFFER_END_CMD since HW prefetch max 8 cachelines from BB everytime
    CODECHAL_ENCODE_CHK_NULL_RETURN(m_trackedBufCurrMbCode = (MOS_RESOURCE*)m_allocator->AllocateResource(
        m_standard, m_encoder->m_mbCodeSize + 8 * CODECHAL_CACHELINE_SIZE, 1, mbCodeBuffer, bufIndex, true));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CodechalEncodeTrackedBuffer::AllocateMvDataResources(uint8_t bufIndex)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    // early exit if already allocated
    if (m_trackedBufCurrMvData = (MOS_RESOURCE*)m_allocator->GetResource(m_standard, mvDataBuffer, bufIndex))
    {
        return MOS_STATUS_SUCCESS;
    }

    CODECHAL_ENCODE_CHK_NULL_RETURN(m_trackedBufCurrMvData = (MOS_RESOURCE*)m_allocator->AllocateResource(
        m_standard, m_encoder->m_mvDataSize, 1, mvDataBuffer, bufIndex, true));

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CodechalEncodeTrackedBuffer::AllocateSurfaceDS()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto trackedBuffer = &m_trackedBuffer[m_trackedBufCurrIdx];

    if (!Mos_ResourceIsNull(&trackedBuffer->sScaled4xSurface.OsResource))
    {
        return eStatus;
    }

    // initiate allocation paramters
    MOS_ALLOC_GFXRES_PARAMS allocParamsForBufferNV12;
    MOS_ZeroMemory(&allocParamsForBufferNV12, sizeof(MOS_ALLOC_GFXRES_PARAMS));
    allocParamsForBufferNV12.Type = MOS_GFXRES_2D;
    allocParamsForBufferNV12.TileType = MOS_TILE_Y;
    allocParamsForBufferNV12.Format = Format_NV12;

    uint32_t downscaledSurfaceWidth4x, downscaledSurfaceHeight4x;
    uint32_t downscaledSurfaceWidth16x, downscaledSurfaceHeight16x;
    uint32_t downscaledSurfaceWidth32x, downscaledSurfaceHeight32x;
    if (m_encoder->m_useCommonKernel)
    {
        downscaledSurfaceWidth4x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(m_encoder->m_frameWidth);
        downscaledSurfaceHeight4x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(m_encoder->m_frameHeight);

        downscaledSurfaceWidth16x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(downscaledSurfaceWidth4x);
        downscaledSurfaceHeight16x = CODECHAL_GET_4xDS_SIZE_32ALIGNED(downscaledSurfaceHeight4x);

        downscaledSurfaceWidth32x = CODECHAL_GET_2xDS_SIZE_32ALIGNED(downscaledSurfaceWidth16x);
        downscaledSurfaceHeight32x = CODECHAL_GET_2xDS_SIZE_32ALIGNED(downscaledSurfaceHeight16x);
    }
    else
    {
        // MB-alignment not required since dataport handles out-of-bound pixel replication, but IME requires this.
        downscaledSurfaceWidth4x = m_encoder->m_downscaledWidth4x;
        // Account for field case, offset needs to be 4K aligned if tiled for DI surface state.
        // Width will be allocated tile Y aligned, so also tile align height.
        downscaledSurfaceHeight4x = ((m_encoder->m_downscaledHeight4x / CODECHAL_MACROBLOCK_HEIGHT + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
        downscaledSurfaceHeight4x = MOS_ALIGN_CEIL(downscaledSurfaceHeight4x, MOS_YTILE_H_ALIGNMENT) << 1;

        downscaledSurfaceWidth16x = m_encoder->m_downscaledWidth16x;
        downscaledSurfaceHeight16x = ((m_encoder->m_downscaledHeight16x / CODECHAL_MACROBLOCK_HEIGHT + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
        downscaledSurfaceHeight16x = MOS_ALIGN_CEIL(downscaledSurfaceHeight16x, MOS_YTILE_H_ALIGNMENT) << 1;

        downscaledSurfaceWidth32x = m_encoder->m_downscaledWidth32x;
        downscaledSurfaceHeight32x = ((m_encoder->m_downscaledHeight32x / CODECHAL_MACROBLOCK_HEIGHT + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
        downscaledSurfaceHeight32x = MOS_ALIGN_CEIL(downscaledSurfaceHeight32x, MOS_YTILE_H_ALIGNMENT) << 1;
    }

    allocParamsForBufferNV12.dwWidth = downscaledSurfaceWidth4x;
    allocParamsForBufferNV12.dwHeight = downscaledSurfaceHeight4x;
    allocParamsForBufferNV12.pBufName = "4x Scaled Surface";

    // allocate 4x DS surface
    CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
        m_osInterface,
        &allocParamsForBufferNV12,
        &trackedBuffer->sScaled4xSurface.OsResource),
        "Failed to allocate 4xScaled surface.");

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalGetResourceInfo(
        m_osInterface,
        &trackedBuffer->sScaled4xSurface));

    // allocate 16x DS surface
    
    if (m_encoder->m_16xMeSupported)
    {
        allocParamsForBufferNV12.dwWidth = downscaledSurfaceWidth16x;
        allocParamsForBufferNV12.dwHeight = downscaledSurfaceHeight16x;
        allocParamsForBufferNV12.pBufName = "16x Scaled Surface";

        CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
            m_osInterface,
            &allocParamsForBufferNV12,
            &trackedBuffer->sScaled16xSurface.OsResource),
            "Failed to allocate 16xScaled surface.");

        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalGetResourceInfo(
            m_osInterface,
            &trackedBuffer->sScaled16xSurface));
    }

    // allocate 32x DS surface
    if (m_encoder->m_32xMeSupported)
    {
        allocParamsForBufferNV12.dwWidth = downscaledSurfaceWidth32x;
        allocParamsForBufferNV12.dwHeight = downscaledSurfaceHeight32x;
        allocParamsForBufferNV12.pBufName = "32x Scaled Surface";

        CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
            m_osInterface,
            &allocParamsForBufferNV12,
            &trackedBuffer->sScaled32xSurface.OsResource),
            "Failed to allocate 32xScaled surface.");

        CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalGetResourceInfo(
            m_osInterface,
            &trackedBuffer->sScaled32xSurface));
    }

    if (!m_encoder->m_fieldScalingOutputInterleaved)
    {
        // Separated scaled surfaces
        // Height should be 4K aligned for DI surface state, assume always Y tiled
        m_encoder->m_scaledBottomFieldOffset = MOS_ALIGN_CEIL(
            (trackedBuffer->sScaled4xSurface.dwPitch *
            (trackedBuffer->sScaled4xSurface.dwHeight / 2)), CODECHAL_PAGE_SIZE);

        if (m_encoder->m_16xMeSupported)
        {
            // Height should be 4K aligned for DI surface state, assume always Y tiled
            m_encoder->m_scaled16xBottomFieldOffset = MOS_ALIGN_CEIL(
                (trackedBuffer->sScaled16xSurface.dwPitch *
                (trackedBuffer->sScaled16xSurface.dwHeight / 2)), CODECHAL_PAGE_SIZE);
        }

        if (m_encoder->m_32xMeSupported)
        {
            // Height should be 4K aligned for DI surface state, assume always Y tiled
            m_encoder->m_scaled32xBottomFieldOffset = MOS_ALIGN_CEIL(
                (trackedBuffer->sScaled32xSurface.dwPitch *
                (trackedBuffer->sScaled32xSurface.dwHeight / 2)), CODECHAL_PAGE_SIZE);
        }

    }
    else
    {
        // Interleaved scaled surfaces
        m_encoder->m_scaledBottomFieldOffset =
        m_encoder->m_scaled16xBottomFieldOffset =
        m_encoder->m_scaled32xBottomFieldOffset = 0;
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeTrackedBuffer::AllocateSurface2xDS()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    auto trackedBuffer = &m_trackedBuffer[m_trackedBufCurrIdx];

    if (!Mos_ResourceIsNull(&trackedBuffer->sScaled2xSurface.OsResource))
    {
        return eStatus;
    }

    // initiate allocation paramters
    MOS_ALLOC_GFXRES_PARAMS allocParamsForBufferNV12;
    MOS_ZeroMemory(&allocParamsForBufferNV12, sizeof(MOS_ALLOC_GFXRES_PARAMS));
    allocParamsForBufferNV12.Type = MOS_GFXRES_2D;
    allocParamsForBufferNV12.TileType = MOS_TILE_Y;
    allocParamsForBufferNV12.Format = Format_NV12;

    uint32_t surfaceWidth, surfaceHeight;
    if (m_encoder->m_useCommonKernel)
    {
        surfaceWidth = CODECHAL_GET_2xDS_SIZE_32ALIGNED(m_encoder->m_frameWidth);
        surfaceHeight = CODECHAL_GET_2xDS_SIZE_32ALIGNED(m_encoder->m_frameHeight);
    }
    else
    {
        surfaceWidth = MOS_ALIGN_CEIL(m_encoder->m_frameWidth, 64) >> 1;
        surfaceHeight = MOS_ALIGN_CEIL(m_encoder->m_frameHeight, 64) >> 1;
    }

    if ((uint8_t)HCP_CHROMA_FORMAT_YUV422 == m_encoder->m_outputChromaFormat)
    {
        allocParamsForBufferNV12.Format = Format_YUY2;
        allocParamsForBufferNV12.dwWidth = surfaceWidth >> 1;
        allocParamsForBufferNV12.dwHeight = surfaceHeight << 1;
    }
    else
    {
        allocParamsForBufferNV12.dwWidth = surfaceWidth;
        allocParamsForBufferNV12.dwHeight = surfaceHeight;
    }
    allocParamsForBufferNV12.pBufName = "2x Scaled Surface";

    // allocate 2x DS surface
    CODECHAL_ENCODE_CHK_STATUS_MESSAGE_RETURN(m_osInterface->pfnAllocateResource(
        m_osInterface,
        &allocParamsForBufferNV12,
        &trackedBuffer->sScaled2xSurface.OsResource),
        "Failed to allocate 2xScaled surface.");

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalGetResourceInfo(
        m_osInterface,
        &trackedBuffer->sScaled2xSurface));

    if ((uint8_t)HCP_CHROMA_FORMAT_YUV422 == m_encoder->m_outputChromaFormat)
    {
        trackedBuffer->sScaled2xSurface.Format = Format_YUY2V;
        trackedBuffer->sScaled2xSurface.dwWidth = surfaceWidth;
        trackedBuffer->sScaled2xSurface.dwHeight = surfaceHeight;
    }

    return eStatus;
}

MOS_STATUS CodechalEncodeTrackedBuffer::AllocateDsReconSurfacesVdenc(uint8_t bufIndex)
{
    CODECHAL_ENCODE_FUNCTION_ENTER; 
    
    // early exit if already allocated
    if (m_trackedBufCurr4xDsRecon = (MOS_SURFACE*)m_allocator->GetResource(m_standard, ds4xRecon, bufIndex))
    {
        m_trackedBufCurr8xDsRecon = (MOS_SURFACE*)m_allocator->GetResource(m_standard, ds8xRecon, bufIndex);
        return MOS_STATUS_SUCCESS;
    }

    // MB-alignment not required since dataport handles out-of-bound pixel replication, but HW IME requires this.
    uint32_t downscaledSurfaceWidth4x = m_encoder->m_downscaledWidthInMb4x * CODECHAL_MACROBLOCK_WIDTH;
    // Account for field case, offset needs to be 4K aligned if tiled for DI surface state.
    // Width will be allocated tile Y aligned, so also tile align height.
    uint32_t downscaledSurfaceHeight4x = ((m_encoder->m_downscaledHeightInMb4x + 1) >> 1) * CODECHAL_MACROBLOCK_HEIGHT;
    downscaledSurfaceHeight4x = MOS_ALIGN_CEIL(downscaledSurfaceHeight4x, MOS_YTILE_H_ALIGNMENT) << 1;

    // Allocating VDEnc 4x DsRecon surface
    CODECHAL_ENCODE_CHK_NULL_RETURN(m_trackedBufCurr4xDsRecon = (MOS_SURFACE*)m_allocator->AllocateResource(
        m_standard, downscaledSurfaceWidth4x, downscaledSurfaceHeight4x, ds4xRecon, bufIndex, false, Format_NV12, MOS_TILE_Y));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalGetResourceInfo(m_osInterface, m_trackedBufCurr4xDsRecon));

    // Allocating VDEnc 8x DsRecon surfaces
    CODECHAL_ENCODE_CHK_NULL_RETURN(m_trackedBufCurr8xDsRecon = (MOS_SURFACE*)m_allocator->AllocateResource(
        m_standard, downscaledSurfaceWidth4x >> 1, downscaledSurfaceHeight4x >> 1, ds8xRecon, bufIndex, false, Format_NV12, MOS_TILE_Y));

    CODECHAL_ENCODE_CHK_STATUS_RETURN(CodecHalGetResourceInfo(m_osInterface, m_trackedBufCurr8xDsRecon));

    return MOS_STATUS_SUCCESS;
}

void CodechalEncodeTrackedBuffer::ReleaseMbCode(uint8_t bufIndex)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    m_allocator->ReleaseResource(m_standard, mbCodeBuffer, bufIndex);
}

void CodechalEncodeTrackedBuffer::ReleaseMvData(uint8_t bufIndex)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    m_allocator->ReleaseResource(m_standard, mvDataBuffer, bufIndex);
}

void CodechalEncodeTrackedBuffer::ReleaseSurfaceDS(uint8_t bufIndex)
{
    // free DS surface
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[bufIndex].sScaled2xSurface.OsResource);
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[bufIndex].sScaled4xSurface.OsResource);
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[bufIndex].sScaled16xSurface.OsResource);
    m_osInterface->pfnFreeResource(m_osInterface, &m_trackedBuffer[bufIndex].sScaled32xSurface.OsResource);
}

void CodechalEncodeTrackedBuffer::ReleaseDsRecon(uint8_t bufIndex)
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    m_allocator->ReleaseResource(m_standard, ds4xRecon, bufIndex);
    m_allocator->ReleaseResource(m_standard, ds8xRecon, bufIndex);
}

CodechalEncodeTrackedBuffer::CodechalEncodeTrackedBuffer(CodechalEncoderState* encoder)
{
    // Initilize interface pointers
    m_encoder = encoder;
    m_allocator = encoder->m_allocator;
    m_standard = encoder->m_standard;
    m_osInterface = encoder->GetOsInterface();
    m_trackedBuffer = encoder->m_trackedBuffer;
    m_mbCodeIsTracked = true;

    for (auto i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        // Init all tracked buffer slots to usable
        MOS_ZeroMemory(&m_trackedBuffer[i], sizeof(m_trackedBuffer[i]));
        m_trackedBuffer[i].ucSurfIndex7bits = PICTURE_MAX_7BITS;
    }
}

CodechalEncodeTrackedBuffer::~CodechalEncodeTrackedBuffer()
{
    CODECHAL_ENCODE_FUNCTION_ENTER;

    for (uint8_t i = 0; i < CODEC_NUM_TRACKED_BUFFERS; i++)
    {
        ReleaseSurfaceDS(i);
    }
}