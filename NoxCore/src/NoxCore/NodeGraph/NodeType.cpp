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

    const std::vector<NodeTypeDesc>& NodeTypeRegistry::All()
    {
        return Types();
    }
}
