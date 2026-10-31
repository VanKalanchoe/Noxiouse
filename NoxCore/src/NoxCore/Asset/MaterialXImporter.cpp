#include "MaterialXImporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <stb_image.h>
#include <stb_image_write.h>

// MaterialXImporter.h has the overview. The pieces, in order: a small XML reader; the document model (nodes, node graphs, node
// definitions, as MaterialX's own Document holds them); per-texel evaluation of a node graph (the standard library's nodes Nox needs,
// and any node whose definition is a node graph -- glTF's gltf_image, the translation graphs -- by expanding that graph); the
// shaders' mapping onto glTF's material through the translation graphs; the bake of what varies over the surface.

namespace Nox
{
    namespace
    {
        // ---- XML --------------------------------------------------------------------------------------------------------------

        struct XmlElement
        {
            std::string Tag;
            std::vector<std::pair<std::string, std::string>> Attributes;
            std::vector<std::unique_ptr<XmlElement>> Children;

            std::string Get(const char* name, const std::string& fallback = std::string()) const
            {
                for (const auto& [key, value] : Attributes)
                    if (key == name)
                        return value;
                return fallback;
            }
            bool Has(const char* name) const
            {
                for (const auto& attribute : Attributes)
                    if (attribute.first == name)
                        return true;
                return false;
            }
        };

        class XmlReader
        {
        public:
            explicit XmlReader(const std::string& text) : m_Text(text) {}

            bool Read(XmlElement& root, std::string& error)
            {
                for (;;)
                {
                    SkipSpace();
                    if (m_Pos >= m_Text.size())
                        return Fail("no root element", error);
                    if (StartsWith("<?"))
                        SkipPast("?>");
                    else if (StartsWith("<!--"))
                        SkipPast("-->");
                    else if (StartsWith("<!"))
                        SkipPast(">");
                    else if (m_Text[m_Pos] == '<')
                        return Element(root, error);
                    else
                        return Fail("text before the root element", error);
                }
            }

        private:
            bool Fail(const char* what, std::string& error)
            {
                size_t line = 1 + static_cast<size_t>(std::count(m_Text.begin(), m_Text.begin() + std::min(m_Pos, m_Text.size()), '\n'));
                error = std::string("XML: ") + what + " (line " + std::to_string(line) + ")";
                return false;
            }
            bool StartsWith(const char* token) const { return m_Text.compare(m_Pos, std::strlen(token), token) == 0; }
            void SkipPast(const char* token)
            {
                const size_t found = m_Text.find(token, m_Pos);
                m_Pos = found == std::string::npos ? m_Text.size() : found + std::strlen(token);
            }
            void SkipSpace()
            {
                while (m_Pos < m_Text.size() && std::isspace(static_cast<unsigned char>(m_Text[m_Pos])))
                    ++m_Pos;
            }
            std::string Name()
            {
                const size_t start = m_Pos;
                while (m_Pos < m_Text.size() && (std::isalnum(static_cast<unsigned char>(m_Text[m_Pos])) || std::strchr("_:.-", m_Text[m_Pos])))
                    ++m_Pos;
                return m_Text.substr(start, m_Pos - start);
            }
            static std::string Decode(const std::string& raw)
            {
                std::string out;
                out.reserve(raw.size());
                for (size_t i = 0; i < raw.size(); ++i)
                {
                    if (raw[i] != '&')
                    {
                        out += raw[i];
                        continue;
                    }
                    const size_t end = raw.find(';', i);
                    if (end == std::string::npos)
                    {
                        out += raw[i];
                        continue;
                    }
                    const std::string entity = raw.substr(i + 1, end - i - 1);
                    if (entity == "lt") out += '<';
                    else if (entity == "gt") out += '>';
                    else if (entity == "amp") out += '&';
                    else if (entity == "quot") out += '"';
                    else if (entity == "apos") out += '\'';
                    else if (!entity.empty() && entity[0] == '#')
                    {
                        const unsigned long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                                                       ? std::strtoul(entity.c_str() + 2, nullptr, 16)
                                                       : std::strtoul(entity.c_str() + 1, nullptr, 10);
                        out += code < 128 ? static_cast<char>(code) : '?';
                    }
                    i = end;
                }
                return out;
            }
            bool Element(XmlElement& element, std::string& error)
            {
                ++m_Pos; // '<'
                element.Tag = Name();
                if (element.Tag.empty())
                    return Fail("element without a name", error);
                for (;;)
                {
                    SkipSpace();
                    if (m_Pos >= m_Text.size())
                        return Fail("unterminated element", error);
                    if (StartsWith("/>"))
                    {
                        m_Pos += 2;
                        return true;
                    }
                    if (m_Text[m_Pos] == '>')
                    {
                        ++m_Pos;
                        break;
                    }
                    const std::string key = Name();
                    SkipSpace();
                    if (key.empty() || m_Pos >= m_Text.size() || m_Text[m_Pos] != '=')
                        return Fail("malformed attribute", error);
                    ++m_Pos;
                    SkipSpace();
                    if (m_Pos >= m_Text.size() || (m_Text[m_Pos] != '"' && m_Text[m_Pos] != '\''))
                        return Fail("unquoted attribute", error);
                    const char quote = m_Text[m_Pos++];
                    const size_t end = m_Text.find(quote, m_Pos);
                    if (end == std::string::npos)
                        return Fail("unterminated attribute", error);
                    element.Attributes.emplace_back(key, Decode(m_Text.substr(m_Pos, end - m_Pos)));
                    m_Pos = end + 1;
                }
                for (;;)
                {
                    const size_t next = m_Text.find('<', m_Pos);
                    if (next == std::string::npos)
                        return Fail("unterminated element", error);
                    m_Pos = next;
                    if (StartsWith("</"))
                    {
                        m_Pos += 2;
                        Name();
                        SkipPast(">");
                        return true;
                    }
                    if (StartsWith("<!--"))
                        SkipPast("-->");
                    else if (StartsWith("<![CDATA["))
                        SkipPast("]]>");
                    else if (StartsWith("<?"))
                        SkipPast("?>");
                    else
                    {
                        auto child = std::make_unique<XmlElement>();
                        if (!Element(*child, error))
                            return false;
                        element.Children.push_back(std::move(child));
                    }
                }
            }

            const std::string& m_Text;
            size_t m_Pos = 0;
        };

        // ---- Document model ---------------------------------------------------------------------------------------------------

        // A value of any MaterialX numeric type: `n` components (float / integer / boolean 1, vector2 2, color3 / vector3 3, color4 /
        // vector4 4). A scalar is broadcast in operations with vectors.
        struct Value
        {
            float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            int n = 1;

            float operator[](int i) const { return n == 1 ? v[0] : v[std::min(i, n - 1)]; }
            static Value Scalar(float x) { Value r; r.v[0] = x; r.n = 1; return r; }
            static Value Vec(int count, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f)
            {
                Value r;
                r.n = count;
                r.v[0] = x; r.v[1] = y; r.v[2] = z; r.v[3] = w;
                return r;
            }
            static Value Broadcast(int count, float x) { return Vec(count, x, x, x, x); }
        };

        int ComponentsOf(const std::string& type)
        {
            if (type == "vector2") return 2;
            if (type == "color3" || type == "vector3") return 3;
            if (type == "color4" || type == "vector4") return 4;
            return 1;
        }

        Value ParseValue(const std::string& text, const std::string& type)
        {
            Value value;
            value.n = ComponentsOf(type);
            if (type == "boolean")
            {
                value.v[0] = (text == "true" || text == "1") ? 1.0f : 0.0f;
                return value;
            }
            std::stringstream stream(text);
            std::string item;
            int i = 0;
            while (i < 4 && std::getline(stream, item, ','))
                value.v[i++] = std::strtof(item.c_str(), nullptr);
            if (i == 1) // a single number given for a vector: every component
                for (int c = 1; c < 4; ++c)
                    value.v[c] = value.v[0];
            return value;
        }

        // A node category as a number (FNV-1a): the evaluation compares numbers per texel, not strings.
        constexpr uint32_t CategoryHash(const char* text)
        {
            uint32_t hash = 2166136261u;
            for (; *text; ++text)
                hash = (hash ^ uint32_t(uint8_t(*text))) * 16777619u;
            return hash;
        }
        uint32_t CategoryHash(const std::string& text) { return CategoryHash(text.c_str()); }

        struct Node;
        struct Graph;
        struct Document;
        struct Scope;
        struct Source;

        struct Port
        {
            std::string Name, Type, ValueText, NodeName, NodeGraph, Output, InterfaceName, Colorspace, FilePrefix;
            bool HasValue = false;
            Value Parsed;
            const Node* OwnerNode = nullptr;
            const Graph* OwnerGraph = nullptr;
            const Node* Target = nullptr; // NodeName resolved in its own graph (FixOwners)
            uint32_t NameHash = 0;        // CategoryHash(Name)
            int Components = 1;           // ComponentsOf(Type)
            // The interface binding found for it in its scope (Converter::Prepare, before the bake's threads only read it).
            mutable const Scope* BoundScope = nullptr;
            mutable const Source* Bound = nullptr;

            bool Connected() const { return !NodeName.empty() || !NodeGraph.empty() || !InterfaceName.empty(); }
        };

        struct Node
        {
            std::string Category, Name, Type, NodeDef, Colorspace, FilePrefix;
            std::vector<Port> Inputs;
            const Graph* Owner = nullptr;
            int Id = -1;
            uint32_t Hash = 0;    // CategoryHash(Category)
            bool Builtin = false; // evaluated here (else expanded from its definition's node graph)
            int Components = 1;   // ComponentsOf(Type)
            // An image node's image and address modes (Converter::PrepareNode; the bake's threads only read them).
            mutable const struct Image* SampledImage = nullptr;
            mutable int AddressU = 0, AddressV = 0;

            const Port* Input(const char* name) const
            {
                const uint32_t hash = CategoryHash(name);
                for (const Port& port : Inputs)
                    if (port.NameHash == hash && port.Name == name)
                        return &port;
                return nullptr;
            }
            const Port* Input(const std::string& name) const { return Input(name.c_str()); }
        };

        struct Graph
        {
            std::string Name, NodeDef, Colorspace, FilePrefix;
            std::unordered_map<std::string, std::unique_ptr<Node>> Nodes;
            std::vector<Port> Interface;
            std::vector<Port> Outputs;
            const Document* Doc = nullptr;

            const Node* FindNode(const std::string& name) const
            {
                const auto found = Nodes.find(name);
                return found == Nodes.end() ? nullptr : found->second.get();
            }
            const Port* InterfacePort(const std::string& name) const
            {
                for (const Port& port : Interface)
                    if (port.Name == name)
                        return &port;
                return nullptr;
            }
            const Port* OutputPort(const std::string& name) const
            {
                if (name.empty())
                    return Outputs.empty() ? nullptr : &Outputs.front();
                for (const Port& port : Outputs)
                    if (port.Name == name)
                        return &port;
                return nullptr;
            }
        };

        struct NodeDefinition
        {
            std::string Name, Node, Type;
            std::string Inherit;    // a definition it extends (its inputs and outputs unless overridden: InheritDefinitions)
            bool IsDefault = false; // isdefaultversion: the one a node without a version uses
            std::vector<Port> Inputs;
            std::vector<std::string> OutputNames;

            const Port* Input(const std::string& name) const
            {
                for (const Port& port : Inputs)
                    if (port.Name == name)
                        return &port;
                return nullptr;
            }
        };

        struct Document
        {
            std::filesystem::path Directory;
            std::unique_ptr<Graph> Root;
            std::unordered_map<std::string, std::unique_ptr<Graph>> Graphs;
            std::map<std::string, NodeDefinition> NodeDefs;
        };

        Port ReadPort(const XmlElement& element)
        {
            Port port;
            port.Name = element.Get("name");
            port.Type = element.Get("type");
            port.NodeName = element.Get("nodename");
            port.NodeGraph = element.Get("nodegraph");
            port.Output = element.Get("output");
            port.InterfaceName = element.Get("interfacename");
            port.Colorspace = element.Get("colorspace");
            port.FilePrefix = element.Get("fileprefix");
            if (element.Has("value"))
            {
                port.ValueText = element.Get("value");
                port.HasValue = true;
                port.Parsed = ParseValue(port.ValueText, port.Type);
            }
            return port;
        }

        bool IsPortTag(const std::string& tag) { return tag == "input" || tag == "token" || tag == "parameter"; }

        std::unique_ptr<Node> ReadNode(const XmlElement& element, const Graph* owner)
        {
            auto node = std::make_unique<Node>();
            node->Category = element.Tag;
            node->Name = element.Get("name");
            node->Type = element.Get("type");
            node->NodeDef = element.Get("nodedef");
            node->Colorspace = element.Get("colorspace");
            node->FilePrefix = element.Get("fileprefix");
            node->Owner = owner;
            for (const auto& child : element.Children)
                if (IsPortTag(child->Tag))
                    node->Inputs.push_back(ReadPort(*child));
            return node;
        }

        bool IsBuiltinCategory(const std::string& category)
        {
            static const std::set<std::string> builtins = {
                "constant", "image", "tiledimage", "texcoord", "place2d", "rotate2d", "add", "subtract", "multiply", "divide", "modulo",
                "power", "invert", "absval", "floor", "ceil", "round", "sign", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan2", "exp",
                "ln", "normalize", "magnitude", "dotproduct", "crossproduct", "clamp", "min", "max", "mix", "remap", "range", "smoothstep",
                "luminance", "contrast", "extract", "convert", "combine2", "combine3", "combine4", "separate2", "separate3", "separate4",
                "swizzle", "ifgreater", "ifgreatereq", "ifequal", "dot", "roughness_anisotropy", "normalmap", "noise2d", "noise3d",
                "fractal2d", "fractal3d", "cellnoise2d", "cellnoise3d", "worleynoise2d", "worleynoise3d", "ramplr", "ramptb", "ramp4",
                "splitlr", "splittb", "checkerboard", "position", "normal", "tangent", "bitangent", "geomcolor", "geompropvalue", "time",
                "frame", "hsvtorgb", "rgbtohsv", "hextiledimage", "safepower", "transformpoint", "transformvector", "transformnormal",
                "and", "or", "not", "creatematrix", "determinant", "transpose", "minus", "plus", "difference", "burn", "dodge", "screen",
                "overlay", "unpremult", "premult", "disjointover", "in", "mask", "matte", "out", "over", "inside", "outside" };
            return builtins.contains(category);
        }

        // The nodes and graphs of a document, owners filled in; every node gets a global id (the evaluation's memo index).
        void FixOwners(Graph& graph, int& nextId)
        {
            auto target = [&graph](const Port& port) -> const Node* {
                if (port.NodeName.empty())
                    return nullptr;
                const auto found = graph.Nodes.find(port.NodeName);
                return found == graph.Nodes.end() ? nullptr : found->second.get();
            };
            for (auto& [name, node] : graph.Nodes)
            {
                node->Owner = &graph;
                node->Id = nextId++;
                node->Hash = CategoryHash(node->Category);
                node->Builtin = IsBuiltinCategory(node->Category);
                node->Components = ComponentsOf(node->Type);
                for (Port& port : node->Inputs)
                {
                    port.NameHash = CategoryHash(port.Name);
                    port.Components = ComponentsOf(port.Type);
                    port.OwnerNode = node.get();
                    port.OwnerGraph = &graph;
                    port.Target = target(port);
                    port.BoundScope = nullptr;
                    port.Bound = nullptr;
                }
            }
            for (Port& port : graph.Interface)
            {
                port.OwnerGraph = &graph;
                port.Target = nullptr; // (a node graph's interface port names a node of the document's top level)
            }
            for (Port& port : graph.Outputs)
            {
                port.OwnerGraph = &graph;
                port.Target = target(port);
                port.BoundScope = nullptr;
                port.Bound = nullptr;
            }
        }

        bool LoadDocument(const std::filesystem::path& path, Document& document, int& nextId, std::string& error, int depth = 0)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "cannot open " + path.generic_string();
                return false;
            }
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            XmlElement root;
            if (!XmlReader(text).Read(root, error))
            {
                error = path.filename().generic_string() + ": " + error;
                return false;
            }
            if (root.Tag != "materialx")
            {
                error = path.filename().generic_string() + ": not a MaterialX document";
                return false;
            }
            if (!document.Root)
            {
                document.Directory = path.parent_path();
                document.Root = std::make_unique<Graph>();
                document.Root->Colorspace = root.Get("colorspace");
                document.Root->FilePrefix = root.Get("fileprefix");
                document.Root->Doc = &document;
            }
            Graph& top = *document.Root;
            // An included document's own file prefix and color space hold for its elements; its prefix is relative to its own folder,
            // file names resolve from the top document's (the look example's brass and grey sphere included from elsewhere).
            std::string includedPrefix, includedColorspace;
            if (depth > 0)
            {
                std::error_code relativeError;
                std::string folder = std::filesystem::relative(path.parent_path(), document.Directory, relativeError).generic_string();
                if (relativeError || folder == ".")
                    folder.clear();
                includedPrefix = root.Get("fileprefix");
                if (!folder.empty())
                    includedPrefix = folder + "/" + includedPrefix;
                includedColorspace = root.Get("colorspace");
            }
            auto inherit = [&](std::string& prefix, std::string& colorspace) {
                if (prefix.empty())
                    prefix = includedPrefix;
                if (colorspace.empty())
                    colorspace = includedColorspace;
            };
            for (const auto& child : root.Children)
            {
                const XmlElement& element = *child;
                if (element.Tag == "xi:include")
                {
                    if (depth < 8 && element.Has("href"))
                    {
                        std::string ignored;
                        LoadDocument(path.parent_path() / element.Get("href"), document, nextId, ignored, depth + 1);
                    }
                }
                else if (element.Tag == "nodedef")
                {
                    NodeDefinition definition;
                    definition.Name = element.Get("name");
                    definition.Node = element.Get("node");
                    definition.Type = element.Get("type");
                    definition.Inherit = element.Get("inherit");
                    definition.IsDefault = element.Get("isdefaultversion") == "true";
                    for (const auto& item : element.Children)
                    {
                        if (IsPortTag(item->Tag))
                            definition.Inputs.push_back(ReadPort(*item));
                        else if (item->Tag == "output")
                        {
                            definition.OutputNames.push_back(item->Get("name"));
                            if (definition.Type.empty() || definition.Type == "multioutput")
                                definition.Type = item->Get("type");
                        }
                    }
                    document.NodeDefs[definition.Name] = std::move(definition);
                }
                else if (element.Tag == "nodegraph")
                {
                    auto graph = std::make_unique<Graph>();
                    graph->Name = element.Get("name");
                    graph->NodeDef = element.Get("nodedef");
                    graph->Colorspace = element.Get("colorspace");
                    graph->FilePrefix = element.Get("fileprefix");
                    graph->Doc = &document;
                    inherit(graph->FilePrefix, graph->Colorspace);
                    for (const auto& item : element.Children)
                    {
                        if (IsPortTag(item->Tag))
                            graph->Interface.push_back(ReadPort(*item));
                        else if (item->Tag == "output")
                            graph->Outputs.push_back(ReadPort(*item));
                        else if (item->Has("name"))
                            graph->Nodes[item->Get("name")] = ReadNode(*item, graph.get());
                    }
                    FixOwners(*graph, nextId);
                    document.Graphs[graph->Name] = std::move(graph);
                }
                else if (element.Tag == "look" || element.Tag == "materialassign" || element.Tag == "collection" || element.Tag == "geominfo" ||
                         element.Tag == "typedef" || element.Tag == "implementation" || element.Tag == "unittypedef" || element.Tag == "unitdef" ||
                         element.Tag == "output" || element.Tag == "backdrop" || element.Tag == "propertyset" || element.Tag == "variantset")
                {
                    // Not materials.
                }
                else if (element.Has("name"))
                {
                    auto node = ReadNode(element, &top);
                    inherit(node->FilePrefix, node->Colorspace);
                    top.Nodes[element.Get("name")] = std::move(node);
                }
            }
            if (depth == 0)
            {
                // The root's nodes (ids assigned once every include is in).
                FixOwners(top, nextId);
            }
            return true;
        }

        // Definitions that extend another (Standard Surface 1.0.1 overrides base / base_color of 1.0.0 and inherits the rest, its
        // output type included): the inherited inputs and type filled in. Without it 1.0.0's defaults were taken (base 0.8: every
        // Standard Surface that leaves base unset 20% too dark).
        void InheritDefinitions(std::map<std::string, NodeDefinition>& definitions)
        {
            for (int pass = 0; pass < 4; ++pass)
                for (auto& [name, definition] : definitions)
                {
                    if (definition.Inherit.empty())
                        continue;
                    const auto parent = definitions.find(definition.Inherit);
                    if (parent == definitions.end() || &parent->second == &definition)
                        continue;
                    if (definition.Type.empty())
                        definition.Type = parent->second.Type;
                    if (definition.OutputNames.empty())
                        definition.OutputNames = parent->second.OutputNames;
                    for (const Port& input : parent->second.Inputs)
                    {
                        const bool overridden = std::any_of(definition.Inputs.begin(), definition.Inputs.end(),
                                                            [&](const Port& own) { return own.Name == input.Name; });
                        if (!overridden)
                            definition.Inputs.push_back(input);
                    }
                }
        }

        // ---- Images -----------------------------------------------------------------------------------------------------------

        float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
        float LinearToSrgb(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        struct Image
        {
            int Width = 0, Height = 0;
            std::vector<float> Texels; // RGBA, linear
            std::filesystem::path Path;
        };

        enum class AddressMode { Periodic, Clamp, Mirror, Constant };

        AddressMode ParseAddressMode(const std::string& text)
        {
            if (text == "clamp") return AddressMode::Clamp;
            if (text == "mirror") return AddressMode::Mirror;
            if (text == "constant") return AddressMode::Constant;
            return AddressMode::Periodic;
        }

        // Bilinear, texel centres at half-integers; `s`, `t` from the image's top-left corner (glTF's orientation).
        bool SampleImage(const Image& image, float s, float t, AddressMode modeU, AddressMode modeV, float out[4])
        {
            auto address = [](int i, int size, AddressMode mode, bool& outside) -> int {
                if (i >= 0 && i < size)
                    return i;
                switch (mode)
                {
                case AddressMode::Clamp: return std::clamp(i, 0, size - 1);
                case AddressMode::Mirror:
                {
                    const int period = 2 * size;
                    int m = ((i % period) + period) % period;
                    return m < size ? m : period - 1 - m;
                }
                case AddressMode::Constant: outside = true; return 0;
                default: return ((i % size) + size) % size;
                }
            };
            const float x = s * float(image.Width) - 0.5f;
            const float y = t * float(image.Height) - 0.5f;
            const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
            const float fx = x - float(x0), fy = y - float(y0);
            bool outside = false;
            const int xs[2] = { address(x0, image.Width, modeU, outside), address(x0 + 1, image.Width, modeU, outside) };
            const int ys[2] = { address(y0, image.Height, modeV, outside), address(y0 + 1, image.Height, modeV, outside) };
            if (outside && (modeU == AddressMode::Constant || modeV == AddressMode::Constant))
            {
                if ((s < 0.0f || s > 1.0f) && modeU == AddressMode::Constant) return false;
                if ((t < 0.0f || t > 1.0f) && modeV == AddressMode::Constant) return false;
            }
            for (int c = 0; c < 4; ++c)
            {
                auto texel = [&](int ix, int iy) { return image.Texels[(size_t(ys[iy]) * image.Width + xs[ix]) * 4 + c]; };
                out[c] = (texel(0, 0) * (1.0f - fx) + texel(1, 0) * fx) * (1.0f - fy) + (texel(0, 1) * (1.0f - fx) + texel(1, 1) * fx) * fy;
            }
            return true;
        }

        // ---- Noise: MaterialX's (mx_noise: Perlin with Bob Jenkins' hash, as OSL) ------------------------------------------------

        uint32_t Rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
        uint32_t JenkinsFinal(uint32_t a, uint32_t b, uint32_t c)
        {
            c ^= b; c -= Rotl(b, 14);
            a ^= c; a -= Rotl(c, 11);
            b ^= a; b -= Rotl(a, 25);
            c ^= b; c -= Rotl(b, 16);
            a ^= c; a -= Rotl(c, 4);
            b ^= a; b -= Rotl(a, 14);
            c ^= b; c -= Rotl(b, 24);
            return c;
        }
        uint32_t Hash2(int x, int y, uint32_t seed = 0)
        {
            uint32_t a, b, c;
            a = b = c = 0xdeadbeefu + (2u << 2) + 13u + seed;
            a += uint32_t(x);
            b += uint32_t(y);
            return JenkinsFinal(a, b, c);
        }
        uint32_t Hash3(int x, int y, int z, uint32_t seed = 0)
        {
            uint32_t a, b, c;
            a = b = c = 0xdeadbeefu + (3u << 2) + 13u + seed;
            a += uint32_t(x);
            b += uint32_t(y);
            c += uint32_t(z);
            return JenkinsFinal(a, b, c);
        }
        float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
        float Gradient2(uint32_t h, float x, float y)
        {
            h &= 7u;
            const float u = h < 4 ? x : y;
            const float v = 2.0f * (h < 4 ? y : x);
            return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
        }
        float Gradient3(uint32_t h, float x, float y, float z)
        {
            h &= 15u;
            const float u = h < 8 ? x : y;
            const float v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
            return ((h & 1u) ? -u : u) + ((h & 2u) ? -v : v);
        }
        float Perlin2(float px, float py, uint32_t seed = 0)
        {
            const int X = int(std::floor(px)), Y = int(std::floor(py));
            const float fx = px - float(X), fy = py - float(Y);
            const float u = Fade(fx), v = Fade(fy);
            const float g00 = Gradient2(Hash2(X, Y, seed), fx, fy), g10 = Gradient2(Hash2(X + 1, Y, seed), fx - 1.0f, fy);
            const float g01 = Gradient2(Hash2(X, Y + 1, seed), fx, fy - 1.0f), g11 = Gradient2(Hash2(X + 1, Y + 1, seed), fx - 1.0f, fy - 1.0f);
            return 0.6616f * ((1.0f - v) * (g00 * (1.0f - u) + g10 * u) + v * (g01 * (1.0f - u) + g11 * u));
        }
        float Perlin3(float px, float py, float pz, uint32_t seed = 0)
        {
            const int X = int(std::floor(px)), Y = int(std::floor(py)), Z = int(std::floor(pz));
            const float fx = px - float(X), fy = py - float(Y), fz = pz - float(Z);
            const float u = Fade(fx), v = Fade(fy), w = Fade(fz);
            float corners[8];
            for (int i = 0; i < 8; ++i)
            {
                const int dx = i & 1, dy = (i >> 1) & 1, dz = (i >> 2) & 1;
                corners[i] = Gradient3(Hash3(X + dx, Y + dy, Z + dz, seed), fx - float(dx), fy - float(dy), fz - float(dz));
            }
            auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
            const float x00 = lerp(corners[0], corners[1], u), x10 = lerp(corners[2], corners[3], u);
            const float x01 = lerp(corners[4], corners[5], u), x11 = lerp(corners[6], corners[7], u);
            return 0.9820f * lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
        }
        float CellNoise2(float px, float py) { return float(Hash2(int(std::floor(px)), int(std::floor(py)))) / 4294967295.0f; }
        float CellNoise3(float px, float py, float pz) { return float(Hash3(int(std::floor(px)), int(std::floor(py)), int(std::floor(pz)))) / 4294967295.0f; }
        // Worley (F1, Euclidean): the distance to the nearest of the jittered cell points around.
        float Worley2(float px, float py, float jitter)
        {
            const int X = int(std::floor(px)), Y = int(std::floor(py));
            float best = 1e30f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const uint32_t h = Hash2(X + dx, Y + dy);
                    const float ox = (float(h & 0xFFFFu) / 65535.0f - 0.5f) * jitter + 0.5f;
                    const float oy = (float(h >> 16) / 65535.0f - 0.5f) * jitter + 0.5f;
                    const float ddx = float(X + dx) + ox - px, ddy = float(Y + dy) + oy - py;
                    best = std::min(best, ddx * ddx + ddy * ddy);
                }
            return std::sqrt(best);
        }

        // ---- Evaluation -------------------------------------------------------------------------------------------------------

        struct Scope;
        struct Source
        {
            const Port* port = nullptr;
            const Scope* scope = nullptr;
            bool Valid() const { return port != nullptr; }
        };

        // Where names resolve while evaluating a graph: its nodes, its interface (bound to the caller's ports for a graph that
        // implements a node or translates a shader), and the parent scope (the document's top level, for a node graph's interface
        // port connected to a top-level node).
        struct Scope
        {
            const Graph* graph = nullptr;
            std::unordered_map<std::string, Source> bindings;
            const Scope* parent = nullptr;
        };

        struct EvalState
        {
            float u = 0.5f, v = 0.5f; // the mesh UV (glTF's orientation: v down the image)
            std::vector<uint32_t> stamp;
            std::vector<Value> memo;
            uint32_t current = 1;
            int depth = 0;
        };

        struct FileRef
        {
            std::filesystem::path Path;
            bool Srgb = false;
        };

        const std::string& InheritedColorspace(const Port& port)
        {
            if (!port.Colorspace.empty()) return port.Colorspace;
            if (port.OwnerNode && !port.OwnerNode->Colorspace.empty()) return port.OwnerNode->Colorspace;
            if (port.OwnerGraph && !port.OwnerGraph->Colorspace.empty()) return port.OwnerGraph->Colorspace;
            static const std::string none;
            return port.OwnerGraph && port.OwnerGraph->Doc && port.OwnerGraph->Doc->Root ? port.OwnerGraph->Doc->Root->Colorspace : none;
        }
        std::string InheritedFilePrefix(const Port& port)
        {
            if (!port.FilePrefix.empty()) return port.FilePrefix;
            if (port.OwnerNode && !port.OwnerNode->FilePrefix.empty()) return port.OwnerNode->FilePrefix;
            if (port.OwnerGraph && !port.OwnerGraph->FilePrefix.empty()) return port.OwnerGraph->FilePrefix;
            return port.OwnerGraph && port.OwnerGraph->Doc && port.OwnerGraph->Doc->Root ? port.OwnerGraph->Doc->Root->FilePrefix : std::string();
        }

        // Converts one document's materials: the evaluation of node graphs, the translation, the bake.
        class Converter
        {
        public:
            Converter(const Document& user, const Document& definitions, int& nextId) : m_User(user), m_Definitions(definitions), m_NextId(nextId)
            {
                m_UserRoot.graph = user.Root.get();
                m_DefinitionsRoot.graph = definitions.Root.get();
            }

            std::vector<std::string>* Warnings = nullptr;

            void Warn(const std::string& message)
            {
                if (Warnings && m_Warned.insert(message).second)
                    Warnings->push_back(message);
            }

            // ---- Lookups

            const Graph* FindGraph(const std::string& name) const
            {
                for (const Document* document : { &m_User, &m_Definitions })
                {
                    const auto found = document->Graphs.find(name);
                    if (found != document->Graphs.end())
                        return found->second.get();
                }
                return nullptr;
            }
            const Graph* FindGraphImplementing(const std::string& nodeDef) const
            {
                for (const Document* document : { &m_User, &m_Definitions })
                    for (const auto& [name, graph] : document->Graphs)
                        if (graph->NodeDef == nodeDef)
                            return graph.get();
                return nullptr;
            }
            const NodeDefinition* FindNodeDef(const std::string& name) const
            {
                for (const Document* document : { &m_User, &m_Definitions })
                {
                    const auto found = document->NodeDefs.find(name);
                    if (found != document->NodeDefs.end())
                        return &found->second;
                }
                return nullptr;
            }
            // A node's definition: the one it names, else the first for its category and output type, else the first for its category.
            const NodeDefinition* FindNodeDefFor(const Node& node) const
            {
                if (!node.NodeDef.empty())
                    if (const NodeDefinition* named = FindNodeDef(node.NodeDef))
                        return named;
                const NodeDefinition* typeMatch = nullptr;
                const NodeDefinition* categoryMatch = nullptr;
                for (const Document* document : { &m_User, &m_Definitions })
                    for (const auto& [name, definition] : document->NodeDefs)
                    {
                        if (definition.Node != node.Category)
                            continue;
                        if (definition.Type == node.Type && (definition.IsDefault || !typeMatch))
                        {
                            typeMatch = &definition;
                            if (definition.IsDefault)
                                return typeMatch;
                        }
                        if (!categoryMatch)
                            categoryMatch = &definition;
                    }
                return typeMatch ? typeMatch : categoryMatch;
            }

            Scope& NewScope(const Graph* graph, const Scope* parent)
            {
                Scope& scope = m_Scopes.emplace_back();
                scope.graph = graph;
                scope.parent = parent;
                return scope;
            }

            const Scope& GraphScope(const Graph* graph)
            {
                const auto found = m_GraphScopes.find(graph);
                if (found != m_GraphScopes.end())
                    return *found->second;
                Scope& scope = NewScope(graph, graph->Doc == &m_User ? &m_UserRoot : &m_DefinitionsRoot);
                m_GraphScopes[graph] = &scope;
                return scope;
            }

            // A copy of a graph with its own node ids: each use of a node implemented by a graph evaluates (and memoizes) its own.
            const Graph* CloneGraph(const Graph& source)
            {
                auto clone = std::make_unique<Graph>();
                clone->Name = source.Name;
                clone->NodeDef = source.NodeDef;
                clone->Colorspace = source.Colorspace;
                clone->FilePrefix = source.FilePrefix;
                clone->Doc = source.Doc;
                clone->Interface = source.Interface;
                clone->Outputs = source.Outputs;
                for (const auto& [name, node] : source.Nodes)
                    clone->Nodes[name] = std::make_unique<Node>(*node);
                FixOwners(*clone, m_NextId);
                m_Clones.push_back(std::move(clone));
                return m_Clones.back().get();
            }

            // The scope of the graph implementing a custom node (a nodedef with a node graph): its interface bound to the node's ports,
            // else to the definition's defaults. Made during the analysis (Prepare), only looked up while baking (threads).
            const Scope* Instance(const Node& node, const Scope& caller, bool create)
            {
                const auto key = std::make_pair(&node, &caller);
                const auto found = m_Instances.find(key);
                if (found != m_Instances.end())
                    return found->second;
                if (!create)
                    return nullptr;
                const NodeDefinition* definition = FindNodeDefFor(node);
                const Graph* implementation = definition ? FindGraphImplementing(definition->Name) : nullptr;
                if (!implementation)
                {
                    m_Instances[key] = nullptr;
                    return nullptr;
                }
                Scope& scope = NewScope(CloneGraph(*implementation), implementation->Doc == &m_User ? &m_UserRoot : &m_DefinitionsRoot);
                for (const Port& input : definition->Inputs)
                {
                    if (const Port* given = node.Input(input.Name))
                        scope.bindings[input.Name] = Source{ given, &caller };
                    else
                        scope.bindings[input.Name] = Source{ &input, &m_DefinitionsRoot };
                }
                m_Instances[key] = &scope;
                return &scope;
            }

            // ---- Following connections (no evaluation)

            // Where a port's value comes from: an interface binding followed to the caller's port, a node graph output to its node.
            // Returns the node producing it (and its scope and output name), or none for a plain value.
            struct NodeRef
            {
                const Node* node = nullptr;
                const Scope* scope = nullptr;
                std::string output;
            };
            NodeRef FollowToNode(const Port* port, const Scope* scope, bool create, int guard = 0)
            {
                while (port && scope && guard++ < 64)
                {
                    if (!port->InterfaceName.empty())
                    {
                        const auto bound = scope->bindings.find(port->InterfaceName);
                        if (bound != scope->bindings.end())
                        {
                            port = bound->second.port;
                            scope = bound->second.scope;
                            continue;
                        }
                        const Port* interfacePort = scope->graph->InterfacePort(port->InterfaceName);
                        if (!interfacePort || interfacePort == port)
                            return {};
                        port = interfacePort;
                        if (interfacePort->Connected())
                            scope = scope->parent ? scope->parent : scope;
                        continue;
                    }
                    if (!port->NodeName.empty())
                    {
                        const Node* node = scope->graph->FindNode(port->NodeName);
                        if (!node && scope->parent)
                            node = scope->parent->graph->FindNode(port->NodeName);
                        if (!node)
                            return {};
                        // Through a node implemented by a graph to the node behind its output.
                        if (!IsBuiltin(node->Category))
                        {
                            const Scope* instance = Instance(*node, *scope, create);
                            if (instance)
                            {
                                const Port* output = instance->graph->OutputPort(port->Output);
                                if (!output)
                                    return {};
                                port = output;
                                scope = instance;
                                continue;
                            }
                        }
                        if (node->Category == "dot") // a pass-through
                        {
                            port = node->Input("in");
                            continue;
                        }
                        return NodeRef{ node, scope, port->Output };
                    }
                    if (!port->NodeGraph.empty())
                    {
                        const Graph* graph = FindGraph(port->NodeGraph);
                        if (!graph)
                            return {};
                        scope = &GraphScope(graph);
                        port = graph->OutputPort(port->Output);
                        continue;
                    }
                    return {};
                }
                return {};
            }

            // A file name through interface bindings, with the color space and file prefix where its value is given.
            bool ResolveFile(const Port* port, const Scope* scope, FileRef& out)
            {
                for (int guard = 0; port && scope && guard < 64; ++guard)
                {
                    if (!port->InterfaceName.empty())
                    {
                        const auto bound = scope->bindings.find(port->InterfaceName);
                        if (bound != scope->bindings.end())
                        {
                            port = bound->second.port;
                            scope = bound->second.scope;
                            continue;
                        }
                        const Port* interfacePort = scope->graph->InterfacePort(port->InterfaceName);
                        if (!interfacePort || interfacePort == port)
                            return false;
                        port = interfacePort;
                        continue;
                    }
                    if (!port->HasValue || port->ValueText.empty())
                        return false;
                    std::string name = InheritedFilePrefix(*port) + port->ValueText;
                    if (name.find("<UDIM>") != std::string::npos || name.find("<UVTILE>") != std::string::npos)
                    {
                        Warn("UDIM / UV tile images are not supported: " + name);
                        return false;
                    }
                    const Document* document = port->OwnerGraph ? port->OwnerGraph->Doc : &m_User;
                    out.Path = (document ? document->Directory : m_User.Directory) / name;
                    const std::string& colorspace = InheritedColorspace(*port);
                    out.Srgb = colorspace == "srgb_texture" || colorspace == "g22_rec709" || colorspace == "g18_rec709" ||
                               colorspace == "srgb_displayp3";
                    return true;
                }
                return false;
            }

            const Image* LoadImage(const FileRef& file)
            {
                const std::string key = file.Path.lexically_normal().generic_string() + (file.Srgb ? "|srgb" : "|linear");
                std::lock_guard<std::mutex> lock(m_ImageMutex);
                const auto found = m_Images.find(key);
                if (found != m_Images.end())
                    return found->second.get();
                auto image = std::make_unique<Image>();
                image->Path = file.Path;
                int width = 0, height = 0, channels = 0;
                const std::string path = file.Path.string();
                if (stbi_is_hdr(path.c_str()))
                {
                    float* pixels = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
                    if (pixels)
                    {
                        image->Texels.assign(pixels, pixels + size_t(width) * height * 4);
                        stbi_image_free(pixels);
                    }
                }
                else if (stbi_is_16_bit(path.c_str()))
                {
                    stbi_us* pixels = stbi_load_16(path.c_str(), &width, &height, &channels, 4);
                    if (pixels)
                    {
                        image->Texels.resize(size_t(width) * height * 4);
                        for (size_t i = 0; i < image->Texels.size(); ++i)
                        {
                            const float value = float(pixels[i]) / 65535.0f;
                            image->Texels[i] = (file.Srgb && (i % 4) != 3) ? SrgbToLinear(value) : value;
                        }
                        stbi_image_free(pixels);
                    }
                }
                else
                {
                    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
                    if (pixels)
                    {
                        image->Texels.resize(size_t(width) * height * 4);
                        for (size_t i = 0; i < image->Texels.size(); ++i)
                        {
                            const float value = float(pixels[i]) / 255.0f;
                            image->Texels[i] = (file.Srgb && (i % 4) != 3) ? SrgbToLinear(value) : value;
                        }
                        stbi_image_free(pixels);
                    }
                }
                if (image->Texels.empty())
                {
                    Warn("cannot read image " + file.Path.generic_string() + " (its default value is used)");
                    m_Images[key] = nullptr;
                    return nullptr;
                }
                image->Width = width;
                image->Height = height;
                m_Images[key] = std::move(image);
                return m_Images[key].get();
            }

            // ---- Built-in nodes

            static bool IsBuiltin(const std::string& category) { return IsBuiltinCategory(category); }

            // A port's value at the state's texel.
            Value EvalPort(const Port& port, const Scope& scope, EvalState& state)
            {
                if (!port.InterfaceName.empty())
                {
                    const Source* binding = port.BoundScope == &scope ? port.Bound : nullptr;
                    if (!binding)
                    {
                        const auto bound = scope.bindings.find(port.InterfaceName);
                        binding = bound != scope.bindings.end() ? &bound->second : nullptr;
                    }
                    if (binding)
                    {
                        if (!binding->Valid())
                            return Value::Broadcast(ComponentsOf(port.Type), 0.0f);
                        return EvalPort(*binding->port, *binding->scope, state);
                    }
                    const Port* interfacePort = scope.graph->InterfacePort(port.InterfaceName);
                    if (interfacePort && interfacePort != &port)
                        return EvalPort(*interfacePort, interfacePort->Connected() && scope.parent ? *scope.parent : scope, state);
                    return Value::Broadcast(ComponentsOf(port.Type), 0.0f);
                }
                if (!port.NodeName.empty())
                {
                    const Node* node = port.Target && port.Target->Owner == scope.graph ? port.Target : scope.graph->FindNode(port.NodeName);
                    const Scope* nodeScope = &scope;
                    if (!node && scope.parent)
                    {
                        node = scope.parent->graph->FindNode(port.NodeName);
                        nodeScope = scope.parent;
                    }
                    if (!node)
                        return Value::Broadcast(ComponentsOf(port.Type), 0.0f);
                    return EvalNode(*node, port.Output, *nodeScope, state);
                }
                if (!port.NodeGraph.empty())
                {
                    const Graph* graph = FindGraph(port.NodeGraph);
                    const Port* output = graph ? graph->OutputPort(port.Output) : nullptr;
                    if (!output)
                        return Value::Broadcast(ComponentsOf(port.Type), 0.0f);
                    const auto found = m_GraphScopes.find(graph);
                    if (found == m_GraphScopes.end())
                        return Value::Broadcast(ComponentsOf(port.Type), 0.0f);
                    return EvalPort(*output, *found->second, state);
                }
                if (port.HasValue)
                    return port.Parsed;
                return Value::Broadcast(ComponentsOf(port.Type), 0.0f);
            }

            Value Input(const Node& node, const char* name, const Scope& scope, EvalState& state, const Value& fallback)
            {
                const Port* port = node.Input(name);
                return port ? EvalPort(*port, scope, state) : fallback;
            }

            // MaterialX's texture coordinates: v up (the image's bottom row at 0), the mesh UV's v down.
            Value TexcoordDefault(const EvalState& state) const { return Value::Vec(2, state.u, 1.0f - state.v); }

            Value SampleImageNode(const Node& node, const Scope& scope, EvalState& state, bool tiled)
            {
                const int components = node.Components;
                Value texcoord = node.Input("texcoord") ? EvalPort(*node.Input("texcoord"), scope, state) : TexcoordDefault(state);
                if (tiled)
                {
                    const Value tiling = Input(node, "uvtiling", scope, state, Value::Vec(2, 1.0f, 1.0f));
                    const Value offset = Input(node, "uvoffset", scope, state, Value::Vec(2, 0.0f, 0.0f));
                    texcoord = Value::Vec(2, texcoord[0] * tiling[0] - offset[0], texcoord[1] * tiling[1] - offset[1]);
                }
                if (const Image* image = node.SampledImage)
                {
                    float texel[4];
                    if (SampleImage(*image, texcoord[0], 1.0f - texcoord[1], AddressMode(node.AddressU), AddressMode(node.AddressV), texel))
                        return Value::Vec(components, texel[0], texel[1], texel[2], texel[3]);
                }
                return Input(node, "default", scope, state, Value::Broadcast(components, 0.0f));
            }

            static Value Map1(const Value& a, int n, float (*f)(float))
            {
                Value r;
                r.n = n;
                for (int i = 0; i < n; ++i)
                    r.v[i] = f(a[i]);
                return r;
            }
            template <typename F>
            static Value Map2(const Value& a, const Value& b, int n, F f)
            {
                Value r;
                r.n = n;
                for (int i = 0; i < n; ++i)
                    r.v[i] = f(a[i], b[i]);
                return r;
            }

            Value EvalNode(const Node& node, const std::string& output, const Scope& scope, EvalState& state)
            {
                // Separate nodes: their input, one component of it.
                if (node.Hash == CategoryHash("separate2") || node.Hash == CategoryHash("separate3") || node.Hash == CategoryHash("separate4"))
                {
                    const Value in = Input(node, "in", scope, state, Value{});
                    static const char* names = "xyzwrgba";
                    int component = 0;
                    if (output.size() == 4)
                        if (const char* at = std::strchr(names, output[3]))
                            component = int(at - names) % 4;
                    return Value::Scalar(in[component]);
                }
                if (!node.Builtin)
                {
                    const Scope* instance = Instance(node, scope, false);
                    if (!instance)
                    {
                        Warn("unsupported node '" + node.Category + "' (its output is taken as 0)");
                        return Value::Broadcast(ComponentsOf(node.Type), 0.0f);
                    }
                    const Port* out = instance->graph->OutputPort(output);
                    return out ? EvalPort(*out, *instance, state) : Value::Broadcast(ComponentsOf(node.Type), 0.0f);
                }
                if (node.Id >= 0 && size_t(node.Id) < state.stamp.size() && state.stamp[node.Id] == state.current)
                    return state.memo[node.Id];
                if (++state.depth > 256)
                {
                    --state.depth;
                    Warn("node graph too deep or cyclic at '" + node.Name + "'");
                    return Value::Broadcast(ComponentsOf(node.Type), 0.0f);
                }
                const Value result = Compute(node, scope, state);
                --state.depth;
                if (node.Id >= 0 && size_t(node.Id) < state.stamp.size())
                {
                    state.stamp[node.Id] = state.current;
                    state.memo[node.Id] = result;
                }
                return result;
            }

            Value Compute(const Node& node, const Scope& scope, EvalState& state)
            {
                const std::string& c = node.Category;
                const uint32_t h = node.Hash;
                const int n = node.Components;
                auto in = [&](const char* name, float fallback = 0.0f) { return Input(node, name, scope, state, Value::Broadcast(n, fallback)); };
                auto inScalar = [&](const char* name, float fallback) { return Input(node, name, scope, state, Value::Scalar(fallback))[0]; };
                auto inVec2 = [&](const char* name, float x, float y) { return Input(node, name, scope, state, Value::Vec(2, x, y)); };
                auto texcoord = [&]() { return node.Input("texcoord") ? EvalPort(*node.Input("texcoord"), scope, state) : TexcoordDefault(state); };
                auto position = [&]() {
                    if (node.Input("position"))
                        return EvalPort(*node.Input("position"), scope, state);
                    Warn("3D procedural baked with the UV coordinates in place of the object position (" + node.Category + ")");
                    return Value::Vec(3, state.u, 1.0f - state.v, 0.0f);
                };
                auto broadcastNoise = [&](float x) { return Value::Broadcast(n, x); };

                if (h == CategoryHash("constant")) return in("value");
                if (h == CategoryHash("dot")) return in("in");
                if (h == CategoryHash("image")) return SampleImageNode(node, scope, state, false);
                if (h == CategoryHash("tiledimage")) return SampleImageNode(node, scope, state, true);
                if (h == CategoryHash("hextiledimage"))
                {
                    Warn("hextiledimage is baked as a plain tiled image (no hexagonal blending)");
                    return SampleImageNode(node, scope, state, true);
                }
                if (h == CategoryHash("texcoord")) return n == 3 ? Value::Vec(3, state.u, 1.0f - state.v, 0.0f) : TexcoordDefault(state);
                if (h == CategoryHash("position"))
                {
                    Warn("position baked as the UV coordinates (a .mtlx material has no mesh)");
                    return Value::Vec(3, state.u, 1.0f - state.v, 0.0f);
                }
                if (h == CategoryHash("normal")) return Value::Vec(3, 0.0f, 0.0f, 1.0f);
                if (h == CategoryHash("tangent")) return Value::Vec(3, 1.0f, 0.0f, 0.0f);
                if (h == CategoryHash("bitangent")) return Value::Vec(3, 0.0f, 1.0f, 0.0f);
                if (h == CategoryHash("geomcolor")) return Value::Broadcast(n, 1.0f);
                if (h == CategoryHash("geompropvalue")) return in("default");
                if (h == CategoryHash("time") || h == CategoryHash("frame")) return Value::Scalar(0.0f);
                if (h == CategoryHash("add")) return Map2(in("in1"), in("in2"), n, [](float a, float b) { return a + b; });
                if (h == CategoryHash("subtract")) return Map2(in("in1"), in("in2"), n, [](float a, float b) { return a - b; });
                if (h == CategoryHash("multiply")) return Map2(in("in1"), in("in2", 1.0f), n, [](float a, float b) { return a * b; });
                if (h == CategoryHash("divide")) return Map2(in("in1"), in("in2", 1.0f), n, [](float a, float b) { return b != 0.0f ? a / b : 0.0f; });
                if (h == CategoryHash("modulo")) return Map2(in("in1"), in("in2", 1.0f), n, [](float a, float b) { return b != 0.0f ? a - b * std::floor(a / b) : 0.0f; });
                if (h == CategoryHash("power") || h == CategoryHash("safepower")) return Map2(in("in1"), in("in2", 1.0f), n, [](float a, float b) { return std::pow(std::max(a, 0.0f), b); });
                if (h == CategoryHash("invert")) return Map2(in("in"), in("amount", 1.0f), n, [](float a, float b) { return b - a; });
                if (h == CategoryHash("absval")) return Map1(in("in"), n, [](float x) { return std::fabs(x); });
                if (h == CategoryHash("floor")) return Map1(in("in"), n, [](float x) { return std::floor(x); });
                if (h == CategoryHash("ceil")) return Map1(in("in"), n, [](float x) { return std::ceil(x); });
                if (h == CategoryHash("round")) return Map1(in("in"), n, [](float x) { return std::round(x); });
                if (h == CategoryHash("sign")) return Map1(in("in"), n, [](float x) { return float((x > 0.0f) - (x < 0.0f)); });
                if (h == CategoryHash("sqrt")) return Map1(in("in"), n, [](float x) { return std::sqrt(std::max(x, 0.0f)); });
                if (h == CategoryHash("sin")) return Map1(in("in"), n, [](float x) { return std::sin(x); });
                if (h == CategoryHash("cos")) return Map1(in("in"), n, [](float x) { return std::cos(x); });
                if (h == CategoryHash("tan")) return Map1(in("in"), n, [](float x) { return std::tan(x); });
                if (h == CategoryHash("asin")) return Map1(in("in"), n, [](float x) { return std::asin(std::clamp(x, -1.0f, 1.0f)); });
                if (h == CategoryHash("acos")) return Map1(in("in"), n, [](float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); });
                if (h == CategoryHash("atan2")) return Map2(in("iny"), in("inx", 1.0f), n, [](float y, float x) { return std::atan2(y, x); });
                if (h == CategoryHash("exp")) return Map1(in("in"), n, [](float x) { return std::exp(x); });
                if (h == CategoryHash("ln")) return Map1(in("in"), n, [](float x) { return std::log(std::max(x, 1e-30f)); });
                if (h == CategoryHash("clamp"))
                {
                    const Value a = in("in"), low = in("low", 0.0f), high = in("high", 1.0f);
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i) r.v[i] = std::clamp(a[i], low[i], std::max(low[i], high[i]));
                    return r;
                }
                if (h == CategoryHash("min")) return Map2(in("in1"), in("in2"), n, [](float a, float b) { return std::min(a, b); });
                if (h == CategoryHash("max")) return Map2(in("in1"), in("in2"), n, [](float a, float b) { return std::max(a, b); });
                if (h == CategoryHash("mix"))
                {
                    const Value fg = in("fg"), bg = in("bg"), amount = Input(node, "mix", scope, state, Value::Scalar(0.0f));
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i) r.v[i] = bg[i] + (fg[i] - bg[i]) * amount[i];
                    return r;
                }
                if (h == CategoryHash("remap"))
                {
                    const Value a = in("in"), inLow = in("inlow", 0.0f), inHigh = in("inhigh", 1.0f), outLow = in("outlow", 0.0f), outHigh = in("outhigh", 1.0f);
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i)
                        r.v[i] = outLow[i] + (a[i] - inLow[i]) * (outHigh[i] - outLow[i]) / ((inHigh[i] - inLow[i]) != 0.0f ? (inHigh[i] - inLow[i]) : 1.0f);
                    return r;
                }
                if (h == CategoryHash("range"))
                {
                    const Value a = in("in"), inLow = in("inlow", 0.0f), inHigh = in("inhigh", 1.0f), gamma = in("gamma", 1.0f),
                                outLow = in("outlow", 0.0f), outHigh = in("outhigh", 1.0f);
                    const bool doClamp = inScalar("doclamp", 0.0f) != 0.0f;
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i)
                    {
                        float x = (a[i] - inLow[i]) / ((inHigh[i] - inLow[i]) != 0.0f ? (inHigh[i] - inLow[i]) : 1.0f);
                        x = std::copysign(std::pow(std::fabs(x), 1.0f / (gamma[i] != 0.0f ? gamma[i] : 1.0f)), x);
                        x = outLow[i] + x * (outHigh[i] - outLow[i]);
                        r.v[i] = doClamp ? std::clamp(x, std::min(outLow[i], outHigh[i]), std::max(outLow[i], outHigh[i])) : x;
                    }
                    return r;
                }
                if (h == CategoryHash("smoothstep"))
                {
                    const Value a = in("in"), low = in("low", 0.0f), high = in("high", 1.0f);
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i)
                    {
                        const float t = std::clamp((a[i] - low[i]) / ((high[i] - low[i]) != 0.0f ? (high[i] - low[i]) : 1.0f), 0.0f, 1.0f);
                        r.v[i] = t * t * (3.0f - 2.0f * t);
                    }
                    return r;
                }
                if (h == CategoryHash("contrast"))
                {
                    const Value a = in("in"), amount = in("amount", 1.0f), pivot = in("pivot", 0.5f);
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i) r.v[i] = (a[i] - pivot[i]) * amount[i] + pivot[i];
                    return r;
                }
                if (h == CategoryHash("luminance"))
                {
                    const Value a = Input(node, "in", scope, state, Value::Broadcast(3, 0.0f));
                    const Value coefficients = Input(node, "lumacoeffs", scope, state, Value::Vec(3, 0.2722287f, 0.6740818f, 0.0536895f));
                    const float y = a[0] * coefficients[0] + a[1] * coefficients[1] + a[2] * coefficients[2];
                    return Value::Vec(n, y, y, y, n == 4 ? a[3] : 0.0f);
                }
                if (h == CategoryHash("dotproduct"))
                {
                    const Value a = Input(node, "in1", scope, state, Value{}), b = Input(node, "in2", scope, state, Value{});
                    const int count = std::max(a.n, b.n);
                    float sum = 0.0f;
                    for (int i = 0; i < count; ++i) sum += a[i] * b[i];
                    return Value::Scalar(sum);
                }
                if (h == CategoryHash("crossproduct"))
                {
                    const Value a = in("in1"), b = in("in2");
                    return Value::Vec(3, a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
                }
                if (h == CategoryHash("magnitude"))
                {
                    const Value a = Input(node, "in", scope, state, Value{});
                    float sum = 0.0f;
                    for (int i = 0; i < a.n; ++i) sum += a.v[i] * a.v[i];
                    return Value::Scalar(std::sqrt(sum));
                }
                if (h == CategoryHash("normalize"))
                {
                    const Value a = in("in");
                    float sum = 0.0f;
                    for (int i = 0; i < n; ++i) sum += a[i] * a[i];
                    const float length = std::sqrt(sum);
                    Value r = a; r.n = n;
                    for (int i = 0; i < n; ++i) r.v[i] = length > 0.0f ? a[i] / length : 0.0f;
                    return r;
                }
                if (h == CategoryHash("extract"))
                {
                    const Value a = Input(node, "in", scope, state, Value{});
                    const int index = std::clamp(int(inScalar("index", 0.0f)), 0, 3);
                    return Value::Scalar(a.v[std::min(index, a.n - 1)]);
                }
                if (h == CategoryHash("convert"))
                {
                    const Value a = Input(node, "in", scope, state, Value{});
                    if (a.n == 1) return Value::Broadcast(n, a.v[0]);
                    Value r = a;
                    r.n = n;
                    for (int i = a.n; i < n; ++i) r.v[i] = (i == 3) ? 1.0f : 0.0f;
                    return r;
                }
                if (h == CategoryHash("combine2") || h == CategoryHash("combine3") || h == CategoryHash("combine4"))
                {
                    const Value a = in("in1"), b = in("in2"), d = in("in3"), e = in("in4");
                    if (h == CategoryHash("combine2") && n == 4) // two vector2 into a vector4
                        return Value::Vec(4, a[0], a[1], b[0], b[1]);
                    if (h == CategoryHash("combine2") && a.n == 3) // a color3 and alpha into a color4
                        return Value::Vec(4, a[0], a[1], a[2], b[0]);
                    return Value::Vec(n, a[0], b[0], d[0], e[0]);
                }
                if (h == CategoryHash("swizzle"))
                {
                    const Value a = Input(node, "in", scope, state, Value{});
                    const Port* channels = node.Input("channels");
                    const std::string pattern = channels && channels->HasValue ? channels->ValueText : "xyzw";
                    Value r; r.n = n;
                    for (int i = 0; i < n && i < int(pattern.size()); ++i)
                    {
                        const char ch = pattern[size_t(i)];
                        if (ch == '0' || ch == '1') { r.v[i] = float(ch - '0'); continue; }
                        static const char* names = "xyzwrgba";
                        const char* at = std::strchr(names, ch);
                        r.v[i] = at ? a[int(at - names) % 4] : 0.0f;
                    }
                    return r;
                }
                if (h == CategoryHash("ifgreater") || h == CategoryHash("ifgreatereq") || h == CategoryHash("ifequal"))
                {
                    const float value1 = inScalar("value1", h == CategoryHash("ifequal") ? 0.0f : 1.0f), value2 = inScalar("value2", 0.0f);
                    const bool condition = h == CategoryHash("ifgreater") ? value1 > value2 : h == CategoryHash("ifgreatereq") ? value1 >= value2 : value1 == value2;
                    return condition ? in("in1") : in("in2");
                }
                if (h == CategoryHash("roughness_anisotropy"))
                {
                    // MaterialX's mx_roughness_anisotropy: GGX's alpha along the tangent and the bitangent.
                    const float roughness = inScalar("roughness", 0.0f), anisotropy = inScalar("anisotropy", 0.0f);
                    const float roughnessSqr = std::clamp(roughness * roughness, 1e-6f, 1.0f);
                    if (anisotropy > 0.0f)
                    {
                        const float aspect = std::sqrt(1.0f - std::clamp(anisotropy, 0.0f, 0.98f));
                        return Value::Vec(2, std::min(roughnessSqr / aspect, 1.0f), roughnessSqr * aspect);
                    }
                    return Value::Vec(2, roughnessSqr, roughnessSqr);
                }
                if (h == CategoryHash("normalmap"))
                    return Value::Vec(3, 0.0f, 0.0f, 1.0f); // used as a color elsewhere: the unperturbed normal (the normal slots read it apart)
                if (h == CategoryHash("place2d"))
                {
                    Value p = texcoord();
                    const Value pivot = inVec2("pivot", 0.0f, 0.0f), scale = inVec2("scale", 1.0f, 1.0f), offset = inVec2("offset", 0.0f, 0.0f);
                    const float angle = inScalar("rotate", 0.0f) * 3.14159265f / 180.0f;
                    const bool trs = inScalar("operationorder", 0.0f) != 0.0f;
                    float x = p[0] - pivot[0], y = p[1] - pivot[1];
                    auto applyScale = [&]() { x /= scale[0] != 0.0f ? scale[0] : 1.0f; y /= scale[1] != 0.0f ? scale[1] : 1.0f; };
                    auto applyRotate = [&]() { const float cs = std::cos(angle), sn = std::sin(angle); const float rx = x * cs + y * sn, ry = -x * sn + y * cs; x = rx; y = ry; };
                    auto applyOffset = [&]() { x -= offset[0]; y -= offset[1]; };
                    if (trs) { applyOffset(); applyRotate(); applyScale(); }
                    else { applyScale(); applyRotate(); applyOffset(); }
                    return Value::Vec(2, x + pivot[0], y + pivot[1]);
                }
                if (h == CategoryHash("rotate2d"))
                {
                    const Value p = Input(node, "in", scope, state, Value::Vec(2, 0.0f, 0.0f));
                    const float angle = inScalar("amount", 0.0f) * 3.14159265f / 180.0f;
                    const float cs = std::cos(angle), sn = std::sin(angle);
                    return Value::Vec(2, p[0] * cs - p[1] * sn, p[0] * sn + p[1] * cs);
                }
                if (h == CategoryHash("noise2d") || h == CategoryHash("fractal2d"))
                {
                    const Value t = texcoord();
                    const Value amplitude = in("amplitude", 1.0f), pivot = in("pivot", 0.0f);
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i)
                    {
                        float value = 0.0f;
                        if (h == CategoryHash("noise2d"))
                            value = Perlin2(t[0], t[1], uint32_t(i) * 0x51ED27u);
                        else
                        {
                            const int octaves = std::clamp(int(inScalar("octaves", 3.0f)), 1, 16);
                            const float lacunarity = inScalar("lacunarity", 2.0f), diminish = inScalar("diminish", 0.5f);
                            float amp = 1.0f, fx = t[0], fy = t[1];
                            for (int o = 0; o < octaves; ++o, amp *= diminish, fx *= lacunarity, fy *= lacunarity)
                                value += amp * Perlin2(fx, fy, uint32_t(i) * 0x51ED27u);
                        }
                        r.v[i] = value * amplitude[i] + (h == CategoryHash("noise2d") ? pivot[i] : 0.0f);
                    }
                    return r;
                }
                if (h == CategoryHash("noise3d") || h == CategoryHash("fractal3d"))
                {
                    const Value p = position();
                    const Value amplitude = in("amplitude", 1.0f), pivot = in("pivot", 0.0f);
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i)
                    {
                        float value = 0.0f;
                        if (h == CategoryHash("noise3d"))
                            value = Perlin3(p[0], p[1], p[2], uint32_t(i) * 0x51ED27u);
                        else
                        {
                            const int octaves = std::clamp(int(inScalar("octaves", 3.0f)), 1, 16);
                            const float lacunarity = inScalar("lacunarity", 2.0f), diminish = inScalar("diminish", 0.5f);
                            float amp = 1.0f, fx = p[0], fy = p[1], fz = p[2];
                            for (int o = 0; o < octaves; ++o, amp *= diminish, fx *= lacunarity, fy *= lacunarity, fz *= lacunarity)
                                value += amp * Perlin3(fx, fy, fz, uint32_t(i) * 0x51ED27u);
                        }
                        r.v[i] = value * amplitude[i] + (h == CategoryHash("noise3d") ? pivot[i] : 0.0f);
                    }
                    return r;
                }
                if (h == CategoryHash("cellnoise2d")) { const Value t = texcoord(); return broadcastNoise(CellNoise2(t[0], t[1])); }
                if (h == CategoryHash("cellnoise3d")) { const Value p = position(); return broadcastNoise(CellNoise3(p[0], p[1], p[2])); }
                if (h == CategoryHash("worleynoise2d")) { const Value t = texcoord(); return broadcastNoise(Worley2(t[0], t[1], inScalar("jitter", 1.0f))); }
                if (h == CategoryHash("worleynoise3d"))
                {
                    const Value p = position();
                    return broadcastNoise(Worley2(p[0] + p[2] * 0.618f, p[1] + p[2] * 0.382f, inScalar("jitter", 1.0f)));
                }
                if (h == CategoryHash("ramplr")) { const Value t = texcoord(); const Value l = in("valuel"), r = in("valuer");
                    return Map2(l, r, n, [&](float a, float b) { return a + (b - a) * std::clamp(t[0], 0.0f, 1.0f); }); }
                if (h == CategoryHash("ramptb")) { const Value t = texcoord(); const Value top = in("valuet"), bottom = in("valueb");
                    return Map2(bottom, top, n, [&](float a, float b) { return a + (b - a) * std::clamp(t[1], 0.0f, 1.0f); }); }
                if (h == CategoryHash("splitlr")) { const Value t = texcoord(); const float center = inScalar("center", 0.5f);
                    return t[0] < center ? in("valuel") : in("valuer"); }
                if (h == CategoryHash("splittb")) { const Value t = texcoord(); const float center = inScalar("center", 0.5f);
                    return t[1] < center ? in("valueb") : in("valuet"); }
                if (h == CategoryHash("ramp4"))
                {
                    const Value t = texcoord();
                    const float s = std::clamp(t[0], 0.0f, 1.0f), u = std::clamp(t[1], 0.0f, 1.0f);
                    const Value tl = in("valuetl"), tr = in("valuetr"), bl = in("valuebl"), br = in("valuebr");
                    Value r; r.n = n;
                    for (int i = 0; i < n; ++i)
                    {
                        const float bottom = bl[i] + (br[i] - bl[i]) * s, top = tl[i] + (tr[i] - tl[i]) * s;
                        r.v[i] = bottom + (top - bottom) * u;
                    }
                    return r;
                }
                if (h == CategoryHash("checkerboard"))
                {
                    const Value t = texcoord();
                    const Value tiling = inVec2("uvtiling", 8.0f, 8.0f), offset = inVec2("uvoffset", 0.0f, 0.0f);
                    const int cell = int(std::floor(t[0] * tiling[0] - offset[0])) + int(std::floor(t[1] * tiling[1] - offset[1]));
                    return (cell & 1) == 0 ? Input(node, "color1", scope, state, Value::Broadcast(n, 1.0f)) : Input(node, "color2", scope, state, Value::Broadcast(n, 0.0f));
                }
                if (h == CategoryHash("hsvtorgb"))
                {
                    const Value a = in("in");
                    const float h = (a[0] - std::floor(a[0])) * 6.0f, s = a[1], v = a[2];
                    const int sector = int(h) % 6;
                    const float f = h - std::floor(h), p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
                    const float rgb[6][3] = { { v, t, p }, { q, v, p }, { p, v, t }, { p, q, v }, { t, p, v }, { v, p, q } };
                    return Value::Vec(n, rgb[sector][0], rgb[sector][1], rgb[sector][2], n == 4 ? a[3] : 0.0f);
                }
                if (h == CategoryHash("rgbtohsv"))
                {
                    const Value a = in("in");
                    const float maxC = std::max({ a[0], a[1], a[2] }), minC = std::min({ a[0], a[1], a[2] }), delta = maxC - minC;
                    float h = 0.0f;
                    if (delta > 0.0f)
                    {
                        if (maxC == a[0]) h = std::fmod((a[1] - a[2]) / delta, 6.0f);
                        else if (maxC == a[1]) h = (a[2] - a[0]) / delta + 2.0f;
                        else h = (a[0] - a[1]) / delta + 4.0f;
                        h /= 6.0f;
                        if (h < 0.0f) h += 1.0f;
                    }
                    return Value::Vec(n, h, maxC > 0.0f ? delta / maxC : 0.0f, maxC, n == 4 ? a[3] : 0.0f);
                }
                Warn("unsupported node '" + c + "' (its first input is passed through)");
                for (const char* name : { "in", "in1", "bg", "fg", "value" })
                    if (node.Input(name))
                        return in(name);
                return Value::Broadcast(n, 0.0f);
            }

            // ---- Analysis: what varies over the surface, and from which images

            struct Leaves
            {
                std::vector<const Node*> Images; // image / tiledimage nodes (their images loaded into m_NodeImages)
                bool OtherVarying = false;       // texcoord, procedurals, an image with a custom texcoord
            };

            // Whether a port's value changes over the surface. Also creates the node-graph instances and loads the images the bake
            // will read, and collects the leaves that make it vary.
            bool Prepare(const Port* port, const Scope* scope, Leaves& leaves, int guard = 0)
            {
                if (!port || !scope || guard > 256)
                    return false;
                if (!port->InterfaceName.empty())
                {
                    const auto bound = scope->bindings.find(port->InterfaceName);
                    if (bound != scope->bindings.end())
                    {
                        // Remembered for the bake (the port belongs to this one scope: graphs are cloned per use).
                        port->BoundScope = scope;
                        port->Bound = &bound->second;
                        return bound->second.Valid() && Prepare(bound->second.port, bound->second.scope, leaves, guard + 1);
                    }
                    const Port* interfacePort = scope->graph->InterfacePort(port->InterfaceName);
                    if (!interfacePort || interfacePort == port)
                        return false;
                    return Prepare(interfacePort, interfacePort->Connected() && scope->parent ? scope->parent : scope, leaves, guard + 1);
                }
                if (!port->NodeName.empty())
                {
                    const Node* node = scope->graph->FindNode(port->NodeName);
                    const Scope* nodeScope = scope;
                    if (!node && scope->parent)
                    {
                        node = scope->parent->graph->FindNode(port->NodeName);
                        nodeScope = scope->parent;
                    }
                    return node && PrepareNode(*node, port->Output, *nodeScope, leaves, guard + 1);
                }
                if (!port->NodeGraph.empty())
                {
                    const Graph* graph = FindGraph(port->NodeGraph);
                    if (!graph)
                    {
                        Warn("missing node graph '" + port->NodeGraph + "'");
                        return false;
                    }
                    const Scope& graphScope = GraphScope(graph);
                    return Prepare(graph->OutputPort(port->Output), &graphScope, leaves, guard + 1);
                }
                return false;
            }

            bool PrepareNode(const Node& node, const std::string& output, const Scope& scope, Leaves& leaves, int guard)
            {
                if (!node.Builtin)
                {
                    const Scope* instance = Instance(node, scope, true);
                    if (!instance)
                    {
                        Warn("unsupported node '" + node.Category + "' (its output is taken as 0)");
                        return false;
                    }
                    return Prepare(instance->graph->OutputPort(output), instance, leaves, guard + 1);
                }
                const auto key = std::make_pair(&node, &scope);
                const auto known = m_Varying.find(key);
                Leaves own;
                bool varying = false;
                if (known != m_Varying.end())
                {
                    own = known->second.second;
                    varying = known->second.first;
                }
                else
                {
                    const uint32_t h = node.Hash;
                    if (h == CategoryHash("image") || h == CategoryHash("tiledimage") || h == CategoryHash("hextiledimage"))
                    {
                        varying = true;
                        FileRef file;
                        const Image* image = nullptr;
                        if (ResolveFile(node.Input("file"), &scope, file))
                            image = LoadImage(file);
                        m_NodeImages[&node] = image;
                        node.SampledImage = image;
                        const Port* uMode = node.Input("uaddressmode");
                        const Port* vMode = node.Input("vaddressmode");
                        node.AddressU = int(ParseAddressMode(uMode && uMode->HasValue ? uMode->ValueText : ""));
                        node.AddressV = int(ParseAddressMode(vMode && vMode->HasValue ? vMode->ValueText : ""));
                        if (image)
                            own.Images.push_back(&node);
                        // A texcoord of its own (place2d, a glTF transform) moves the image: no bake in its own texture space.
                        if (const Port* texcoordPort = node.Input("texcoord"); texcoordPort && texcoordPort->Connected())
                        {
                            own.OtherVarying = true;
                            Prepare(texcoordPort, &scope, own, guard + 1);
                        }
                        for (const char* name : { "uvtiling", "uvoffset", "default" })
                            if (const Port* other = node.Input(name); other && Prepare(other, &scope, own, guard + 1))
                                own.OtherVarying = true;
                    }
                    else if (h == CategoryHash("texcoord") || h == CategoryHash("position") || h == CategoryHash("noise2d") || h == CategoryHash("noise3d") || h == CategoryHash("fractal2d") || h == CategoryHash("fractal3d") ||
                             h == CategoryHash("cellnoise2d") || h == CategoryHash("cellnoise3d") || h == CategoryHash("worleynoise2d") || h == CategoryHash("worleynoise3d") || h == CategoryHash("ramplr") ||
                             h == CategoryHash("ramptb") || h == CategoryHash("ramp4") || h == CategoryHash("splitlr") || h == CategoryHash("splittb") || h == CategoryHash("checkerboard") || h == CategoryHash("place2d"))
                    {
                        varying = true;
                        own.OtherVarying = true;
                        for (const Port& input : node.Inputs)
                            Prepare(&input, &scope, own, guard + 1);
                    }
                    else
                    {
                        for (const Port& input : node.Inputs)
                            if (Prepare(&input, &scope, own, guard + 1))
                                varying = true;
                    }
                    m_Varying[key] = { varying, own };
                }
                leaves.Images.insert(leaves.Images.end(), own.Images.begin(), own.Images.end());
                leaves.OtherVarying = leaves.OtherVarying || own.OtherVarying;
                return varying;
            }

            int NodeCount() const { return m_NextId; }

            // A constant port value (evaluated once at the UV centre).
            Value Constant(const Source& source, Value fallback)
            {
                if (!source.Valid())
                    return fallback;
                EvalState state;
                state.stamp.assign(size_t(m_NextId), 0);
                state.memo.resize(size_t(m_NextId));
                return EvalPort(*source.port, *source.scope, state);
            }

            const Scope& UserRoot() const { return m_UserRoot; }
            const Scope& DefinitionsRoot() const { return m_DefinitionsRoot; }
            const Image* NodeImage(const Node* node) const
            {
                const auto found = m_NodeImages.find(node);
                return found == m_NodeImages.end() ? nullptr : found->second;
            }

        private:
            const Document& m_User;
            const Document& m_Definitions;
            int& m_NextId;
            Scope m_UserRoot, m_DefinitionsRoot;
            std::deque<Scope> m_Scopes;
            std::vector<std::unique_ptr<Graph>> m_Clones;
            std::map<std::pair<const Node*, const Scope*>, const Scope*> m_Instances;
            std::map<const Graph*, const Scope*> m_GraphScopes;
            std::map<std::pair<const Node*, const Scope*>, std::pair<bool, Leaves>> m_Varying;
            std::unordered_map<const Node*, const Image*> m_NodeImages;
            std::map<std::string, std::unique_ptr<Image>> m_Images;
            std::mutex m_ImageMutex;
            std::set<std::string> m_Warned;
        };

        // ---- Baking -----------------------------------------------------------------------------------------------------------

        // One texture of glTF's layout: up to four inputs evaluated per texel, turned into its RGBA by `pack`.
        struct BakeChannel
        {
            Source source;
            Value fallback;
            bool varying = false;
            Converter::Leaves leaves;
        };

        struct BakeTexture
        {
            std::string Slot;                      // file name part
            std::vector<BakeChannel> Inputs;
            std::function<void(const Value* inputs, float rgba[4])> Pack;
            bool SrgbColor = false;                // RGB written sRGB-encoded (a color texture)
        };

        std::string Sanitize(const std::string& name)
        {
            std::string out;
            for (char ch : name)
                out += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') ? ch : '_';
            return out.empty() ? std::string("Material") : out;
        }

        // The texture-space transform of a tiled image: glTF's UV transform (scale, offset) that maps the mesh UV onto the image as
        // the tiledimage node reads it (MaterialX's v up: texcoord (u, 1 - v), times the tiling, less the offset).
        struct TextureSpace
        {
            bool Valid = false;
            float TilingX = 1.0f, TilingY = 1.0f, OffsetX = 0.0f, OffsetY = 0.0f;
            int Width = 0, Height = 0;
            const Image* SingleImage = nullptr;

            MaterialUVChannel Channel() const
            {
                MaterialUVChannel channel;
                channel.Source = 0;
                channel.Scale = glm::vec2(TilingX, TilingY);
                channel.Offset = glm::vec2(-OffsetX, 1.0f - TilingY + OffsetY);
                return channel;
            }
            bool SameTransform(const TextureSpace& other) const
            {
                return TilingX == other.TilingX && TilingY == other.TilingY && OffsetX == other.OffsetX && OffsetY == other.OffsetY;
            }
        };

        bool WritePng(const std::filesystem::path& path, int width, int height, const std::vector<uint8_t>& rgba)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            // Light compression: the PNG is only the source the texture cook turns into BC7 (stb's default level 8 took about a second
            // for a 2048^2 texture, most of a large material's import).
            stbi_write_png_compression_level = 2;
            return stbi_write_png(path.string().c_str(), width, height, 4, rgba.data(), width * 4) != 0;
        }

        // The image's own 8-bit RGBA, to see whether a bake is exactly it.
        bool SameAsImageFile(const std::filesystem::path& path, int width, int height, const std::vector<uint8_t>& rgba)
        {
            int w = 0, h = 0, channels = 0;
            stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
            if (!pixels)
                return false;
            bool same = w == width && h == height;
            for (size_t i = 0; same && i < rgba.size(); ++i)
                same = std::abs(int(pixels[i]) - int(rgba[i])) <= 1;
            stbi_image_free(pixels);
            return same;
        }
    }

    bool MaterialXImporter::Import(const std::filesystem::path& assetDirectory, const std::filesystem::path& relativePath,
                                   const std::filesystem::path& outputFolder, const std::filesystem::path& definitionsDirectory,
                                   std::vector<MaterialXMaterial>& outMaterials, std::string& error)
    {
        int nextId = 0;
        Document user, definitions;
        if (!LoadDocument(assetDirectory / relativePath, user, nextId, error))
            return false;
        for (const char* file : { "standard_surface.mtlx", "open_pbr_surface.mtlx", "gltf_pbr.mtlx", "usd_preview_surface.mtlx",
                                  "standard_surface_to_gltf_pbr.mtlx", "open_pbr_to_standard_surface.mtlx" })
        {
            std::string definitionError;
            Document part;
            if (!LoadDocument(definitionsDirectory / file, definitions.Root ? part : definitions, nextId, definitionError))
            {
                error = "MaterialX definitions missing (" + definitionError + ")";
                return false;
            }
            if (part.Root)
            {
                for (auto& [name, graph] : part.Graphs)
                    definitions.Graphs[name] = std::move(graph);
                for (auto& [name, definition] : part.NodeDefs)
                    definitions.NodeDefs[name] = std::move(definition);
            }
        }
        for (auto& [name, graph] : definitions.Graphs)
            graph->Doc = &definitions;
        InheritDefinitions(definitions.NodeDefs);
        InheritDefinitions(user.NodeDefs);


        // Every surfacematerial of the document, in name order (a stable result).
        std::vector<const Node*> materials;
        for (const auto& [name, node] : user.Root->Nodes)
            if (node->Category == "surfacematerial")
                materials.push_back(node.get());
        std::sort(materials.begin(), materials.end(), [](const Node* a, const Node* b) { return a->Name < b->Name; });
        if (materials.empty())
        {
            error = relativePath.filename().generic_string() + ": no surfacematerial (a MaterialX 1.38+ material) in the document";
            return false;
        }

        for (const Node* material : materials)
        {
            MaterialXMaterial result;
            result.Name = material->Name;
            result.MaterialPath = outputFolder / (Sanitize(material->Name) + ".nmat");
            MaterialData& data = result.Data;
            data.Name = material->Name;

            Converter converter(user, definitions, nextId);
            converter.Warnings = &result.Warnings;

            const Port* shaderPort = material->Input("surfaceshader");
            const Node* shader = shaderPort && !shaderPort->NodeName.empty() ? user.Root->FindNode(shaderPort->NodeName) : nullptr;
            if (!shader)
            {
                result.Warnings.push_back("no surface shader: the default material");
                outMaterials.push_back(std::move(result));
                continue;
            }
            const NodeDefinition* shaderDef = converter.FindNodeDefFor(*shader);
            auto shaderInput = [&](const std::string& name) -> Source {
                if (const Port* given = shader->Input(name))
                    return Source{ given, &converter.UserRoot() };
                if (shaderDef)
                    if (const Port* fallback = shaderDef->Input(name))
                        return Source{ fallback, &converter.DefinitionsRoot() };
                return {};
            };
            auto warnIfSet = [&](std::initializer_list<const char*> names) {
                for (const char* name : names)
                {
                    const Port* given = shader->Input(name);
                    if (!given)
                        continue;
                    const Port* fallback = shaderDef ? shaderDef->Input(name) : nullptr;
                    const bool different = given->Connected() || !fallback || !fallback->HasValue ||
                                           std::memcmp(given->Parsed.v, fallback->Parsed.v, sizeof(float) * size_t(std::max(1, given->Parsed.n))) != 0;
                    if (different)
                        result.Warnings.push_back(std::string("'") + name + "' has no equivalent in Nox's material and is ignored");
                }
            };

            // The glTF outputs (KHR names) and where they come from.
            std::map<std::string, Source> gltf;
            std::string normalName, coatNormalName;
            Scope* translation = nullptr;
            auto translate = [&](const char* nodeDefName, const std::function<Source(const std::string&)>& bind) -> Scope* {
                const Graph* graph = converter.FindGraphImplementing(nodeDefName);
                const NodeDefinition* definition = converter.FindNodeDef(nodeDefName);
                if (!graph || !definition)
                    return nullptr;
                Scope& scope = converter.NewScope(converter.CloneGraph(*graph), &converter.DefinitionsRoot());
                for (const Port& input : definition->Inputs)
                    scope.bindings[input.Name] = bind(input.Name);
                return &scope;
            };
            const std::string& category = shader->Category;
            if (category == "standard_surface" || category == "open_pbr_surface")
            {
                const NodeDefinition* standardDef = converter.FindNodeDef("ND_standard_surface_surfaceshader");
                auto standardDefault = [&](const std::string& name) -> Source {
                    const Port* fallback = standardDef ? standardDef->Input(name) : nullptr;
                    return fallback ? Source{ fallback, &converter.DefinitionsRoot() } : Source{};
                };
                if (category == "standard_surface")
                {
                    translation = translate("ND_standard_surface_to_gltf_pbr", [&](const std::string& name) { return shaderInput(name); });
                    normalName = "normal";
                    coatNormalName = "coat_normal";
                    warnIfSet({ "diffuse_roughness", "subsurface", "subsurface_color", "subsurface_radius", "subsurface_scale", "transmission_scatter",
                                "transmission_extra_roughness", "coat_anisotropy", "coat_rotation", "coat_IOR", "coat_affect_color", "coat_affect_roughness" });
                }
                else
                {
                    Scope* openPbr = translate("ND_open_pbr_surface_to_standard_surface", [&](const std::string& name) { return shaderInput(name); });
                    if (openPbr)
                        translation = translate("ND_standard_surface_to_gltf_pbr", [&](const std::string& name) -> Source {
                            if (const Port* output = openPbr->graph->OutputPort(name + "_out"))
                                return Source{ output, openPbr };
                            return standardDefault(name);
                        });
                    normalName = "geometry_normal";
                    coatNormalName = "geometry_coat_normal";
                    warnIfSet({ "base_diffuse_roughness", "subsurface_weight", "subsurface_color", "subsurface_radius", "subsurface_radius_scale",
                                "transmission_scatter", "coat_ior", "coat_roughness_anisotropy", "coat_darkening" });
                    // OpenPBR's dispersion: a scale of its Abbe number (glTF's 20 / Abbe).
                    const float dispersionScale = converter.Constant(shaderInput("transmission_dispersion_scale"), Value::Scalar(0.0f))[0];
                    const float abbe = converter.Constant(shaderInput("transmission_dispersion_abbe_number"), Value::Scalar(20.0f))[0];
                    data.Dispersion = dispersionScale > 0.0f && abbe > 0.0f ? dispersionScale * 20.0f / abbe : 0.0f;
                }
                if (!translation)
                {
                    error = "MaterialX translation graph missing (assets/materialx)";
                    return false;
                }
                for (const Port& output : translation->graph->Outputs)
                {
                    std::string name = output.Name;
                    if (name.size() > 4 && name.compare(name.size() - 4, 4, "_out") == 0)
                        name.resize(name.size() - 4);
                    gltf[name] = Source{ &output, translation };
                }
                if (category == "open_pbr_surface")
                    gltf.erase("dispersion"); // set above from the Abbe number
            }
            else if (category == "gltf_pbr")
            {
                for (const char* name : { "base_color", "alpha", "metallic", "roughness", "occlusion", "transmission", "specular", "specular_color",
                                          "ior", "iridescence", "iridescence_ior", "iridescence_thickness", "sheen_color", "sheen_roughness",
                                          "clearcoat", "clearcoat_roughness", "emissive", "emissive_strength", "thickness", "attenuation_distance",
                                          "attenuation_color", "anisotropy_strength", "anisotropy_rotation", "dispersion", "alpha_mode", "alpha_cutoff" })
                    if (const Source source = shaderInput(name); source.Valid())
                        gltf[name] = source;
                normalName = "normal";
                coatNormalName = "clearcoat_normal";
            }
            else if (category == "UsdPreviewSurface")
            {
                const std::pair<const char*, const char*> names[] = { { "base_color", "diffuseColor" }, { "metallic", "metallic" }, { "roughness", "roughness" },
                                                                      { "emissive", "emissiveColor" }, { "alpha", "opacity" }, { "ior", "ior" },
                                                                      { "clearcoat", "clearcoat" }, { "clearcoat_roughness", "clearcoatRoughness" },
                                                                      { "occlusion", "occlusion" }, { "alpha_cutoff", "opacityThreshold" } };
                for (const auto& [gltfName, usdName] : names)
                    if (const Source source = shaderInput(usdName); source.Valid())
                        gltf[gltfName] = source;
                normalName = "normal";
                if (converter.Constant(shaderInput("useSpecularWorkflow"), Value::Scalar(0.0f))[0] != 0.0f)
                    result.Warnings.push_back("UsdPreviewSurface's specular workflow is converted as metallic-roughness (specularColor ignored)");
            }
            else
            {
                result.Warnings.push_back("unsupported surface shader '" + category + "' (Standard Surface, OpenPBR, glTF PBR and UsdPreviewSurface are): the default material");
                outMaterials.push_back(std::move(result));
                continue;
            }

            // What varies over the surface (and the images behind it); everything the bake will read is made here, before the threads.
            auto channel = [&](const std::string& name, Value fallback) -> BakeChannel {
                BakeChannel bake;
                bake.fallback = fallback;
                const auto found = gltf.find(name);
                if (found == gltf.end())
                    return bake;
                bake.source = found->second;
                bake.varying = converter.Prepare(bake.source.port, bake.source.scope, bake.leaves);
                return bake;
            };
            auto constant = [&](const BakeChannel& bake) { return converter.Constant(bake.source, bake.fallback); };

            // The normal maps: a normalmap node's tangent-space input, baked (or its image named) as the texture; its scale the strength.
            struct NormalSlot
            {
                BakeChannel in;
                float scale = 1.0f;
                bool present = false;
            };
            auto normalSlot = [&](const std::string& name) -> NormalSlot {
                NormalSlot slot;
                if (name.empty())
                    return slot;
                const Source source = shaderInput(name);
                if (!source.Valid() || !source.port->Connected())
                    return slot;
                const auto node = converter.FollowToNode(source.port, source.scope, true);
                if (!node.node || node.node->Category != "normalmap")
                {
                    result.Warnings.push_back("'" + name + "' is not a normalmap node's output (a tangent-space normal map): ignored");
                    return slot;
                }
                slot.present = true;
                if (const Port* scale = node.node->Input("scale"))
                    slot.scale = converter.Constant(Source{ scale, node.scope }, Value::Scalar(1.0f))[0];
                if (const Port* in = node.node->Input("in"))
                {
                    slot.in.source = Source{ in, node.scope };
                    slot.in.varying = converter.Prepare(in, node.scope, slot.in.leaves);
                }
                slot.in.fallback = Value::Vec(3, 0.5f, 0.5f, 1.0f);
                return slot;
            };

            BakeChannel baseColor = channel("base_color", Value::Vec(3, 1.0f, 1.0f, 1.0f));
            BakeChannel alpha = channel("alpha", Value::Scalar(1.0f));
            BakeChannel metallic = channel("metallic", Value::Scalar(1.0f));
            BakeChannel roughness = channel("roughness", Value::Scalar(1.0f));
            BakeChannel occlusion = channel("occlusion", Value::Scalar(1.0f));
            BakeChannel emissive = channel("emissive", Value::Vec(3, 0.0f, 0.0f, 0.0f));
            BakeChannel emissiveStrength = channel("emissive_strength", Value::Scalar(1.0f));
            BakeChannel transmission = channel("transmission", Value::Scalar(0.0f));
            BakeChannel specular = channel("specular", Value::Scalar(1.0f));
            BakeChannel specularColor = channel("specular_color", Value::Vec(3, 1.0f, 1.0f, 1.0f));
            BakeChannel sheenColor = channel("sheen_color", Value::Vec(3, 0.0f, 0.0f, 0.0f));
            BakeChannel sheenRoughness = channel("sheen_roughness", Value::Scalar(0.0f));
            BakeChannel clearcoat = channel("clearcoat", Value::Scalar(0.0f));
            BakeChannel clearcoatRoughness = channel("clearcoat_roughness", Value::Scalar(0.0f));
            BakeChannel iridescence = channel("iridescence", Value::Scalar(0.0f));
            BakeChannel iridescenceThickness = channel("iridescence_thickness", Value::Scalar(100.0f));
            BakeChannel anisotropyStrength = channel("anisotropy_strength", Value::Scalar(0.0f));
            BakeChannel anisotropyRotation = channel("anisotropy_rotation", Value::Scalar(0.0f));
            NormalSlot normal = normalSlot(normalName);
            NormalSlot coatNormal = normalSlot(coatNormalName);

            auto scalarConstant = [&](const char* name, float fallback) {
                BakeChannel bake = channel(name, Value::Scalar(fallback));
                if (bake.varying)
                    result.Warnings.push_back(std::string("'") + name + "' varies over the surface: its value at the UV centre is used");
                return constant(bake)[0];
            };

            // Factors (constants) first; each texture below overrides its slot's factors.
            const Value baseColorValue = constant(baseColor);
            data.BaseColorFactor = glm::vec4(baseColorValue[0], baseColorValue[1], baseColorValue[2], constant(alpha)[0]);
            data.MetallicFactor = constant(metallic)[0];
            data.RoughnessFactor = constant(roughness)[0];
            const Value emissiveValue = constant(emissive);
            data.EmissiveFactor = glm::vec3(emissiveValue[0], emissiveValue[1], emissiveValue[2]);
            data.emissiveStrength = scalarConstant("emissive_strength", 1.0f);
            (void)emissiveStrength;
            data.TransmissionFactor = constant(transmission)[0];
            data.IOR = std::max(scalarConstant("ior", 1.5f), 1.0f); // glTF's IOR is at least 1 (Standard Surface allows 0: no specular)
            data.SpecularStrength = constant(specular)[0];
            const Value specularColorValue = constant(specularColor);
            data.SpecularColorFactor = glm::vec3(specularColorValue[0], specularColorValue[1], specularColorValue[2]);
            const Value sheenValue = constant(sheenColor);
            data.SheenColorFactor = glm::vec3(sheenValue[0], sheenValue[1], sheenValue[2]);
            data.SheenRoughnessFactor = constant(sheenRoughness)[0];
            data.ClearcoatFactor = constant(clearcoat)[0];
            data.ClearcoatRoughnessFactor = constant(clearcoatRoughness)[0];
            data.IridescenceFactor = constant(iridescence)[0];
            data.IridescenceIOR = scalarConstant("iridescence_ior", 1.3f);
            data.IridescenceThicknessMinimum = data.IridescenceThicknessMaximum = constant(iridescenceThickness)[0];
            data.AnisotropyStrength = std::clamp(constant(anisotropyStrength)[0], 0.0f, 1.0f);
            data.AnisotropyRotation = constant(anisotropyRotation)[0];
            if (gltf.contains("dispersion"))
                data.Dispersion = scalarConstant("dispersion", 0.0f);
            // A volume (the translation's thickness: 1 unless thin-walled) only where light goes through.
            data.Thickness = data.TransmissionFactor > 0.0f ? scalarConstant("thickness", 0.0f) : 0.0f;
            const Value attenuation = constant(channel("attenuation_color", Value::Vec(3, 1.0f, 1.0f, 1.0f)));
            data.AttenuationColor = glm::vec3(attenuation[0], attenuation[1], attenuation[2]);
            data.AttenuationDistance = scalarConstant("attenuation_distance", 0.0f);
            if (normal.present)
                data.NormalScale = normal.scale;
            if (coatNormal.present)
                data.ClearcoatNormalScale = coatNormal.scale;

            // Alpha: the shader's mode (glTF PBR, the translation's from the opacity), blending wherever the opacity varies.
            int alphaMode = int(scalarConstant("alpha_mode", 0.0f) + 0.5f);
            if (alpha.varying)
                alphaMode = 2;
            if (category == "UsdPreviewSurface" && scalarConstant("alpha_cutoff", 0.0f) > 0.0f)
                alphaMode = 1;
            data.Mode = alphaMode == 1 ? AlphaMode::Mask : alphaMode == 2 ? AlphaMode::Blend : AlphaMode::Opaque;
            data.AlphaMaskCutoff = gltf.contains("alpha_cutoff") ? scalarConstant("alpha_cutoff", 0.5f) : 0.5f;
            if (category == "UsdPreviewSurface" && data.Mode == AlphaMode::Mask && data.AlphaMaskCutoff <= 0.0f)
                data.Mode = AlphaMode::Opaque;
            // Thin-walled surfaces are seen from both sides.
            if (category == "standard_surface")
                data.DoubleSided = converter.Constant(shaderInput("thin_walled"), Value::Scalar(0.0f))[0] != 0.0f;
            else if (category == "open_pbr_surface")
                data.DoubleSided = converter.Constant(shaderInput("geometry_thin_walled"), Value::Scalar(0.0f))[0] != 0.0f;

            // The textures, in glTF's channel layout.
            float iridescenceMaximum = data.IridescenceThicknessMaximum;
            std::vector<BakeTexture> textures;
            auto addTexture = [&](std::string slot, std::vector<BakeChannel> inputs, bool srgb, std::function<void(const Value*, float*)> pack) {
                bool any = false;
                for (const BakeChannel& input : inputs)
                    any = any || input.varying;
                if (!any)
                    return false;
                textures.push_back(BakeTexture{ std::move(slot), std::move(inputs), std::move(pack), srgb });
                return true;
            };
            int baseColorTexture = -1, metallicRoughnessTexture = -1, normalTexture = -1, occlusionTexture = -1, emissiveTexture = -1,
                transmissionTexture = -1, specularTexture = -1, sheenTexture = -1, clearcoatTexture = -1, coatNormalTexture = -1,
                iridescenceTexture = -1, anisotropyTexture = -1;
            if (addTexture("BaseColor", { baseColor, alpha }, true, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[0][1]; out[2] = in[0][2]; out[3] = in[1][0]; }))
                baseColorTexture = int(textures.size()) - 1;
            if (addTexture("MetallicRoughness", { roughness, metallic }, false, [](const Value* in, float* out) { out[0] = 1.0f; out[1] = in[0][0]; out[2] = in[1][0]; out[3] = 1.0f; }))
                metallicRoughnessTexture = int(textures.size()) - 1;
            if (normal.present && addTexture("Normal", { normal.in }, false, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[0][1]; out[2] = in[0][2]; out[3] = 1.0f; }))
                normalTexture = int(textures.size()) - 1;
            if (addTexture("Occlusion", { occlusion }, false, [](const Value* in, float* out) { out[0] = out[1] = out[2] = in[0][0]; out[3] = 1.0f; }))
                occlusionTexture = int(textures.size()) - 1;
            if (addTexture("Emissive", { emissive }, true, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[0][1]; out[2] = in[0][2]; out[3] = 1.0f; }))
                emissiveTexture = int(textures.size()) - 1;
            if (addTexture("Transmission", { transmission }, false, [](const Value* in, float* out) { out[0] = out[1] = out[2] = in[0][0]; out[3] = 1.0f; }))
                transmissionTexture = int(textures.size()) - 1;
            if (addTexture("Specular", { specularColor, specular }, true, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[0][1]; out[2] = in[0][2]; out[3] = in[1][0]; }))
                specularTexture = int(textures.size()) - 1;
            if (addTexture("Sheen", { sheenColor, sheenRoughness }, true, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[0][1]; out[2] = in[0][2]; out[3] = in[1][0]; }))
                sheenTexture = int(textures.size()) - 1;
            if (addTexture("Clearcoat", { clearcoat, clearcoatRoughness }, false, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[1][0]; out[2] = 0.0f; out[3] = 1.0f; }))
                clearcoatTexture = int(textures.size()) - 1;
            if (coatNormal.present && addTexture("ClearcoatNormal", { coatNormal.in }, false, [](const Value* in, float* out) { out[0] = in[0][0]; out[1] = in[0][1]; out[2] = in[0][2]; out[3] = 1.0f; }))
                coatNormalTexture = int(textures.size()) - 1;
            if (iridescenceThickness.varying)
            {
                // The thickness texture spans 0 to the thickest found (a coarse look first).
                EvalState probe;
                probe.stamp.assign(size_t(converter.NodeCount()), 0);
                probe.memo.resize(size_t(converter.NodeCount()));
                iridescenceMaximum = 0.0f;
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x)
                    {
                        ++probe.current;
                        probe.u = (float(x) + 0.5f) / 64.0f;
                        probe.v = (float(y) + 0.5f) / 64.0f;
                        iridescenceMaximum = std::max(iridescenceMaximum, converter.EvalPort(*iridescenceThickness.source.port, *iridescenceThickness.source.scope, probe)[0]);
                    }
                iridescenceMaximum = std::max(iridescenceMaximum, 1.0f);
            }
            if (addTexture("Iridescence", { iridescence, iridescenceThickness }, false, [iridescenceMaximum](const Value* in, float* out) {
                    out[0] = in[0][0]; out[1] = in[1][0] / iridescenceMaximum; out[2] = 0.0f; out[3] = 1.0f; }))
                iridescenceTexture = int(textures.size()) - 1;
            if (addTexture("Anisotropy", { anisotropyStrength, anisotropyRotation }, false, [](const Value* in, float* out) {
                    out[0] = 0.5f + 0.5f * std::cos(in[1][0]); out[1] = 0.5f + 0.5f * std::sin(in[1][0]); out[2] = std::clamp(in[0][0], 0.0f, 1.0f); out[3] = 1.0f; }))
                anisotropyTexture = int(textures.size()) - 1;

            // Bake each texture (rows over the threads), in the texture space of its one tiled image where it has one.
            std::vector<MaterialUVChannel> channels = { MaterialUVChannel{ 0 } };
            std::vector<std::string> texturePaths(textures.size());
            std::vector<int> textureChannels(textures.size(), 0);
            for (size_t t = 0; t < textures.size(); ++t)
            {
                BakeTexture& texture = textures[t];
                TextureSpace space;
                bool textureSpace = true, sawImage = false;
                int width = 0, height = 0;
                float maxTilingX = 1.0f, maxTilingY = 1.0f;
                std::set<const Image*> images;
                for (const BakeChannel& input : texture.Inputs)
                {
                    if (!input.varying)
                        continue;
                    if (input.leaves.OtherVarying)
                        textureSpace = false;
                    for (const Node* node : input.leaves.Images)
                    {
                        const Image* image = converter.NodeImage(node);
                        if (!image)
                            continue;
                        images.insert(image);
                        width = std::max(width, image->Width);
                        height = std::max(height, image->Height);
                        TextureSpace own;
                        own.Valid = true;
                        if (node->Category != "image")
                        {
                            const Value tiling = node->Input("uvtiling") ? converter.Constant(Source{ node->Input("uvtiling"), &converter.UserRoot() }, Value::Vec(2, 1.0f, 1.0f)) : Value::Vec(2, 1.0f, 1.0f);
                            const Value offset = node->Input("uvoffset") ? converter.Constant(Source{ node->Input("uvoffset"), &converter.UserRoot() }, Value::Vec(2, 0.0f, 0.0f)) : Value::Vec(2, 0.0f, 0.0f);
                            own.TilingX = tiling[0]; own.TilingY = tiling[1]; own.OffsetX = offset[0]; own.OffsetY = offset[1];
                        }
                        maxTilingX = std::max(maxTilingX, std::fabs(own.TilingX));
                        maxTilingY = std::max(maxTilingY, std::fabs(own.TilingY));
                        if (!sawImage)
                            space = own;
                        else if (!space.SameTransform(own))
                            textureSpace = false;
                        sawImage = true;
                    }
                }
                if (!sawImage || space.TilingX == 0.0f || space.TilingY == 0.0f)
                    textureSpace = false;
                int channelIndex = 0;
                if (textureSpace)
                {
                    const MaterialUVChannel wanted = space.Channel();
                    const auto found = std::find(channels.begin(), channels.end(), wanted);
                    if (found != channels.end())
                        channelIndex = int(found - channels.begin());
                    else if (channels.size() < MaxMaterialUVChannels)
                    {
                        channels.push_back(wanted);
                        channelIndex = int(channels.size()) - 1;
                    }
                    else
                        textureSpace = false;
                }
                if (!textureSpace)
                {
                    channelIndex = 0;
                    if (sawImage)
                    {
                        width = int(std::min(2048.0f, std::max(256.0f, float(width) * maxTilingX)));
                        height = int(std::min(2048.0f, std::max(256.0f, float(height) * maxTilingY)));
                    }
                    else
                        width = height = 1024;
                }
                width = std::max(width, 4);
                height = std::max(height, 4);

                std::vector<Value> constants(texture.Inputs.size());
                for (size_t i = 0; i < texture.Inputs.size(); ++i)
                    if (!texture.Inputs[i].varying)
                        constants[i] = converter.Constant(texture.Inputs[i].source, texture.Inputs[i].fallback);

                std::vector<uint8_t> rgba(size_t(width) * height * 4);
                const unsigned threadCount = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
                std::vector<std::thread> threads;
                for (unsigned worker = 0; worker < threadCount; ++worker)
                {
                    threads.emplace_back([&, worker]() {
                        EvalState state;
                        state.stamp.assign(size_t(converter.NodeCount()), 0);
                        state.memo.resize(size_t(converter.NodeCount()));
                        std::vector<Value> values(texture.Inputs.size());
                        for (int y = int(worker); y < height; y += int(threadCount))
                            for (int x = 0; x < width; ++x)
                            {
                                const float s = (float(x) + 0.5f) / float(width), tt = (float(y) + 0.5f) / float(height);
                                if (textureSpace)
                                {
                                    // The mesh UV this texel of the image is read at (the inverse of the tiled image's transform).
                                    state.u = (s + space.OffsetX) / space.TilingX;
                                    state.v = 1.0f - (1.0f - tt + space.OffsetY) / space.TilingY;
                                }
                                else
                                {
                                    state.u = s;
                                    state.v = tt;
                                }
                                ++state.current;
                                for (size_t i = 0; i < texture.Inputs.size(); ++i)
                                    values[i] = texture.Inputs[i].varying
                                                    ? converter.EvalPort(*texture.Inputs[i].source.port, *texture.Inputs[i].source.scope, state)
                                                    : constants[i];
                                float out[4];
                                texture.Pack(values.data(), out);
                                uint8_t* pixel = &rgba[(size_t(y) * width + x) * 4];
                                for (int c = 0; c < 4; ++c)
                                {
                                    const float value = (texture.SrgbColor && c < 3) ? LinearToSrgb(out[c]) : std::clamp(out[c], 0.0f, 1.0f);
                                    pixel[c] = uint8_t(std::lround(value * 255.0f));
                                }
                            }
                    });
                }
                for (std::thread& thread : threads)
                    thread.join();

                // The same everywhere after all (an output that only depends on an image through a branch not taken: the anisotropy
                // from a roughness texture with no anisotropy): no texture, the factors (its value at the UV centre) stand.
                bool uniform = true;
                for (size_t i = 4; uniform && i < rgba.size(); i += 4)
                    uniform = std::memcmp(&rgba[i], &rgba[0], 4) == 0;
                if (uniform)
                    continue;

                // Exactly one source image (in its own texture space): the material names the image instead of a copy.
                std::filesystem::path written;
                if (textureSpace && images.size() == 1)
                {
                    const Image* source = *images.begin();
                    std::error_code relativeError;
                    const std::filesystem::path relative = std::filesystem::relative(source->Path, assetDirectory, relativeError);
                    if (!relativeError && !relative.empty() && relative.native()[0] != '.' && SameAsImageFile(source->Path, width, height, rgba))
                        written = relative;
                }
                if (written.empty())
                {
                    written = outputFolder / "Textures" / (Sanitize(material->Name) + "_" + texture.Slot + ".png");
                    if (!WritePng(assetDirectory / written, width, height, rgba))
                    {
                        result.Warnings.push_back("cannot write " + written.generic_string());
                        continue;
                    }
                }
                texturePaths[t] = written.lexically_normal().generic_string();
                textureChannels[t] = channelIndex;
            }
            for (size_t i = 0; i < channels.size() && i < MaxMaterialUVChannels; ++i)
                data.UVChannels[i] = channels[i];

            auto use = [&](int index, std::string& path, int32_t& set) {
                if (index < 0 || texturePaths[size_t(index)].empty())
                    return false;
                path = texturePaths[size_t(index)];
                set = textureChannels[size_t(index)];
                return true;
            };
            auto useLobe = [&](int index, MaterialTexture& texture) { return use(index, texture.Path, texture.Set); };
            if (use(baseColorTexture, data.BaseColorTexturePath, data.BaseColorTextureSet))
                data.BaseColorFactor = glm::vec4(1.0f);
            if (use(metallicRoughnessTexture, data.MetallicRoughnessTexturePath, data.PhysicalDescriptorTextureSet))
            {
                data.MetallicFactor = 1.0f;
                data.RoughnessFactor = 1.0f;
            }
            use(normalTexture, data.NormalTexturePath, data.NormalTextureSet);
            use(occlusionTexture, data.OcclusionTexturePath, data.OcclusionTextureSet);
            if (use(emissiveTexture, data.EmissiveTexturePath, data.EmissiveTextureSet))
                data.EmissiveFactor = glm::vec3(1.0f);
            if (use(transmissionTexture, data.TransmissionTexturePath, data.TransmissionTextureSet))
                data.TransmissionFactor = 1.0f;
            if (useLobe(specularTexture, data.SpecularColorTexture))
            {
                data.SpecularStrengthTexture = data.SpecularColorTexture;
                data.SpecularStrength = 1.0f;
                data.SpecularColorFactor = glm::vec3(1.0f);
            }
            if (useLobe(sheenTexture, data.SheenColorTexture))
            {
                data.SheenRoughnessTexture = data.SheenColorTexture;
                data.SheenColorFactor = glm::vec3(1.0f);
                data.SheenRoughnessFactor = 1.0f;
            }
            if (useLobe(clearcoatTexture, data.ClearcoatTexture))
            {
                data.ClearcoatRoughnessTexture = data.ClearcoatTexture;
                data.ClearcoatFactor = 1.0f;
                data.ClearcoatRoughnessFactor = 1.0f;
            }
            useLobe(coatNormalTexture, data.ClearcoatNormalTexture);
            if (useLobe(iridescenceTexture, data.IridescenceTexture))
            {
                data.IridescenceThicknessTexture = data.IridescenceTexture;
                data.IridescenceFactor = 1.0f;
                data.IridescenceThicknessMinimum = 0.0f;
                data.IridescenceThicknessMaximum = iridescenceMaximum;
            }
            if (useLobe(anisotropyTexture, data.AnisotropyTexture))
            {
                data.AnisotropyStrength = 1.0f;
                data.AnisotropyRotation = 0.0f;
            }
            outMaterials.push_back(std::move(result));
        }
        return true;
    }
}
