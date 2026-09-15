#include "FrameArena.h"

#include <algorithm>

namespace Nox
{
    void* FrameArena::Allocate(size_t size, size_t alignment)
    {
        while (true)
        {
            if (m_BlockIndex < m_Blocks.size())
            {
                Block& block = m_Blocks[m_BlockIndex];
                const size_t alignedOffset = (m_Offset + alignment - 1) & ~(alignment - 1);
                if (alignedOffset + size <= block.Size)
                {
                    m_Offset = alignedOffset + size;
                    return block.Memory.get() + alignedOffset;
                }

                // Next reserved block (kept from earlier frames) or a new one.
                ++m_BlockIndex;
                m_Offset = 0;
                continue;
            }

            // new[] of std::byte is aligned for any fundamental alignment; larger alignments are not used here.
            const size_t blockSize = std::max(DefaultBlockSize, size + alignment);
            m_Blocks.push_back({ std::make_unique<std::byte[]>(blockSize), blockSize });
        }
    }

    void FrameArena::Reset()
    {
        m_BlockIndex = 0;
        m_Offset = 0;
    }
}
