#pragma once

#include <stdint.h>
#include <cstring>

namespace Nox
{
    // Non-owning raw bufer class
    struct Buffer
    {
        uint8_t* Data = nullptr;
        uint64_t Size = 0;

        Buffer() = default;
        
        Buffer(uint64_t size)
        {
            Allocate(size);
        }

        Buffer(const void* data, uint64_t size) : Data((uint8_t*)data), Size(size)
        {
        }
        
        Buffer(const Buffer&) = default;

        static Buffer Copy(Buffer other)
        {
            Buffer result(other.Size);
            memcpy(result.Data, other.Data, other.Size);
            return result;
        }

        void Allocate(uint64_t size)
        {
            Release();
            
            Data = (uint8_t*)malloc(size);
            Size = size;
        }
        
        void Release()
        {
            free(Data);
            Data = nullptr;
            Size = 0;
        }

        template<typename T>
        T* As()
        {
            return (T*)Data;
        }

        operator bool() const
        {
            return (bool)Data;
        }
        
        static Buffer FromRGBToRGBA(const void* rgbData, uint64_t width, uint64_t height)
        {
            uint64_t pixelCount = width * height;
            Buffer result(pixelCount * 4); // Allocate for 4 channels

            const uint8_t* src = static_cast<const uint8_t*>(rgbData);
            uint8_t* dst = result.Data;

            for (uint64_t i = 0; i < pixelCount; ++i)
            {
                dst[i * 4 + 0] = src[i * 3 + 0]; // R
                dst[i * 4 + 1] = src[i * 3 + 1]; // G
                dst[i * 4 + 2] = src[i * 3 + 2]; // B
                dst[i * 4 + 3] = 255;            // A (Alpha)
            }
            return result;
        }
    };

    struct ScopedBuffer
    {
        ScopedBuffer(Buffer buffer) : m_Buffer(buffer)
        {
        }
        
        ScopedBuffer(uint64_t size) : m_Buffer(size)
        {
        }
        
        ~ScopedBuffer()
        {
            m_Buffer.Release();
        }

        uint8_t* Data() { return m_Buffer.Data; }
        uint64_t Size() { return m_Buffer.Size; }

        template<typename T>
        T* As()
        {
            return m_Buffer.As<T>();
        }

        operator bool() const { return m_Buffer; }
    private:
        Buffer m_Buffer;
    };
}
