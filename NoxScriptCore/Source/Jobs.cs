namespace Nox;

/// <summary>
/// Work off the main thread, on .NET's thread pool (every core): terrain noise, meshing a chunk, path finding. A job computes
/// data only -- engine calls (entities, transforms, meshes, physics) are refused from a job thread. Start it in OnUpdate, check
/// <see cref="JobHandle{T}.IsCompleted"/> in a later OnUpdate and apply the result there (Unity's Jobs pattern):
/// <code>
/// m_Job = Jobs.Run(() => BuildChunk(x, z));           // returns a MeshBuilder
/// ...
/// if (m_Job.TryGetResult(out MeshBuilder chunk))
///     m_Mesh.SetData(chunk);
/// </code>
/// </summary>
public static class Jobs
{
    /// <summary>How many jobs run at the same time at most (the machine's logical cores).</summary>
    public static int WorkerCount => Environment.ProcessorCount;

    public static JobHandle Run(Action work) => new(Task.Run(work));

    public static JobHandle<T> Run<T>(Func<T> work) => new(Task.Run(work));

    /// <summary>
    /// Runs <paramref name="body"/> for every index in [0, count) spread over the cores, and returns when all are done (one
    /// frame's data-parallel work: a 16x16 column of noise per index).
    /// </summary>
    public static void ParallelFor(int count, Action<int> body) => Parallel.For(0, count, body);
}

/// <summary>A job started with <see cref="Jobs.Run(Action)"/>.</summary>
public readonly struct JobHandle
{
    private readonly Task? m_Task;

    internal JobHandle(Task task) => m_Task = task;

    public bool IsCompleted => m_Task?.IsCompleted ?? true;
    public bool IsFaulted => m_Task?.IsFaulted ?? false;

    /// <summary>Waits for the job (blocking the frame) and rethrows what it threw.</summary>
    public void Complete() => m_Task?.GetAwaiter().GetResult();
}

/// <summary>A job started with <see cref="Jobs.Run{T}(Func{T})"/>: its result once it is done.</summary>
public readonly struct JobHandle<T>
{
    private readonly Task<T>? m_Task;

    internal JobHandle(Task<T> task) => m_Task = task;

    public bool IsValid => m_Task != null;
    public bool IsCompleted => m_Task?.IsCompleted ?? false;
    public bool IsFaulted => m_Task?.IsFaulted ?? false;

    /// <summary>Waits for the job (blocking the frame) and returns its result, or rethrows what it threw.</summary>
    public T Complete() => m_Task != null ? m_Task.GetAwaiter().GetResult() : throw new InvalidOperationException("The job was never started.");

    /// <summary>The result when the job finished successfully (without waiting); a job that threw rethrows here.</summary>
    public bool TryGetResult(out T result)
    {
        if (m_Task == null || !m_Task.IsCompleted)
        {
            result = default!;
            return false;
        }
        result = m_Task.GetAwaiter().GetResult();
        return true;
    }
}
