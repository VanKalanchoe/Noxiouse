using System.Runtime.InteropServices;

namespace Nox;

/// <summary>The process's memory and the asset manager's, for tests and on-screen stats.</summary>
public static class Diagnostics
{
    /// <summary>Right now (the GPU's is sampled a few times a second).</summary>
    public static MemoryStats Memory => InternalCalls.GetMemoryStats();
}

/// <summary>What <see cref="Diagnostics.Memory"/> returns.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct MemoryStats
{
    /// <summary>The process's own memory (Windows: private bytes; Linux: resident anonymous pages).</summary>
    public ulong ProcessBytes;
    /// <summary>Video memory the engine's resources hold (meshes, textures, render targets; 0 before the first sample).</summary>
    public ulong GpuBytes;
    /// <summary>Video memory the process uses in all: the allocator's blocks (kept when what was in them is freed) and the
    /// driver's own.</summary>
    public ulong GpuProcessBytes;
    /// <summary>Assets in memory.</summary>
    public int LoadedAssets;
    /// <summary>Assets being read, or textures being streamed to another resolution.</summary>
    public int LoadingAssets;
}
