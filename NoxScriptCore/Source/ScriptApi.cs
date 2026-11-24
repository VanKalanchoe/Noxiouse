using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Threading;

namespace Nox;

/// <summary>What the engine asks Nox.ScriptCore about itself.</summary>
internal static class ScriptApi
{
    // Raised whenever the internal calls change (InternalCalls.cs <-> DotNetBackend.cpp RegisterInternalCalls): the engine refuses
    // a Nox.ScriptCore of another version instead of calling functions that moved. Must equal kScriptApiVersion in DotNetBackend.cpp.
    public const int Version = 23;

    internal static int GetVersion() => Version;

    // Enum member names and numeric values ("Name=Value\n...") for [Expose] enum fields/properties in the Inspector.
    internal static string GetEnumMembers(string typeFullName)
    {
        var alc = AssemblyLoadContext.GetLoadContext(typeof(ScriptApi).Assembly);
        if (alc == null)
            return string.Empty;

        foreach (var asm in alc.Assemblies)
        {
            var type = asm.GetType(typeFullName, throwOnError: false);
            if (type == null || !type.IsEnum)
                continue;

            var names = Enum.GetNames(type);
            var values = Enum.GetValues(type);
            var sb = new StringBuilder();
            sb.Append(Marshal.SizeOf(Enum.GetUnderlyingType(type)));
            for (int i = 0; i < names.Length; i++)
                sb.Append('\n').Append(names[i]).Append('=').Append(Convert.ToInt64(values.GetValue(i)));
            return sb.ToString();
        }
        return string.Empty;
    }

    // Wires a C# event (System.Action) on source to a public parameterless method on target (Phase 5 Inspector event bindings).
    internal static int SubscribeEvent(long sourceHandle, string eventName, long targetHandle, string methodName)
    {
        var source = GCHandle.FromIntPtr((IntPtr)sourceHandle).Target;
        var target = GCHandle.FromIntPtr((IntPtr)targetHandle).Target;
        if (source == null || target == null)
            return 0;

        var ev = source.GetType().GetEvent(eventName, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance);
        if (ev == null || ev.EventHandlerType == null)
            return 0;

        var method = target.GetType().GetMethod(methodName, BindingFlags.Public | BindingFlags.Instance, null, Type.EmptyTypes, null);
        if (method == null)
            return 0;

        var del = Delegate.CreateDelegate(ev.EventHandlerType, target, method, throwOnBindFailure: false);
        if (del == null)
            return 0;

        ev.AddEventHandler(source, del);
        return 1;
    }

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
