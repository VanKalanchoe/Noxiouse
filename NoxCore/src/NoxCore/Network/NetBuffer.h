#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The bytes of a network message (docs/Networking_Plan_2026-11.md, NET1): written little-endian whatever the machine, read with
// every read checked against the message's end -- what arrives from the network is never trusted. A read past the end (or a
// string longer than the caller allows) marks the reader failed and returns zero / empty from then on; the session then drops
// the connection with a protocol error. NET2's replication adds bit packing and quantization on top.
namespace Nox::Net
{
    class NetWriter
    {
    public:
        NetWriter() = default;
        explicit NetWriter(size_t reserve) { m_Data.reserve(reserve); }

        void WriteU8(uint8_t value) { m_Data.push_back(value); }
        void WriteU16(uint16_t value) { WriteLittleEndian(value, 2); }
        void WriteU32(uint32_t value) { WriteLittleEndian(value, 4); }
        void WriteU64(uint64_t value) { WriteLittleEndian(value, 8); }
        void WriteBool(bool value) { WriteU8(value ? 1 : 0); }
        void WriteFloat(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            WriteU32(bits);
        }
        // 7 bits a byte, the high bit set while more follow (1 byte below 128, 5 for the largest 32-bit value).
        void WriteVarU64(uint64_t value)
        {
            while (value >= 0x80)
            {
                m_Data.push_back(static_cast<uint8_t>(value | 0x80));
                value >>= 7;
            }
            m_Data.push_back(static_cast<uint8_t>(value));
        }
        void WriteVarU32(uint32_t value) { WriteVarU64(value); }
        void WriteString(std::string_view text)
        {
            WriteVarU32(static_cast<uint32_t>(text.size()));
            WriteBytes(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
        }
        void WriteBytes(std::span<const uint8_t> bytes) { m_Data.insert(m_Data.end(), bytes.begin(), bytes.end()); }

        std::span<const uint8_t> GetData() const { return m_Data; }
        size_t GetSize() const { return m_Data.size(); }
        void Clear() { m_Data.clear(); }

    private:
        void WriteLittleEndian(uint64_t value, int bytes)
        {
            for (int index = 0; index < bytes; ++index)
                m_Data.push_back(static_cast<uint8_t>(value >> (8 * index)));
        }

        std::vector<uint8_t> m_Data;
    };

    class NetReader
    {
    public:
        explicit NetReader(std::span<const uint8_t> data) : m_Data(data) {}

        uint8_t ReadU8() { return static_cast<uint8_t>(ReadLittleEndian(1)); }
        uint16_t ReadU16() { return static_cast<uint16_t>(ReadLittleEndian(2)); }
        uint32_t ReadU32() { return static_cast<uint32_t>(ReadLittleEndian(4)); }
        uint64_t ReadU64() { return ReadLittleEndian(8); }
        bool ReadBool()
        {
            const uint8_t value = ReadU8();
            if (value > 1)
                m_Failed = true;
            return value == 1;
        }
        float ReadFloat()
        {
            const uint32_t bits = ReadU32();
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
        uint64_t ReadVarU64()
        {
            uint64_t value = 0;
            for (int shift = 0; shift < 64; shift += 7)
            {
                if (!Has(1))
                    return Fail();
                const uint8_t byte = m_Data[m_Position++];
                value |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if ((byte & 0x80) == 0)
                    return value;
            }
            return Fail(); // more than 10 bytes: not a varint this writer makes
        }
        uint32_t ReadVarU32()
        {
            const uint64_t value = ReadVarU64();
            if (value > UINT32_MAX)
                return static_cast<uint32_t>(Fail());
            return static_cast<uint32_t>(value);
        }
        // At most maxLength bytes: a longer one fails the reader (a peer cannot make the receiver allocate what it likes).
        std::string ReadString(size_t maxLength)
        {
            const uint32_t length = ReadVarU32();
            if (m_Failed || length > maxLength || !Has(length))
            {
                Fail();
                return {};
            }
            std::string text(reinterpret_cast<const char*>(m_Data.data() + m_Position), length);
            m_Position += length;
            return text;
        }
        std::span<const uint8_t> ReadBytes(size_t count)
        {
            if (m_Failed || !Has(count))
            {
                Fail();
                return {};
            }
            const std::span<const uint8_t> bytes = m_Data.subspan(m_Position, count);
            m_Position += count;
            return bytes;
        }

        bool IsFailed() const { return m_Failed; }
        size_t GetRemaining() const { return m_Failed ? 0 : m_Data.size() - m_Position; }
        bool IsAtEnd() const { return !m_Failed && m_Position == m_Data.size(); }

    private:
        bool Has(size_t count) const { return !m_Failed && m_Data.size() - m_Position >= count; }
        uint64_t Fail()
        {
            m_Failed = true;
            return 0;
        }
        uint64_t ReadLittleEndian(int bytes)
        {
            if (!Has(static_cast<size_t>(bytes)))
                return Fail();
            uint64_t value = 0;
            for (int index = 0; index < bytes; ++index)
                value |= static_cast<uint64_t>(m_Data[m_Position + index]) << (8 * index);
            m_Position += static_cast<size_t>(bytes);
            return value;
        }

        std::span<const uint8_t> m_Data;
        size_t m_Position = 0;
        bool m_Failed = false;
    };
}
