#include "VirtualFileSystem.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <streambuf>
#include <unordered_map>

#include "NoxCore/Core/Log.h"

namespace Nox::VFS
{
    namespace
    {
        constexpr char kMagic[8] = { 'N', 'O', 'X', 'P', 'A', 'K', '0', '1' };
        constexpr uint32_t kVersion = 1;

        struct Entry
        {
            uint64_t Offset = 0;
            uint64_t Size = 0;
        };

        struct Pack
        {
            std::filesystem::path Path;
            std::filesystem::path Root; // lexically normal, generic
            std::unordered_map<std::string, Entry> Entries; // relative path, generic, lowercase on Windows
            std::ifstream Stream;
            std::mutex StreamMutex;
        };

        std::shared_mutex s_MountMutex;
        std::vector<std::unique_ptr<Pack>> s_Packs;

        std::string Key(const std::filesystem::path& relative)
        {
            std::string key = relative.lexically_normal().generic_string();
#ifdef _WIN32
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            return key;
        }

        // The pack entry holding `path` (a file under a pack's root), with its pack.
        std::pair<Pack*, const Entry*> Find(const std::filesystem::path& path)
        {
            const std::filesystem::path normal = std::filesystem::absolute(path).lexically_normal();
            for (const std::unique_ptr<Pack>& pack : s_Packs)
            {
                const std::filesystem::path relative = normal.lexically_relative(pack->Root);
                if (relative.empty() || *relative.begin() == "..")
                    continue;
                auto entry = pack->Entries.find(Key(relative));
                if (entry != pack->Entries.end())
                    return { pack.get(), &entry->second };
            }
            return { nullptr, nullptr };
        }

        bool ReadEntry(Pack& pack, const Entry& entry, std::vector<char>& out)
        {
            out.resize(static_cast<size_t>(entry.Size));
            std::scoped_lock lock(pack.StreamMutex);
            pack.Stream.clear();
            pack.Stream.seekg(static_cast<std::streamoff>(entry.Offset));
            pack.Stream.read(out.data(), static_cast<std::streamsize>(entry.Size));
            return static_cast<uint64_t>(pack.Stream.gcount()) == entry.Size;
        }

        // An istream over bytes it owns, seekable (the importers seek within their files).
        class MemoryBuffer : public std::streambuf
        {
        public:
            explicit MemoryBuffer(std::vector<char>&& bytes) : m_Bytes(std::move(bytes))
            {
                char* begin = m_Bytes.data();
                setg(begin, begin, begin + m_Bytes.size());
            }

        protected:
            pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override
            {
                off_type base = 0;
                if (direction == std::ios_base::cur)
                    base = gptr() - eback();
                else if (direction == std::ios_base::end)
                    base = static_cast<off_type>(m_Bytes.size());
                return seekpos(pos_type(base + offset), std::ios_base::in);
            }
            pos_type seekpos(pos_type position, std::ios_base::openmode) override
            {
                const off_type target = static_cast<off_type>(position);
                if (target < 0 || target > static_cast<off_type>(m_Bytes.size()))
                    return pos_type(off_type(-1));
                setg(eback(), eback() + target, egptr());
                return position;
            }

        private:
            std::vector<char> m_Bytes;
        };

        class MemoryStream : public std::istream
        {
        public:
            explicit MemoryStream(std::vector<char>&& bytes) : std::istream(nullptr), m_Buffer(std::move(bytes)) { rdbuf(&m_Buffer); }

        private:
            MemoryBuffer m_Buffer;
        };

        template <typename T>
        bool ReadValue(std::istream& stream, T& value)
        {
            stream.read(reinterpret_cast<char*>(&value), sizeof(T));
            return static_cast<bool>(stream);
        }

        template <typename T>
        void WriteValue(std::ostream& stream, const T& value)
        {
            stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }
    }

    bool MountPack(const std::filesystem::path& packPath, const std::filesystem::path& root)
    {
        auto pack = std::make_unique<Pack>();
        pack->Path = packPath;
        pack->Root = std::filesystem::absolute(root).lexically_normal();
        pack->Stream.open(packPath, std::ios::binary);
        if (!pack->Stream)
        {
            NOX_CORE_ERROR("VFS: cannot open the pack '{}'", packPath.string());
            return false;
        }
        char magic[8];
        uint32_t version = 0, count = 0;
        uint64_t tocOffset = 0;
        pack->Stream.read(magic, sizeof(magic));
        if (!pack->Stream || std::memcmp(magic, kMagic, sizeof(magic)) != 0 || !ReadValue(pack->Stream, version) || version != kVersion ||
            !ReadValue(pack->Stream, count) || !ReadValue(pack->Stream, tocOffset))
        {
            NOX_CORE_ERROR("VFS: '{}' is not a pack of version {}", packPath.string(), kVersion);
            return false;
        }
        pack->Stream.seekg(static_cast<std::streamoff>(tocOffset));
        for (uint32_t index = 0; index < count; ++index)
        {
            uint32_t length = 0;
            Entry entry;
            if (!ReadValue(pack->Stream, length) || length > 4096)
                break;
            std::string path(length, '\0');
            pack->Stream.read(path.data(), length);
            if (!pack->Stream || !ReadValue(pack->Stream, entry.Offset) || !ReadValue(pack->Stream, entry.Size))
                break;
            pack->Entries.emplace(Key(path), entry);
        }
        if (pack->Entries.size() != count)
        {
            NOX_CORE_ERROR("VFS: the table of contents of '{}' is damaged ({} of {} entries)", packPath.string(), pack->Entries.size(), count);
            return false;
        }
        NOX_CORE_INFO("VFS: mounted {} ({} files) over {}", packPath.filename().string(), count, pack->Root.generic_string());
        std::unique_lock lock(s_MountMutex);
        s_Packs.push_back(std::move(pack));
        return true;
    }

    size_t MountPacks(const std::filesystem::path& directory, const std::filesystem::path& root)
    {
        std::vector<std::filesystem::path> packs;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        {
            if (entry.is_regular_file(error) && entry.path().extension() == ".npak")
                packs.push_back(entry.path());
        }
        std::sort(packs.begin(), packs.end()); // a stable order: the first pack holding a path wins
        size_t mounted = 0;
        for (const std::filesystem::path& pack : packs)
            mounted += MountPack(pack, root) ? 1 : 0;
        return mounted;
    }

    void UnmountAll()
    {
        std::unique_lock lock(s_MountMutex);
        s_Packs.clear();
    }

    bool HasMounts()
    {
        std::shared_lock lock(s_MountMutex);
        return !s_Packs.empty();
    }

    bool Exists(const std::filesystem::path& path)
    {
        std::error_code error;
        if (std::filesystem::exists(path, error))
            return true;
        std::shared_lock lock(s_MountMutex);
        return Find(path).second != nullptr;
    }

    std::optional<uint64_t> FileSize(const std::filesystem::path& path)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
        {
            const uint64_t size = std::filesystem::file_size(path, error);
            if (!error)
                return size;
        }
        std::shared_lock lock(s_MountMutex);
        if (const Entry* entry = Find(path).second)
            return entry->Size;
        return std::nullopt;
    }

    bool ReadBytes(const std::filesystem::path& path, std::vector<char>& out)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                return false;
            const std::streamsize size = stream.tellg();
            out.resize(static_cast<size_t>(std::max<std::streamsize>(size, 0)));
            stream.seekg(0);
            stream.read(out.data(), size);
            return static_cast<bool>(stream) || stream.gcount() == size;
        }
        std::shared_lock lock(s_MountMutex);
        auto [pack, entry] = Find(path);
        return pack && ReadEntry(*pack, *entry, out);
    }

    bool ReadText(const std::filesystem::path& path, std::string& out)
    {
        std::vector<char> bytes;
        if (!ReadBytes(path, bytes))
            return false;
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    std::string ReadTextOrEmpty(const std::filesystem::path& path)
    {
        std::string text;
        ReadText(path, text);
        return text;
    }

    bool ReadRange(const std::filesystem::path& path, uint64_t offset, uint64_t size, void* destination)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
        {
            std::ifstream stream(path, std::ios::binary);
            stream.seekg(static_cast<std::streamoff>(offset));
            stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
            return !stream.fail();
        }
        std::shared_lock lock(s_MountMutex);
        auto [pack, entry] = Find(path);
        if (!pack || offset + size > entry->Size)
            return false;
        std::scoped_lock streamLock(pack->StreamMutex);
        pack->Stream.clear();
        pack->Stream.seekg(static_cast<std::streamoff>(entry->Offset + offset));
        pack->Stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
        return static_cast<uint64_t>(pack->Stream.gcount()) == size;
    }

    std::unique_ptr<std::istream> Open(const std::filesystem::path& path)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
        {
            auto stream = std::make_unique<std::ifstream>(path, std::ios::binary);
            if (*stream)
                return stream;
            return nullptr;
        }
        std::vector<char> bytes;
        {
            std::shared_lock lock(s_MountMutex);
            auto [pack, entry] = Find(path);
            if (!pack || !ReadEntry(*pack, *entry, bytes))
                return nullptr;
        }
        return std::make_unique<MemoryStream>(std::move(bytes));
    }

    bool WritePack(const std::filesystem::path& packPath, const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
                   std::string& error, uint64_t* bytesWritten)
    {
        std::error_code fsError;
        std::filesystem::create_directories(packPath.parent_path(), fsError);
        std::ofstream out(packPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + packPath.generic_string();
            return false;
        }
        out.write(kMagic, sizeof(kMagic));
        WriteValue(out, kVersion);
        WriteValue(out, static_cast<uint32_t>(files.size()));
        WriteValue(out, uint64_t(0)); // the table of contents' offset, written at the end

        struct Written { std::string Path; uint64_t Offset; uint64_t Size; };
        std::vector<Written> written;
        written.reserve(files.size());
        std::vector<char> buffer;
        for (const std::filesystem::path& relative : files)
        {
            std::ifstream in(root / relative, std::ios::binary | std::ios::ate);
            if (!in)
            {
                error = "cannot read " + (root / relative).generic_string();
                return false;
            }
            const std::streamsize size = in.tellg();
            buffer.resize(static_cast<size_t>(std::max<std::streamsize>(size, 0)));
            in.seekg(0);
            in.read(buffer.data(), size);

            uint64_t offset = static_cast<uint64_t>(out.tellp());
            const uint64_t padding = (16 - offset % 16) % 16;
            static const char zeros[16] = {};
            out.write(zeros, static_cast<std::streamsize>(padding));
            offset += padding;
            out.write(buffer.data(), size);
            written.push_back({ relative.lexically_normal().generic_string(), offset, static_cast<uint64_t>(size) });
        }

        const uint64_t tocOffset = static_cast<uint64_t>(out.tellp());
        for (const Written& entry : written)
        {
            WriteValue(out, static_cast<uint32_t>(entry.Path.size()));
            out.write(entry.Path.data(), static_cast<std::streamsize>(entry.Path.size()));
            WriteValue(out, entry.Offset);
            WriteValue(out, entry.Size);
        }
        const uint64_t total = static_cast<uint64_t>(out.tellp());
        out.seekp(sizeof(kMagic) + sizeof(uint32_t) * 2);
        WriteValue(out, tocOffset);
        out.close();
        if (!out)
        {
            error = "writing " + packPath.generic_string() + " failed";
            return false;
        }
        if (bytesWritten)
            *bytesWritten = total;
        return true;
    }
}
