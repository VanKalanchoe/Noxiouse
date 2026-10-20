using System.Diagnostics;
using System.Threading;

namespace Nox;

/// <summary>What the engine asks Nox.ScriptCore about itself.</summary>
internal static class ScriptApi
{
    // Raised whenever the internal calls change (InternalCalls.cs <-> DotNetBackend.cpp RegisterInternalCalls): the engine refuses
    // a Nox.ScriptCore of another version instead of calling functions that moved. Must equal kScriptApiVersion in DotNetBackend.cpp.
    public const int Version = 2;

    internal static int GetVersion() => Version;

    // Script > Wait for Debugger: Play pauses here until a .NET debugger attaches to the editor (Visual Studio: Debug > Attach to
    // Process, code type Managed (.NET Core); Rider: Attach to Process), at most timeoutMilliseconds. 1 once one is attached.
    internal static int WaitForDebugger(int timeoutMilliseconds)
    {
        var clock = Stopwatch.StartNew();
        while (!Debugger.IsAttached && clock.ElapsedMilliseconds < timeoutMilliseconds)
            Thread.Sleep(100);
        if (Debugger.IsAttached)
            Debugger.Break(); // stops here: set breakpoints in the scripts, then continue
        return Debugger.IsAttached ? 1 : 0;
    }
}
