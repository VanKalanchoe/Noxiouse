#include "VoxReader.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstring>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <utility>

namespace Nox::Vox
{
    namespace
    {
        constexpr uint32_t ChunkId(const char (&id)[5])
        {
            return static_cast<uint32_t>(static_cast<uint8_t>(id[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(id[1])) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(id[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(id[3])) << 24);
        }

        std::string IdText(uint32_t id)
        {
            std::string text(4, ' ');
            for (int i = 0; i < 4; ++i)
            {
                const char c = static_cast<char>((id >> (8 * i)) & 0xFF);
                text[i] = (c >= 32 && c < 127) ? c : '?';
            }
            return text;
        }

        // A bounded little-endian reader over one chunk's content: every read checks what is left and fails once, sticky.
        class Cursor
        {
        public:
            Cursor(const uint8_t* data, size_t size) : m_Data(data), m_Size(size) {}

            bool Ok() const { return m_Ok; }
            size_t Left() const { return m_Ok ? m_Size - m_Offset : 0; }

            bool Bytes(void* out, size_t count)
            {
                if (!m_Ok || count > m_Size - m_Offset)
                {
                    m_Ok = false;
                    return false;
                }
                if (count)
                    std::memcpy(out, m_Data + m_Offset, count);
                m_Offset += count;
                return true;
            }
            bool Skip(size_t count)
            {
                if (!m_Ok || count > m_Size - m_Offset)
                {
                    m_Ok = false;
                    return false;
                }
                m_Offset += count;
                return true;
            }
            uint32_t U32()
            {
                uint8_t b[4] = {};
                if (!Bytes(b, 4))
                    return 0;
                return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
            }
            int32_t I32() { return static_cast<int32_t>(U32()); }
            float F32()
            {
                const uint32_t bits = U32();
                float value = 0.0f;
                std::memcpy(&value, &bits, 4);
                return value;
            }
            std::string String()
            {
                const uint32_t length = U32();
                std::string text;
                if (!m_Ok || length > m_Size - m_Offset)
                {
                    m_Ok = false;
                    return text;
                }
                text.assign(reinterpret_cast<const char*>(m_Data + m_Offset), length);
                m_Offset += length;
                return text;
            }
            // DICT: count, then count x (key STRING, value STRING).
            std::unordered_map<std::string, std::string> Dict()
            {
                std::unordered_map<std::string, std::string> dict;
                const uint32_t count = U32();
                // Each pair needs at least its two lengths: a count larger than that is a broken file, not an allocation.
                if (!m_Ok || count > (m_Size - m_Offset) / 8)
                {
                    m_Ok = false;
                    return dict;
                }
                for (uint32_t i = 0; i < count && m_Ok; ++i)
                {
                    std::string key = String();
                    std::string value = String();
                    if (m_Ok)
                        dict[std::move(key)] = std::move(value);
                }
                return dict;
            }

        private:
            const uint8_t* m_Data;
            size_t m_Size;
            size_t m_Offset = 0;
            bool m_Ok = true;
        };

        const std::string* Find(const std::unordered_map<std::string, std::string>& dict, const char* key)
        {
            const auto found = dict.find(key);
            return found == dict.end() ? nullptr : &found->second;
        }

        bool ParseFloat(const std::string& text, float& out)
        {
            const char* begin = text.data();
            const char* end = text.data() + text.size();
            while (begin < end && *begin == ' ')
                ++begin;
            const auto result = std::from_chars(begin, end, out);
            return result.ec == std::errc();
        }

        bool ParseInt(const char*& cursor, const char* end, int32_t& out)
        {
            while (cursor < end && *cursor == ' ')
                ++cursor;
            const auto result = std::from_chars(cursor, end, out);
            if (result.ec != std::errc())
                return false;
            cursor = result.ptr;
            return true;
        }

        bool IsSet(const std::unordered_map<std::string, std::string>& dict, const char* key)
        {
            const std::string* value = Find(dict, key);
            return value && *value == "1";
        }

        struct TransformNode
        {
            std::string Name;
            bool Hidden = false;
            int32_t Child = -1;
            int32_t Layer = -1;
            Rotation Rotate;
            std::array<int32_t, 3> Translation = { 0, 0, 0 };
        };
        struct GroupNode
        {
            std::vector<int32_t> Children;
        };
        struct ShapeNode
        {
            std::vector<int32_t> Models;
        };
        enum class NodeKind : uint8_t { Transform, Group, Shape };
        struct NodeRef
        {
            NodeKind Kind;
            size_t Index;
        };
    }

    // ---------------------------------------------------------------------------------------------------------------------------
    // Rotation

    bool Rotation::FromByte(uint8_t packed, Rotation& out)
    {
        const int first = packed & 3;
        const int second = (packed >> 2) & 3;
        if (first > 2 || second > 2 || first == second)
            return false;
        out.Column = { static_cast<int8_t>(first), static_cast<int8_t>(second), static_cast<int8_t>(3 - first - second) };
        out.Sign = { static_cast<int8_t>((packed & 0x10) ? -1 : 1), static_cast<int8_t>((packed & 0x20) ? -1 : 1),
                     static_cast<int8_t>((packed & 0x40) ? -1 : 1) };
        return true;
    }

    uint8_t Rotation::ToByte() const
    {
        uint8_t packed = static_cast<uint8_t>(Column[0] | (Column[1] << 2));
        if (Sign[0] < 0) packed |= 0x10;
        if (Sign[1] < 0) packed |= 0x20;
        if (Sign[2] < 0) packed |= 0x40;
        return packed;
    }

    int32_t Rotation::Determinant() const
    {
        // A permutation's sign (even: +1, odd: -1) times the product of the row signs.
        const int a = Column[0], b = Column[1], c = Column[2];
        const int inversions = (a > b) + (a > c) + (b > c);
        const int permutation = (inversions % 2 == 0) ? 1 : -1;
        return permutation * Sign[0] * Sign[1] * Sign[2];
    }

    Rotation Rotation::operator*(const Rotation& other) const
    {
        // (A B)[r][c] = A[r][k] B[k][c] with k = A.Column[r]: row r of the product is row A.Column[r] of B, signed.
        Rotation result;
        for (int row = 0; row < 3; ++row)
        {
            const int k = Column[row];
            result.Column[row] = other.Column[k];
            result.Sign[row] = static_cast<int8_t>(Sign[row] * other.Sign[k]);
        }
        return result;
    }

    std::array<int32_t, 3> Rotation::Apply(const std::array<int32_t, 3>& v) const
    {
        return { Sign[0] * v[Column[0]], Sign[1] * v[Column[1]], Sign[2] * v[Column[2]] };
    }

    std::array<double, 3> Rotation::Apply(const std::array<double, 3>& v) const
    {
        return { Sign[0] * v[Column[0]], Sign[1] * v[Column[1]], Sign[2] * v[Column[2]] };
    }

    // ---------------------------------------------------------------------------------------------------------------------------
    // Default palette

    const std::array<uint32_t, 256>& DefaultPalette()
    {
        static const std::array<uint32_t, 256> palette = []
        {
            std::array<uint32_t, 256> colors{};
            // Index 1..215: a 6 x 6 x 6 colour cube (each channel ff, cc, 99, 66, 33, 00), blue fastest, then green, then red, without
            // black. Index 216..255: ramps of ee dd bb aa 88 77 55 44 22 11 in red, green, blue, then grey.
            const uint32_t steps[6] = { 0xff, 0xcc, 0x99, 0x66, 0x33, 0x00 };
            size_t index = 1;
            for (uint32_t r : steps)
                for (uint32_t g : steps)
                    for (uint32_t b : steps)
                    {
                        if (r == 0 && g == 0 && b == 0)
                            continue;
                        colors[index++] = 0xff000000u | (b << 16) | (g << 8) | r;
                    }
            const uint32_t ramp[10] = { 0xee, 0xdd, 0xbb, 0xaa, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
            for (uint32_t v : ramp) colors[index++] = 0xff000000u | v;
            for (uint32_t v : ramp) colors[index++] = 0xff000000u | (v << 8);
            for (uint32_t v : ramp) colors[index++] = 0xff000000u | (v << 16);
            for (uint32_t v : ramp) colors[index++] = 0xff000000u | (v << 16) | (v << 8) | v;
            return colors;
        }();
        return palette;
    }

    // ---------------------------------------------------------------------------------------------------------------------------
    // Reading

    bool ReadVox(const uint8_t* data, size_t size, Scene& scene, std::string& error)
    {
        scene = Scene{};
        scene.Palette = DefaultPalette();
        if (!data || size < 8 || std::memcmp(data, "VOX ", 4) != 0)
        {
            error = "not a MagicaVoxel file (no \"VOX \" header)";
            return false;
        }
        Cursor header(data + 4, 4);
        scene.Version = header.U32();

        // MAIN: no content of its own (some writers put none), its children are everything.
        Cursor top(data + 8, size - 8);
        const uint32_t mainId = top.U32();
        const uint32_t mainContent = top.U32();
        const uint32_t mainChildren = top.U32();
        if (!top.Ok() || mainId != ChunkId("MAIN"))
        {
            error = "the MAIN chunk is missing";
            return false;
        }
        if (!top.Skip(mainContent) || mainChildren > top.Left())
        {
            error = "the MAIN chunk is larger than the file (truncated?)";
            return false;
        }
        const uint8_t* children = data + 8 + 12 + mainContent;
        Cursor chunks(children, mainChildren);

        std::vector<TransformNode> transforms;
        std::vector<GroupNode> groups;
        std::vector<ShapeNode> shapes;
        std::unordered_map<int32_t, NodeRef> nodes;
        std::unordered_map<int32_t, bool> hiddenLayers;
        bool pendingSize = false;
        int32_t sizeX = 0, sizeY = 0, sizeZ = 0;

        auto addNode = [&](int32_t id, NodeKind kind, size_t index) -> bool
        {
            if (!nodes.emplace(id, NodeRef{ kind, index }).second)
            {
                error = "two scene graph nodes have the id " + std::to_string(id);
                return false;
            }
            return true;
        };

        while (chunks.Left() > 0)
        {
            const uint32_t id = chunks.U32();
            const uint32_t contentSize = chunks.U32();
            const uint32_t childrenSize = chunks.U32();
            if (!chunks.Ok() || contentSize > chunks.Left() || childrenSize > chunks.Left() - contentSize)
            {
                error = "a chunk (" + IdText(id) + ") is larger than the file (truncated?)";
                return false;
            }
            std::vector<uint8_t> content(contentSize);
            chunks.Bytes(content.data(), contentSize);
            chunks.Skip(childrenSize); // only MAIN has children in the format
            Cursor in(content.data(), content.size());

            if (id == ChunkId("SIZE"))
            {
                sizeX = in.I32();
                sizeY = in.I32();
                sizeZ = in.I32();
                if (!in.Ok() || sizeX <= 0 || sizeY <= 0 || sizeZ <= 0 || sizeX > 1024 || sizeY > 1024 || sizeZ > 1024 ||
                    static_cast<uint64_t>(sizeX) * sizeY * sizeZ > (1ull << 27))
                {
                    error = "a model has an impossible size";
                    return false;
                }
                pendingSize = true;
            }
            else if (id == ChunkId("XYZI"))
            {
                if (!pendingSize)
                {
                    error = "voxel data (XYZI) without a model size (SIZE) before it";
                    return false;
                }
                pendingSize = false;
                const uint32_t count = in.U32();
                if (!in.Ok() || count > in.Left() / 4)
                {
                    error = "a model's voxel count is larger than its data";
                    return false;
                }
                Model& model = scene.Models.emplace_back();
                model.SizeX = sizeX;
                model.SizeY = sizeY;
                model.SizeZ = sizeZ;
                model.Voxels.assign(static_cast<size_t>(sizeX) * sizeY * sizeZ, 0);
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint8_t voxel[4];
                    in.Bytes(voxel, 4);
                    // Outside the size, or colour index 0 (empty): MagicaVoxel never writes either; skipped rather than refused.
                    if (voxel[0] >= sizeX || voxel[1] >= sizeY || voxel[2] >= sizeZ || voxel[3] == 0)
                        continue;
                    uint8_t& cell = model.Voxels[voxel[0] + static_cast<size_t>(sizeX) * (voxel[1] + static_cast<size_t>(sizeY) * voxel[2])];
                    if (cell == 0)
                        ++model.VoxelCount;
                    cell = voxel[3];
                }
            }
            else if (id == ChunkId("RGBA"))
            {
                if (contentSize < 256 * 4)
                {
                    error = "the palette (RGBA) is shorter than 256 colours";
                    return false;
                }
                for (int k = 0; k < 255; ++k)
                {
                    uint8_t rgba[4];
                    in.Bytes(rgba, 4);
                    scene.Palette[k + 1] = static_cast<uint32_t>(rgba[0]) | (static_cast<uint32_t>(rgba[1]) << 8) |
                                           (static_cast<uint32_t>(rgba[2]) << 16) | (static_cast<uint32_t>(rgba[3]) << 24);
                }
                scene.Palette[0] = 0;
                scene.HasPaletteChunk = true;
            }
            else if (id == ChunkId("MATL"))
            {
                const int32_t index = in.I32();
                const auto dict = in.Dict();
                if (!in.Ok())
                {
                    error = "a material (MATL) is broken";
                    return false;
                }
                if (index < 1 || index > 255)
                    continue; // 0 and 256 name no palette entry a voxel can use
                Material material;
                if (const std::string* type = Find(dict, "_type"))
                {
                    if (*type == "_metal") material.Type = MaterialType::Metal;
                    else if (*type == "_glass") material.Type = MaterialType::Glass;
                    else if (*type == "_emit") material.Type = MaterialType::Emit;
                    else if (*type == "_blend") material.Type = MaterialType::Blend;
                    else if (*type == "_media") material.Type = MaterialType::Media;
                    else if (*type == "_cloud") material.Type = MaterialType::Cloud;
                }
                float value = 0.0f;
                const bool hasRough = Find(dict, "_rough") && ParseFloat(*Find(dict, "_rough"), value);
                const float rough = hasRough ? value : 0.1f;
                // A diffuse surface is Lambertian in MagicaVoxel whatever _rough says (it writes _rough for every type).
                material.Roughness = material.Type == MaterialType::Diffuse || material.Type == MaterialType::Emit ? 1.0f : rough;
                if (material.Type == MaterialType::Metal || material.Type == MaterialType::Blend)
                {
                    const std::string* metal = Find(dict, "_metal");
                    material.Metallic = metal && ParseFloat(*metal, value) ? value : (material.Type == MaterialType::Metal ? 1.0f : 0.0f);
                }
                if (material.IsTransparent())
                {
                    const std::string* trans = Find(dict, "_trans");
                    const std::string* alpha = Find(dict, "_alpha");
                    if (trans && ParseFloat(*trans, value))
                        material.Transparency = value;
                    else if (alpha && ParseFloat(*alpha, value))
                        material.Transparency = value;
                    else
                        material.Transparency = material.Type == MaterialType::Glass ? 1.0f : 0.5f;
                }
                if (const std::string* ri = Find(dict, "_ri"); ri && ParseFloat(*ri, value) && value >= 1.0f)
                    material.IOR = value;
                else if (const std::string* ior = Find(dict, "_ior"); ior && ParseFloat(*ior, value) && value >= 0.0f)
                    material.IOR = 1.0f + value;
                if (material.Type == MaterialType::Emit)
                {
                    const std::string* emit = Find(dict, "_emit");
                    const std::string* flux = Find(dict, "_flux");
                    material.Emission = emit && ParseFloat(*emit, value) ? value : 1.0f;
                    material.Flux = flux && ParseFloat(*flux, value) ? value : 0.0f;
                }
                material.Roughness = std::clamp(material.Roughness, 0.0f, 1.0f);
                material.Metallic = std::clamp(material.Metallic, 0.0f, 1.0f);
                material.Transparency = std::clamp(material.Transparency, 0.0f, 1.0f);
                material.Emission = std::max(material.Emission, 0.0f);
                material.Flux = std::clamp(material.Flux, 0.0f, 8.0f);
                scene.Materials[index] = material;
                scene.HasMaterials = true;
            }
            else if (id == ChunkId("MATT"))
            {
                // MagicaVoxel 0.98: type 0 diffuse, 1 metal, 2 glass, 3 emissive; weight is the type's amount; then the values of
                // the properties whose bits are set, in bit order (plastic, roughness, specular, IOR, attenuation, power, glow,
                // isTotalPower -- the last one has no value).
                const int32_t index = in.I32();
                const int32_t type = in.I32();
                const float weight = in.F32();
                const uint32_t bits = in.U32();
                float properties[8] = { 0, 0.1f, 0, 0.3f, 0, 0, 0, 0 };
                for (int bit = 0; bit < 7; ++bit)
                {
                    if (bits & (1u << bit))
                        properties[bit] = in.F32();
                }
                if (!in.Ok())
                {
                    error = "a 0.98 material (MATT) is broken";
                    return false;
                }
                if (index < 1 || index > 255)
                    continue;
                Material material;
                const float amount = std::clamp(weight, 0.0f, 1.0f);
                switch (type)
                {
                case 1:
                    material.Type = MaterialType::Metal;
                    material.Metallic = amount;
                    material.Roughness = std::clamp(properties[1], 0.0f, 1.0f);
                    break;
                case 2:
                    material.Type = MaterialType::Glass;
                    material.Transparency = amount;
                    material.Roughness = std::clamp(properties[1], 0.0f, 1.0f);
                    material.IOR = 1.0f + std::max(properties[3], 0.0f);
                    break;
                case 3:
                    material.Type = MaterialType::Emit;
                    material.Emission = amount;
                    material.Flux = std::clamp(properties[5], 0.0f, 8.0f);
                    break;
                default:
                    break;
                }
                scene.Materials[index] = material;
                scene.HasMaterials = true;
            }
            else if (id == ChunkId("nTRN"))
            {
                TransformNode node;
                const int32_t nodeId = in.I32();
                const auto attributes = in.Dict();
                node.Child = in.I32();
                in.I32(); // reserved (-1)
                node.Layer = in.I32();
                const uint32_t frames = in.U32();
                if (!in.Ok() || frames > in.Left() / 4)
                {
                    error = "a transform node (nTRN) is broken";
                    return false;
                }
                // Frame 0 is the model's place; later frames are MagicaVoxel's animation (not imported).
                for (uint32_t frame = 0; frame < frames && in.Ok(); ++frame)
                {
                    const auto values = in.Dict();
                    if (frame != 0 || !in.Ok())
                        continue;
                    if (const std::string* r = Find(values, "_r"))
                    {
                        int32_t packed = 0;
                        const char* cursor = r->data();
                        if (!ParseInt(cursor, r->data() + r->size(), packed) || packed < 0 || packed > 127 ||
                            !Rotation::FromByte(static_cast<uint8_t>(packed), node.Rotate))
                        {
                            error = "a transform node has an invalid rotation (" + *r + ")";
                            return false;
                        }
                    }
                    if (const std::string* t = Find(values, "_t"))
                    {
                        const char* cursor = t->data();
                        const char* end = t->data() + t->size();
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            if (!ParseInt(cursor, end, node.Translation[axis]))
                            {
                                error = "a transform node has an invalid translation (" + *t + ")";
                                return false;
                            }
                        }
                    }
                }
                if (!in.Ok())
                {
                    error = "a transform node's frames are broken";
                    return false;
                }
                if (const std::string* nodeName = Find(attributes, "_name"))
                    node.Name = *nodeName;
                node.Hidden = IsSet(attributes, "_hidden");
                if (!addNode(nodeId, NodeKind::Transform, transforms.size()))
                    return false;
                transforms.push_back(std::move(node));
            }
            else if (id == ChunkId("nGRP"))
            {
                GroupNode node;
                const int32_t nodeId = in.I32();
                in.Dict();
                const uint32_t count = in.U32();
                if (!in.Ok() || count > in.Left() / 4)
                {
                    error = "a group node (nGRP) is broken";
                    return false;
                }
                node.Children.resize(count);
                for (uint32_t i = 0; i < count; ++i)
                    node.Children[i] = in.I32();
                if (!addNode(nodeId, NodeKind::Group, groups.size()))
                    return false;
                groups.push_back(std::move(node));
            }
            else if (id == ChunkId("nSHP"))
            {
                ShapeNode node;
                const int32_t nodeId = in.I32();
                in.Dict();
                const uint32_t count = in.U32();
                if (!in.Ok() || count > in.Left() / 8)
                {
                    error = "a shape node (nSHP) is broken";
                    return false;
                }
                // Several models in one shape are its animation frames (_f; none = frame 0): the import places frame 0's.
                int32_t bestFrame = INT32_MAX;
                for (uint32_t i = 0; i < count && in.Ok(); ++i)
                {
                    const int32_t model = in.I32();
                    const auto values = in.Dict();
                    int32_t frame = 0;
                    if (const std::string* f = Find(values, "_f"))
                    {
                        const char* cursor = f->data();
                        if (!ParseInt(cursor, f->data() + f->size(), frame))
                            frame = 0;
                    }
                    if (in.Ok() && frame < bestFrame)
                    {
                        bestFrame = frame;
                        node.Models.assign(1, model);
                    }
                }
                if (!in.Ok())
                {
                    error = "a shape node's models are broken";
                    return false;
                }
                if (!addNode(nodeId, NodeKind::Shape, shapes.size()))
                    return false;
                shapes.push_back(std::move(node));
            }
            else if (id == ChunkId("LAYR"))
            {
                const int32_t layer = in.I32();
                const auto attributes = in.Dict();
                if (!in.Ok())
                {
                    error = "a layer (LAYR) is broken";
                    return false;
                }
                hiddenLayers[layer] = IsSet(attributes, "_hidden");
            }
            // PACK, rOBJ, rCAM, NOTE, IMAP, ... carry nothing the import uses.
        }

        if (!chunks.Ok())
        {
            error = "the file ends inside a chunk header";
            return false;
        }
        if (scene.Models.empty())
        {
            error = "the file has no models";
            return false;
        }

        // The scene graph from node 0 (a transform), flattened. Without one (before MagicaVoxel 0.99) several models are the frames of
        // an animation: the first one at the origin unrotated.
        const auto root = nodes.find(0);
        if (root == nodes.end())
        {
            Instance& instance = scene.Instances.emplace_back();
            instance.ModelIndex = 0;
            return true;
        }
        scene.HasSceneGraph = true;

        struct Frame
        {
            Rotation Rotate;
            std::array<int32_t, 3> Translation;
        };
        bool broken = false;
        std::function<void(int32_t, const Frame&, const std::string&, int32_t, int)> visit =
            [&](int32_t nodeId, const Frame& parent, const std::string& name, int32_t layer, int depth)
        {
            if (broken)
                return;
            // A cycle or an absurd depth is a broken file (MagicaVoxel writes transform -> group / shape alternately, a few deep).
            if (depth > 256)
            {
                error = "the scene graph is too deep (a cycle?)";
                broken = true;
                return;
            }
            const auto found = nodes.find(nodeId);
            if (found == nodes.end())
            {
                error = "the scene graph names a missing node (" + std::to_string(nodeId) + ")";
                broken = true;
                return;
            }
            switch (found->second.Kind)
            {
            case NodeKind::Transform:
            {
                const TransformNode& node = transforms[found->second.Index];
                if (node.Hidden)
                    return;
                const int32_t nodeLayer = node.Layer >= 0 ? node.Layer : layer;
                if (const auto hidden = hiddenLayers.find(nodeLayer); hidden != hiddenLayers.end() && hidden->second)
                    return;
                // world = parent.R (R v + t) + parent.t
                Frame frame;
                frame.Rotate = parent.Rotate * node.Rotate;
                const std::array<int32_t, 3> moved = parent.Rotate.Apply(node.Translation);
                for (int axis = 0; axis < 3; ++axis)
                    frame.Translation[axis] = moved[axis] + parent.Translation[axis];
                visit(node.Child, frame, node.Name.empty() ? name : node.Name, nodeLayer, depth + 1);
                break;
            }
            case NodeKind::Group:
                for (int32_t child : groups[found->second.Index].Children)
                    visit(child, parent, name, layer, depth + 1);
                break;
            case NodeKind::Shape:
                for (int32_t model : shapes[found->second.Index].Models)
                {
                    if (model < 0 || static_cast<size_t>(model) >= scene.Models.size())
                    {
                        error = "a shape names a missing model (" + std::to_string(model) + ")";
                        broken = true;
                        return;
                    }
                    Instance& instance = scene.Instances.emplace_back();
                    instance.Name = name;
                    instance.ModelIndex = static_cast<uint32_t>(model);
                    instance.Rotate = parent.Rotate;
                    instance.Translation = parent.Translation;
                    instance.Layer = layer;
                }
                break;
            }
        };
        visit(0, Frame{ Rotation::Identity(), { 0, 0, 0 } }, std::string(), -1, 0);
        return !broken;
    }

    bool ReadVoxFile(const std::filesystem::path& path, Scene& scene, std::string& error)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            error = "cannot open " + path.string();
            return false;
        }
        const std::streamoff size = file.tellg();
        if (size < 0 || size > (std::streamoff(1) << 31))
        {
            error = "cannot read " + path.string();
            return false;
        }
        std::vector<uint8_t> data(static_cast<size_t>(size));
        file.seekg(0);
        if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size))
        {
            error = "cannot read " + path.string();
            return false;
        }
        return ReadVox(data.data(), data.size(), scene, error);
    }
}
