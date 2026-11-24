// NoxGpu.slang -- single-source Vulkan & Direct3D 12 shader helpers for Noxiouse (built on NoxRHI).
//
// NoxRHI's Slang compiler defines NOX_VULKAN for SPIR-V and NOX_DX12 for DXIL.
// All API differences live in this header so engine shaders stay single-source with no #ifdefs.

#pragma once

#ifdef NOX_VULKAN
#define NOX_DRAW_ID_PARAM , uint _noxDrawId : SV_DrawIndex
#define NOX_DRAW_ID (_noxDrawId)
#define NOX_NOINLINE [noinline]
#else
// Root parameter 1 (b1, space0): hardware drawID populated by ExecuteIndirect for drawMeshTasksIndirectCountWithDrawId.
cbuffer NoxIndirectDrawId : register(b1, space0)
{
    uint g_noxDrawId;
};
#define NOX_DRAW_ID_PARAM
#define NOX_DRAW_ID (g_noxDrawId)
// DXIL forbids non-inlined functions with struct/vector/resource parameters in non-library targets.
#define NOX_NOINLINE
#endif

#ifndef NOX_VULKAN
// Wrapping T in a struct ensures DXC's ByteAddressBuffer.Load/Store applies column-major struct matrix layout
// to bare matrices (like BufferRef<float4x4> for bone matrices), matching Vulkan and struct-embedded mat4 fields.
struct _NoxBox<T>
{
    T value;
};
#endif

// Read-only typed buffer reference: device address (BDA) on Vulkan, raw SRV slot on D3D12.
struct BufferRef<T>
{
    uint64_t handle;

    __init(uint64_t h) { handle = h; }

    bool isValid() { return handle != 0; }

    T Load(uint index)
    {
#ifdef NOX_VULKAN
        return ((T*)handle)[index];
#else
        return ByteAddressBuffer.Handle(uint2(uint(handle), 0)).Load<_NoxBox<T>>(uint(handle >> 32) + index * sizeof(T)).value;
#endif
    }

    __subscript(uint index)->T { get { return Load(index); } }
};

// Read-write typed buffer reference: device address (BDA) on Vulkan, raw UAV slot on D3D12.
struct RWBufferRef<T>
{
    uint64_t handle;

    __init(uint64_t h) { handle = h; }

    bool isValid() { return handle != 0; }

    T Load(uint index)
    {
#ifdef NOX_VULKAN
        return ((T*)handle)[index];
#else
        return RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).Load<_NoxBox<T>>(uint(handle >> 32) + index * sizeof(T)).value;
#endif
    }

    void Store(uint index, T value)
    {
#ifdef NOX_VULKAN
        ((T*)handle)[index] = value;
#else
        _NoxBox<T> box;
        box.value = value;
        RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).Store<_NoxBox<T>>(uint(handle >> 32) + index * sizeof(T), box);
#endif
    }

    __subscript(uint index)->T
    {
        get { return Load(index); }
        set { Store(index, newValue); }
    }
};

// Raw byte-addressed read/write helpers for heterogeneous or sub-struct access.
T LoadRaw<T>(uint64_t handle, uint byteOffset)
{
#ifdef NOX_VULKAN
    return *((T*)(handle + uint64_t(byteOffset)));
#else
    return ByteAddressBuffer.Handle(uint2(uint(handle), 0)).Load<_NoxBox<T>>(uint(handle >> 32) + byteOffset).value;
#endif
}

T LoadRWRaw<T>(uint64_t handle, uint byteOffset)
{
#ifdef NOX_VULKAN
    return *((T*)(handle + uint64_t(byteOffset)));
#else
    return RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).Load<_NoxBox<T>>(uint(handle >> 32) + byteOffset).value;
#endif
}

void StoreRWRaw<T>(uint64_t handle, uint byteOffset, T value)
{
#ifdef NOX_VULKAN
    *((T*)(handle + uint64_t(byteOffset))) = value;
#else
    _NoxBox<T> box;
    box.value = value;
    RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).Store<_NoxBox<T>>(uint(handle >> 32) + byteOffset, box);
#endif
}

// Atomics on a uint buffer (by element index or raw byte offset).
uint AtomicAdd(RWBufferRef<uint> buffer, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedAdd(((uint*)buffer.handle)[index], value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedAdd(uint(buffer.handle >> 32) + index * 4u, value, previous);
#endif
    return previous;
}

uint AtomicAddRaw(uint64_t handle, uint byteOffset, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedAdd(*((uint*)(handle + uint64_t(byteOffset))), value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).InterlockedAdd(uint(handle >> 32) + byteOffset, value, previous);
#endif
    return previous;
}

uint AtomicMin(RWBufferRef<uint> buffer, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedMin(((uint*)buffer.handle)[index], value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedMin(uint(buffer.handle >> 32) + index * 4u, value, previous);
#endif
    return previous;
}

uint AtomicMax(RWBufferRef<uint> buffer, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedMax(((uint*)buffer.handle)[index], value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedMax(uint(buffer.handle >> 32) + index * 4u, value, previous);
#endif
    return previous;
}

uint AtomicCompareExchange(RWBufferRef<uint> buffer, uint index, uint compare, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedCompareExchange(((uint*)buffer.handle)[index], compare, value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedCompareExchange(uint(buffer.handle >> 32) + index * 4u, compare, value, previous);
#endif
    return previous;
}

uint AtomicExchange(RWBufferRef<uint> buffer, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedExchange(((uint*)buffer.handle)[index], value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedExchange(uint(buffer.handle >> 32) + index * 4u, value, previous);
#endif
    return previous;
}

uint AtomicOr(RWBufferRef<uint> buffer, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedOr(((uint*)buffer.handle)[index], value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedOr(uint(buffer.handle >> 32) + index * 4u, value, previous);
#endif
    return previous;
}

// One byte of a buffer of packed bytes (DXIL has no uint8_t: four bytes per word, little-endian).
uint LoadByte(BufferRef<uint> words, uint byteIndex)
{
    return (words.Load(byteIndex >> 2u) >> ((byteIndex & 3u) * 8u)) & 0xFFu;
}

uint LoadByteRaw(uint64_t handle, uint byteIndex)
{
    return LoadByte(BufferRef<uint>(handle), byteIndex);
}

// Top-level acceleration structure for ray queries and ray tracing.
RaytracingAccelerationStructure LoadTlas(uint64_t handle)
{
#ifdef NOX_VULKAN
    return RaytracingAccelerationStructure(handle);
#else
    return RaytracingAccelerationStructure.Handle(uint2(uint(handle), 0));
#endif
}

// Samples a texture whose slot may differ between lanes of a wave (per-instance or per-pixel material textures).
float4 SampleNonUniform(uint slot, SamplerState samplerState, float2 uv)
{
#ifdef NOX_VULKAN
    const float2 dx = ddx(uv), dy = ddy(uv);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    for (;;)
    {
        const uint first = WaveReadLaneFirst(slot);
        if (first == slot)
        {
            result = Texture2D.Handle(uint2(first, 0)).SampleGrad(samplerState, uv, dx, dy);
            break;
        }
    }
    return result;
#else
    return Texture2D.Handle(uint2(NonUniformResourceIndex(slot), 0)).Sample(samplerState, uv);
#endif
}

// Bindless textures and samplers (identical on both APIs).
Texture2D Tex2D(uint slot) { return Texture2D.Handle(uint2(slot, 0)); }
RWTexture2D<float4> RWTex2D(uint slot) { return RWTexture2D<float4>.Handle(uint2(slot, 0)); }
SamplerState Sampler(uint slot) { return SamplerState.Handle(uint2(slot, 0)); }

// Fullscreen triangle clip-space position with top-left UV origin (0,0) on both Vulkan (Y-down NDC) and D3D12 (Y-up NDC).
float4 noxFullscreenTriangle(uint vertexID, out float2 uv)
{
    uv = float2((vertexID == 1) ? 2.0 : 0.0, (vertexID == 2) ? 2.0 : 0.0);
#ifdef NOX_VULKAN
    return float4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
#else
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
#endif
}
