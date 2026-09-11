# Antigravity Rules & User Directives

## 1. Execution Policy (Strict)
- **NEVER execute terminal commands, scripts (`.bat`, `.ps1`, shell), builds, or background tasks** unless the user explicitly commands you to execute them (e.g. "run this command", "execute the script").
- When diagnosing, troubleshooting, or fixing scripts and tools: **only inspect, fix the files, and explain the changes**. The user will run the commands/scripts themselves.
- Never spawn compilers, background tasks, or testing runs autonomously.

## 2. Architectural Guidelines
- **Zero Graphics Leak**: Vulkan headers remain strictly within `NoxCore/src/NRI/Vulkan/`.
- **Reverse-Z Projection**: Near plane is `1.0`, far plane / infinity is `0.0`. Depth comparison uses `GREATER` or `GREATER_OR_EQUAL`.
