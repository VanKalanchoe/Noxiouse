#pragma once
#include "NetProtocol.h"

// What the active project is to the network (docs/Networking_Plan_2026-11.md, NET1): its name and a checksum of the content a
// client must share with the server -- the game's script assemblies (Nox.ScriptCore and the project's own: replicated fields and
// RPCs come from them; a rebuilt script on one side is a different game). Two runtimes from the same build of a project match;
// a client with other scripts is refused with DisconnectReason::ContentMismatch.
namespace Nox::Net
{
    // Call after Project::Load and ScriptEngine::Initialize (the assemblies are found the way the script engine finds them).
    NetCompatibility MakeProjectCompatibility();
}
