#include "AssetReferenceIndex.h"

#include <fstream>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    namespace
    {
        constexpr char kMagic[4] = { 'N', 'R', 'E', 'F' };
        constexpr uint32_t kVersion = 1;

        // A file's size and write time; false when it does not exist.
        bool Stamp(const std::filesystem::path& file, uint64_t& size, int64_t& time)
        {
            std::error_code error;
            size = std::filesystem::file_size(file, error);
            if (error)
                return false;
            time = static_cast<int64_t>(std::filesystem::last_write_time(file, error).time_since_epoch().count());
            return !error;
        }

        void WriteU64(std::ofstream& out, uint64_t value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
        void WriteU32(std::ofstream& out, uint32_t value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
        void WriteText(std::ofstream& out, const std::string& text)
        {
            WriteU32(out, static_cast<uint32_t>(text.size()));
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        bool ReadU64(std::ifstream& in, uint64_t& value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))); }
        bool ReadU32(std::ifstream& in, uint32_t& value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))); }
        bool ReadText(std::ifstream& in, std::string& text)
        {
            uint32_t size = 0;
            if (!ReadU32(in, size) || size > (64u << 20))
                return false;
            text.resize(size);
            return static_cast<bool>(in.read(text.data(), size));
        }
    }

    AssetReferenceIndex::AssetReferenceIndex(std::filesystem::path assetDirectory, std::filesystem::path cacheFile, Reader reader)
        : m_AssetDirectory(std::move(assetDirectory)), m_CacheFile(std::move(cacheFile)), m_Reader(std::move(reader))
    {
        Load();
    }

    AssetReferenceIndex::~AssetReferenceIndex()
    {
        StopBuild();
        Save();
    }

    void AssetReferenceIndex::StopBuild()
    {
        m_Stop = true;
        if (m_Builder.joinable())
            m_Builder.join();
        m_Stop = false;
        m_Building = false;
    }

    void AssetReferenceIndex::StartBuild(std::vector<std::filesystem::path> relativeFiles)
    {
        StopBuild();
        m_Done = 0;
        m_Total = relativeFiles.size();
        m_Building = true;
        m_Builder = std::thread([this, files = std::move(relativeFiles)]()
        {
            const auto started = std::chrono::steady_clock::now();
            for (const std::filesystem::path& file : files)
            {
                if (m_Stop)
                    break;
                Get(file);
                ++m_Done;
            }
            if (!m_Stop)
            {
                // Files that are gone (deleted, moved) leave the cache.
                std::unordered_set<std::string> listed;
                for (const std::filesystem::path& file : files)
                    listed.insert(file.lexically_normal().generic_string());
                {
                    std::lock_guard lock(m_Mutex);
                    const size_t before = m_Entries.size();
                    std::erase_if(m_Entries, [&listed](const auto& entry) { return !listed.contains(entry.first); });
                    m_Dirty = m_Dirty || m_Entries.size() != before;
                }
                Save();
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                NOX_CORE_INFO("Asset references: {} files indexed in {:.2f} s", files.size(), seconds);
            }
            m_Building = false;
        });
    }

    std::shared_ptr<const AssetReferenceTokens> AssetReferenceIndex::Get(const std::filesystem::path& relativeFile)
    {
        const std::string key = relativeFile.lexically_normal().generic_string();
        const std::filesystem::path absolute = m_AssetDirectory / relativeFile;
        uint64_t size = 0;
        int64_t time = 0;
        if (!Stamp(absolute, size, time))
            return std::make_shared<const AssetReferenceTokens>();
        {
            std::lock_guard lock(m_Mutex);
            if (const auto found = m_Entries.find(key); found != m_Entries.end() && found->second.Size == size && found->second.Time == time)
                return found->second.Tokens;
        }
        // New or changed: read outside the lock (a scene of a large world takes a moment).
        auto tokens = std::make_shared<const AssetReferenceTokens>(m_Reader ? m_Reader(absolute) : AssetReferenceTokens{});
        std::lock_guard lock(m_Mutex);
        m_Entries[key] = { size, time, tokens };
        m_Dirty = true;
        return tokens;
    }

    AssetReferenceIndex::Progress AssetReferenceIndex::GetProgress() const
    {
        return { m_Done.load(), m_Total.load(), m_Building.load() };
    }

    void AssetReferenceIndex::Load()
    {
        std::ifstream in(m_CacheFile, std::ios::binary);
        if (!in)
            return;
        char magic[4] = {};
        uint32_t version = 0;
        uint64_t count = 0;
        if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !ReadU32(in, version) || version != kVersion || !ReadU64(in, count))
            return;
        std::unordered_map<std::string, Entry> entries;
        for (uint64_t i = 0; i < count; ++i)
        {
            std::string key;
            Entry entry;
            uint64_t time = 0;
            uint32_t handles = 0, paths = 0;
            auto tokens = std::make_shared<AssetReferenceTokens>();
            if (!ReadText(in, key) || !ReadU64(in, entry.Size) || !ReadU64(in, time) || !ReadU32(in, handles))
                return; // a broken cache is ignored: everything is read again
            entry.Time = static_cast<int64_t>(time);
            for (uint32_t h = 0; h < handles; ++h)
            {
                uint64_t handle = 0;
                if (!ReadU64(in, handle))
                    return;
                tokens->Handles.insert(AssetHandle(handle));
            }
            if (!ReadU32(in, paths))
                return;
            for (uint32_t p = 0; p < paths; ++p)
            {
                std::string path;
                if (!ReadText(in, path))
                    return;
                tokens->Paths.insert(std::move(path));
            }
            entry.Tokens = std::move(tokens);
            entries.emplace(std::move(key), std::move(entry));
        }
        std::lock_guard lock(m_Mutex);
        m_Entries = std::move(entries);
    }

    void AssetReferenceIndex::Save()
    {
        std::unordered_map<std::string, Entry> entries;
        {
            std::lock_guard lock(m_Mutex);
            if (!m_Dirty)
                return;
            entries = m_Entries;
            m_Dirty = false;
        }
        std::error_code error;
        std::filesystem::create_directories(m_CacheFile.parent_path(), error);
        const std::filesystem::path temporary = std::filesystem::path(m_CacheFile).concat(".tmp");
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out.write(kMagic, 4);
            WriteU32(out, kVersion);
            WriteU64(out, entries.size());
            for (const auto& [key, entry] : entries)
            {
                WriteText(out, key);
                WriteU64(out, entry.Size);
                WriteU64(out, static_cast<uint64_t>(entry.Time));
                WriteU32(out, static_cast<uint32_t>(entry.Tokens->Handles.size()));
                for (AssetHandle handle : entry.Tokens->Handles)
                    WriteU64(out, static_cast<uint64_t>(handle));
                WriteU32(out, static_cast<uint32_t>(entry.Tokens->Paths.size()));
                for (const std::string& path : entry.Tokens->Paths)
                    WriteText(out, path);
            }
            if (!out)
                return;
        }
        std::filesystem::rename(temporary, m_CacheFile, error);
        if (error)
        {
            std::filesystem::remove(m_CacheFile, error);
            std::filesystem::rename(temporary, m_CacheFile, error);
        }
    }
}
