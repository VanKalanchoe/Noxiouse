#include "Mesh.h"

#include "Renderer.h"

namespace Nox
{
    StaticMesh::~StaticMesh()
    {
        // When the Mesh object dies, release all its submesh allocations from the GPU allocators
        for (const MeshHandle& handle : m_SubMeshes)
        {
            if (handle.IsValid())
            {
                Renderer::UnloadMesh(handle); // Static helper calling s_Instance->UnloadMeshGeometry(handle)
            }
        }
    }
    
    Mesh::~Mesh()
    {
        // When the Mesh object dies, release all its submesh allocations from the GPU allocators
        for (const MeshHandle& handle : m_SubMeshes)
        {
            if (handle.IsValid())
            {
                Renderer::UnloadMesh(handle); // Static helper calling s_Instance->UnloadMeshGeometry(handle)
            }
        }
    }
}
