#include "AudioClip.h"

#include <cstring>

#include <miniaudio.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"

namespace Nox
{
    namespace
    {
        // Unreal's decompression threshold: shorter sounds are decoded once, longer ones decoded while they play.
        constexpr double kAutoDecompressSeconds = 5.0;
        // A streamed file is read in pieces of this size (one read every few seconds of compressed music).
        constexpr uint64_t kStreamChunkBytes = 64 * 1024;

        // miniaudio reading a file through the engine's VFS, so a streamed clip plays from a loose file in the editor and from the
        // pack in a packaged game alike. One open file per decoder: a cursor over the file and the chunk read last.
        struct VfsFile
        {
            std::filesystem::path Path;
            uint64_t Size = 0;
            uint64_t Cursor = 0;
            uint64_t ChunkStart = 0;
            std::vector<uint8_t> Chunk;
        };

        ma_result VfsOpen(ma_vfs*, const char* filePath, ma_uint32 openMode, ma_vfs_file* file)
        {
            if ((openMode & MA_OPEN_MODE_WRITE) != 0)
                return MA_ACCESS_DENIED;
            const std::filesystem::path path(reinterpret_cast<const char8_t*>(filePath));
            const std::optional<uint64_t> size = VFS::FileSize(path);
            if (!size)
                return MA_DOES_NOT_EXIST;
            auto* handle = new VfsFile();
            handle->Path = path;
            handle->Size = *size;
            *file = handle;
            return MA_SUCCESS;
        }

        ma_result VfsOpenW(ma_vfs*, const wchar_t*, ma_uint32, ma_vfs_file*) { return MA_NOT_IMPLEMENTED; }

        ma_result VfsClose(ma_vfs*, ma_vfs_file file)
        {
            delete static_cast<VfsFile*>(file);
            return MA_SUCCESS;
        }

        ma_result VfsRead(ma_vfs*, ma_vfs_file file, void* destination, size_t bytes, size_t* bytesRead)
        {
            auto* handle = static_cast<VfsFile*>(file);
            auto* out = static_cast<uint8_t*>(destination);
            size_t done = 0;
            while (done < bytes && handle->Cursor < handle->Size)
            {
                const bool inChunk = handle->Cursor >= handle->ChunkStart && handle->Cursor < handle->ChunkStart + handle->Chunk.size();
                if (!inChunk)
                {
                    const uint64_t length = std::min(kStreamChunkBytes, handle->Size - handle->Cursor);
                    handle->Chunk.resize(static_cast<size_t>(length));
                    handle->ChunkStart = handle->Cursor;
                    if (!VFS::ReadRange(handle->Path, handle->Cursor, length, handle->Chunk.data()))
                    {
                        handle->Chunk.clear();
                        break;
                    }
                }
                const uint64_t offset = handle->Cursor - handle->ChunkStart;
                const size_t copy = static_cast<size_t>(std::min<uint64_t>(bytes - done, handle->Chunk.size() - offset));
                std::memcpy(out + done, handle->Chunk.data() + offset, copy);
                done += copy;
                handle->Cursor += copy;
            }
            if (bytesRead)
                *bytesRead = done;
            return done == 0 && bytes > 0 ? MA_AT_END : MA_SUCCESS;
        }

        ma_result VfsWrite(ma_vfs*, ma_vfs_file, const void*, size_t, size_t*) { return MA_ACCESS_DENIED; }

        ma_result VfsSeek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
        {
            auto* handle = static_cast<VfsFile*>(file);
            int64_t base = 0;
            if (origin == ma_seek_origin_current)
                base = static_cast<int64_t>(handle->Cursor);
            else if (origin == ma_seek_origin_end)
                base = static_cast<int64_t>(handle->Size);
            const int64_t target = base + offset;
            if (target < 0 || target > static_cast<int64_t>(handle->Size))
                return MA_BAD_SEEK;
            handle->Cursor = static_cast<uint64_t>(target);
            return MA_SUCCESS;
        }

        ma_result VfsTell(ma_vfs*, ma_vfs_file file, ma_int64* cursor)
        {
            *cursor = static_cast<ma_int64>(static_cast<VfsFile*>(file)->Cursor);
            return MA_SUCCESS;
        }

        ma_result VfsInfo(ma_vfs*, ma_vfs_file file, ma_file_info* info)
        {
            info->sizeInBytes = static_cast<VfsFile*>(file)->Size;
            return MA_SUCCESS;
        }

        ma_vfs_callbacks* EngineVfs()
        {
            static ma_vfs_callbacks callbacks = { VfsOpen, VfsOpenW, VfsClose, VfsRead, VfsWrite, VfsSeek, VfsTell, VfsInfo };
            return &callbacks;
        }

        std::string Utf8(const std::filesystem::path& path)
        {
            const std::u8string text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }

        // Floats, at the file's own rate; more than two channels downmixed to stereo (the mixer is stereo until Steam Audio's
        // ambisonics in A4).
        ma_decoder_config DecoderConfig(uint32_t channels)
        {
            return ma_decoder_config_init(ma_format_f32, channels, 0);
        }

        // An Ogg file's length in frames: the granule position of its last page (what stb_vorbis_stream_length_in_samples reads).
        // miniaudio's Vorbis decoder cannot tell the length of a file it reads through callbacks (stb_vorbis's push mode), and a
        // streamed clip is never read to its end up front. 0 when it is not an Ogg file.
        uint64_t OggLengthInFrames(const std::filesystem::path& path)
        {
            const std::optional<uint64_t> size = VFS::FileSize(path);
            if (!size || *size < 27)
                return 0;
            char magic[4] = {};
            if (!VFS::ReadRange(path, 0, 4, magic) || std::memcmp(magic, "OggS", 4) != 0)
                return 0;
            const uint64_t tail = std::min<uint64_t>(*size, 65536); // (an Ogg page is at most 65307 bytes)
            std::vector<uint8_t> bytes(static_cast<size_t>(tail));
            if (!VFS::ReadRange(path, *size - tail, tail, bytes.data()))
                return 0;
            for (size_t i = bytes.size() - 27 + 1; i-- > 0;)
            {
                if (std::memcmp(bytes.data() + i, "OggS", 4) != 0 || bytes[i + 4] != 0) // capture pattern, stream structure version 0
                    continue;
                uint64_t granule = 0;
                for (int b = 7; b >= 0; --b)
                    granule = (granule << 8) | bytes[i + 6 + b];
                if (granule != ~uint64_t(0)) // (-1: a page no packet ends on)
                    return granule;
            }
            return 0;
        }

        bool QueryFormat(ma_decoder& decoder, uint32_t& channels, uint32_t& sampleRate)
        {
            ma_format format;
            ma_uint32 decodedChannels = 0;
            ma_uint32 rate = 0;
            if (ma_decoder_get_data_format(&decoder, &format, &decodedChannels, &rate, nullptr, 0) != MA_SUCCESS || rate == 0)
                return false;
            channels = decodedChannels;
            sampleRate = rate;
            return true;
        }
    }

    Ref<AudioClip> AudioClip::Load(const std::filesystem::path& path, AudioLoadType loadType)
    {
        Ref<AudioClip> clip = CreateRef<AudioClip>();
        clip->m_Path = path;

        // The format first, from the file itself (streamed: without reading all of it).
        uint32_t channels = 0;
        uint32_t sampleRate = 0;
        uint64_t frames = 0;
        {
            ma_decoder decoder;
            ma_decoder_config config = DecoderConfig(0);
            const std::string utf8 = Utf8(path);
            if (ma_decoder_init_vfs(reinterpret_cast<ma_vfs*>(EngineVfs()), utf8.c_str(), &config, &decoder) != MA_SUCCESS)
            {
                NOX_CORE_ERROR("Audio: not a sound file miniaudio can decode: {}", path.generic_string());
                return {};
            }
            const bool format = QueryFormat(decoder, channels, sampleRate);
            ma_uint64 length = 0;
            ma_decoder_get_length_in_pcm_frames(&decoder, &length);
            ma_decoder_uninit(&decoder);
            if (!format || channels == 0)
            {
                NOX_CORE_ERROR("Audio: could not read the format of {}", path.generic_string());
                return {};
            }
            frames = length > 0 ? length : OggLengthInFrames(path);
        }

        clip->m_Channels = std::min(channels, 2u);
        clip->m_SampleRate = sampleRate;
        clip->m_FrameCount = frames;

        if (loadType == AudioLoadType::Auto)
        {
            const double seconds = static_cast<double>(frames) / sampleRate;
            // A length the decoder cannot tell up front (frames 0: some MP3s) is not assumed short.
            loadType = frames > 0 && seconds <= kAutoDecompressSeconds ? AudioLoadType::DecompressOnLoad : AudioLoadType::Streaming;
        }
        clip->m_LoadType = loadType;

        if (loadType == AudioLoadType::Streaming)
            return clip;

        std::vector<char> bytes;
        if (!VFS::ReadBytes(path, bytes))
        {
            NOX_CORE_ERROR("Audio: could not read {}", path.generic_string());
            return {};
        }

        if (loadType == AudioLoadType::CompressedInMemory)
        {
            clip->m_Encoded.assign(bytes.begin(), bytes.end());
            return clip;
        }

        // Decompress On Load: every frame now (the length can be unknown or approximate up front, so read to the end).
        ma_decoder decoder;
        ma_decoder_config config = DecoderConfig(clip->m_Channels);
        if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS)
        {
            NOX_CORE_ERROR("Audio: could not decode {}", path.generic_string());
            return {};
        }
        std::vector<float>& samples = clip->m_Samples;
        samples.reserve(static_cast<size_t>(frames * clip->m_Channels));
        constexpr ma_uint64 kPiece = 16384;
        uint64_t total = 0;
        for (;;)
        {
            samples.resize(static_cast<size_t>((total + kPiece) * clip->m_Channels));
            ma_uint64 read = 0;
            const ma_result result = ma_decoder_read_pcm_frames(&decoder, samples.data() + total * clip->m_Channels, kPiece, &read);
            total += read;
            if (read < kPiece || result != MA_SUCCESS)
                break;
        }
        ma_decoder_uninit(&decoder);
        samples.resize(static_cast<size_t>(total * clip->m_Channels));
        samples.shrink_to_fit();
        clip->m_FrameCount = total;
        if (total == 0)
        {
            NOX_CORE_ERROR("Audio: {} has no samples", path.generic_string());
            return {};
        }
        return clip;
    }

    struct AudioClipDecoder::Impl
    {
        Ref<AudioClip> Clip;
        ma_decoder Decoder{};
        bool Open = false;
    };

    AudioClipDecoder::AudioClipDecoder() : m_Impl(std::make_unique<Impl>()) {}

    AudioClipDecoder::~AudioClipDecoder()
    {
        if (m_Impl->Open)
            ma_decoder_uninit(&m_Impl->Decoder);
    }

    bool AudioClipDecoder::Open(const Ref<AudioClip>& clip)
    {
        if (m_Impl->Open || !clip)
            return false;
        ma_decoder_config config = DecoderConfig(clip->GetChannels());
        ma_result result = MA_INVALID_ARGS;
        if (clip->GetLoadType() == AudioLoadType::CompressedInMemory)
        {
            const std::vector<uint8_t>& bytes = clip->GetEncodedBytes();
            result = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &m_Impl->Decoder);
        }
        else if (clip->GetLoadType() == AudioLoadType::Streaming)
        {
            const std::string utf8 = Utf8(clip->GetPath());
            result = ma_decoder_init_vfs(reinterpret_cast<ma_vfs*>(EngineVfs()), utf8.c_str(), &config, &m_Impl->Decoder);
        }
        if (result != MA_SUCCESS)
            return false;
        m_Impl->Clip = clip;
        m_Impl->Open = true;
        return true;
    }

    uint64_t AudioClipDecoder::Read(float* out, uint64_t frames)
    {
        if (!m_Impl->Open)
            return 0;
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&m_Impl->Decoder, out, frames, &read);
        return read;
    }

    bool AudioClipDecoder::Seek(uint64_t frame)
    {
        return m_Impl->Open && ma_decoder_seek_to_pcm_frame(&m_Impl->Decoder, frame) == MA_SUCCESS;
    }
}
