#include "NodeType.h"

namespace Nox
{
    std::vector<NodeTypeDesc>& NodeTypeRegistry::Types()
    {
        // Function-local static: registration happens from other translation units' static initializers (e.g.
        // an AnimationGraphNodes.cpp), so this avoids static-initialization-order fiasco.
        static std::vector<NodeTypeDesc> types;
        return types;
    }

    void NodeTypeRegistry::Register(NodeTypeDesc desc)
    {
        Types().push_back(std::move(desc));
    }

    const NodeTypeDesc* NodeTypeRegistry::Find(const std::string& domain, const std::string& typeName)
    {
        for (const NodeTypeDesc& type : Types())
        {
            if (type.Domain == domain && type.TypeName == typeName)
                return &type;
        }
        return nullptr;
    }

    void RegisterRerouteNodeType(const std::string& domain)
    {
        NodeTypeDesc desc;
        desc.TypeName = kRerouteNodeType;
        desc.Domain = domain;
        desc.Category = "Routing";
        desc.DefaultProperties.emplace_back(kRerouteTypeProperty, std::string());
        desc.NodePins = [](const std::unordered_map<std::string, NodeGraphValue>& properties, NodePinList& out)
        {
            const auto found = properties.find(kRerouteTypeProperty);
            const std::string* type = found != properties.end() ? std::get_if<std::string>(&found->second) : nullptr;
            const std::string pinType = type ? *type : std::string();
            out.Inputs = { { "In", pinType } };
            out.Outputs = { { "Out", pinType } };
        };
        NodeTypeRegistry::Register(std::move(desc));
    }

    const std::vector<NodeTypeDesc>& NodeTypeRegistry::All()
    {
        return Types();
    }
}
