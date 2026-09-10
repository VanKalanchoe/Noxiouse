#pragma once

#include "Asset.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    class Material : public Asset
    {
    public:
        Material() = default;

        AssetType GetType() const override { return AssetType::Material; }
        static AssetType GetStaticType() { return AssetType::Material; }

        const MaterialData& GetData() const { return m_Data; }
        MaterialData& GetData() { return m_Data; }

    private:
        friend class MaterialImporter;
        MaterialData m_Data;
    };
}
