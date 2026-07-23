# NRI and Render Graph Architecture Plan (v2)

## 1. Introduction

This document proposes a modern C++23 architecture for the **Nox Rendering Interface (NRI)** and a Render Graph. This design aims for modularity, extensibility, and performance, using RAII principles and abstracting different graphics APIs.

This revised plan incorporates your feedback on naming, command buffer abstraction, modern C++ usage, API selection, and a more advanced descriptor buffer model.

## 2. Code Structure: The Importance of Separation

As discussed, **using separate files is essential**. A single large file is not scalable. This design separates the **NRI (interfaces)**, **backends (Vulkan/Metal)**, and the **scheduling (Render Graph/Task Graph)**.

## 3. Proposed Directory Structure

```
NoxCore/
└── src/
    ├── ...
    ├── NRI/
    │   ├── NRI.h                     // Core NRI interfaces and types
    │   ├── CommandPool.h             // NRI Command Pool interface
    │   ├── CommandBuffer.h           // NRI Command Buffer interface
    │   ├── Device.h                  // NRI Device interface
    │   ├── Resources.h               // NRI resource interfaces (Texture, Buffer)
    │   ├── Pipeline.h                // NRI pipeline state objects
    │   │
    │   └── vulkan/                   // Vulkan NRI backend
    │       ├── DeviceVK.h/.cpp
    │       ├── CommandPoolVK.h/.cpp
    │       ├── CommandBufferVK.h/.cpp
    │       ├── ResourcesVK.h/.cpp
    │       └── PipelineVK.h/.cpp
    │
    ├── RenderGraph/
    │   ├── RenderGraph.h/.cpp        // The render graph builder (builds a DAG)
    │   ├── RGPass.h/.cpp
    │   └── RGResource.h/.cpp
    │
    └── Tasking/
        ├── TaskGraph.h/.cpp          // Generic task graph system (uses Taskflow internally)
        └── TaskExecutor.h/.cpp
```

## 4. NRI Abstraction Layer

### API Selection & Device Creation

To select an API, we'll use a factory function on the `Device` interface. This promotes a clean initialization flow.

```cpp
// NRI/Device.h
#pragma once

#include <memory>
#include "Resources.h"
#include "Pipeline.h"

namespace NRI {

enum class API {
    Vulkan,
    Metal // Future
};

class Device {
public:
    // Factory function to create a device for a specific API
    static std::unique_ptr<Device> create(API api);

    virtual ~Device() = default;

    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc) = 0;
    // ... other resource creation methods

    // Get raw buffer of descriptors for bindless access
    virtual Buffer* getDescriptorBuffer() = 0;
};

} // namespace NRI
```

### Command Buffer Abstraction

Instead of a monolithic `VulkanCommand`, we'll have a finer-grained abstraction matching modern APIs.

```cpp
// NRI/CommandBuffer.h
#pragma once
#include <cstdint>

namespace NRI {

class CommandBuffer {
public:
    virtual ~CommandBuffer() = default;

    virtual void begin() = 0;
    virtual void end() = 0;
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
    // ... other commands
};

class CommandPool {
public:
    virtual ~CommandPool() = default;
    virtual std::unique_ptr<CommandBuffer> acquireCommandBuffer() = 0;
};

} // namespace NRI
```
This maps well to Vulkan's `VkCommandPool` and `VkCommandBuffer` and provides a clean RAII-based lifecycle for command buffers.

## 5. Modern Bindless with Descriptor Buffers

Your comment on descriptor heaps being "just memory now" is spot on for modern Vulkan using `VK_EXT_descriptor_buffer`. This extension simplifies bindless rendering significantly.

The RHI abstraction will reflect this by treating descriptors as data within an `NRI::Buffer`.

### How it works:
1.  **Create a Large Buffer:** On startup, the `DeviceVK` will create a large `VkBuffer` with the `VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT` and `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`. This buffer is your "descriptor buffer".
2.  **Get Descriptor Addresses:** When you want to get a descriptor for a texture (e.g., a `VkImageView`), you don't create a `VkDescriptorSet`. Instead, you use `vkGetDescriptorEXT` to write the descriptor information directly into your descriptor buffer at a specific offset.
3.  **Pass Buffer Address to Shaders:** You pass the device address of this descriptor buffer to your shaders (e.g., via a push constant or a single uniform buffer).
4.  **Shader Access:** In GLSL, you declare `buffer_reference` types and use offsets to access the descriptors.

```glsl
// Simplified GLSL
layout(buffer_reference, buffer_reference_align=4) readonly buffer DescriptorBuffer {
    sampler2D textures[];
};

layout(push_constant) uniform PushConstants {
    layout(offset=0) uint64_t descriptorBufferAddress;
    layout(offset=8) uint32_t textureIndex;
};

void main() {
    DescriptorBuffer descBuffer = DescriptorBuffer(descriptorBufferAddress);
    vec4 color = texture(descBuffer.textures[textureIndex], uv);
    // ...
}
```
This approach is much more aligned with modern GPU architectures and is the future of resource binding in Vulkan.

## 6. Abstracted Task Graph

To keep the `RenderGraph` independent of any specific tasking library, we introduce a `TaskGraph` abstraction.

1.  **`RenderGraph` builds a DAG:** The `RenderGraph`'s `compile()` method will produce a generic Directed Acyclic Graph (DAG) of `RGPass` nodes, representing the frame's dependencies.
2.  **`TaskGraph` executes the DAG:** You pass this DAG to a `TaskGraph` system. This system is responsible for translating the generic DAG into a `tf::Taskflow` (or any other backend) and executing it.

```cpp
// Tasking/TaskGraph.h
#pragma once

#include <vector>
#include <functional>

// A generic node for our task graph
struct TaskNode {
    std::function<void()> execute;
    std::vector<TaskNode*> dependencies;
};

class TaskGraph {
public:
    // Takes a list of nodes (our DAG) and executes them
    void submit(const std::vector<TaskNode>& nodes);
};
```

This design decouples your rendering logic from the execution library, allowing you to swap out Taskflow in the future if needed, without changing the `RenderGraph` at all.

## 7. Next Steps

The next step is to implement this v2 plan. I am ready to proceed with generating the code for these files.





-----------








# NRI and Render Graph Architecture Plan (v3)

## 1. Introduction & Core Philosophy

This plan (v3) incorporates critical feedback to build a robust, production-ready rendering architecture. The core philosophy is now:

**The RenderGraph is a *compiler*, not just a scheduler.**

It compiles a user-defined graph of high-level render passes into an optimized, low-level Directed Acyclic Graph (DAG) of GPU commands, with automatically managed resource states and barriers.

-   **NRI (Nox Rendering Interface)**: The low-level "GPU assembly language". Provides the primitive operations.
-   **RenderGraph**: The "compiler". Manages resource state, inserts barriers, and builds the command sequence.
-   **TaskGraph**: The "executor". Schedules the compiled command sequence across CPU and GPU hardware queues.

---

## 2. The NRI: A Richer, Safer Abstraction

The NRI needs to be expressive enough for the RenderGraph compiler to target.

### Resource State Model

This is the most critical addition.

```cpp
// In NRI/NRI.h
namespace NRI {
    enum class ResourceState {
        eUndefined,
        eGeneral,
        eReadOnly,          // For SRV (read-only texture/buffer)
        eRenderTarget,
        eDepthWrite,
        eDepthRead,
        eCopySrc,
        eCopyDst,
        ePresent
    };
}
```

### The `CommandList` and RAII Scopes

The flat `CommandBuffer` is replaced by a `CommandList` that uses RAII to manage state, preventing mistakes.

```cpp
// In NRI/CommandList.h

namespace NRI {

// RAII object for safely managing a render pass
class RenderPassScope {
public:
    // Disallow copy
    RenderPassScope(const RenderPassScope&) = delete;
    RenderPassScope& operator=(const RenderPassScope&) = delete;

    // Drawing commands are methods on the scope
    void setPipeline(const Pipeline& pipeline);
    void setVertexBuffer(const Buffer& buffer, uint32_t binding = 0);
    void draw(uint32_t vertexCount, ...);
    // ...

private:
    friend class CommandList;
    RenderPassScope(CommandList& cmdList, const RenderPassDesc& desc);
    ~RenderPassScope(); // Ends the render pass on destruction

    CommandList& m_commandList;
};

// The main command recording interface
class CommandList {
public:
    virtual void begin() = 0;
    virtual void end() = 0;

    // The core of the new design: insert a barrier
    virtual void resourceBarrier(const Resource& resource, ResourceState before, ResourceState after) = 0;

    // Returns an RAII scope object
    [[nodiscard]] virtual RenderPassScope beginRenderPass(const RenderPassDesc& desc) = 0;

    // ... other commands like copy, compute dispatch, etc.
};

}
```
**Usage:**
```cpp
// This is what the *RenderGraph* would generate, not what a user would write
commandList->begin();
commandList->resourceBarrier(myTexture, eGeneral, eRenderTarget);
{
    // beginRenderPass returns a scope object. Pass begins here.
    auto rpScope = commandList->beginRenderPass(renderPassDesc);
    rpScope.setPipeline(myPipeline);
    rpScope.draw(3, 1, 0, 0);
} // ~RenderPassScope() called here, pass ends automatically.
commandList->resourceBarrier(myTexture, eRenderTarget, eReadOnly);
commandList->end();
```

---

## 3. The RenderGraph: The State Compiler

The `RenderGraph` is where the intelligence lives.

### `RGResource` and `RGPass`
These are the high-level, user-facing parts of the graph builder.

```cpp
// In RenderGraph/RGResource.h
class RGResource {
    // Represents a transient resource managed by the graph
    // (e.g. a G-Buffer texture)
};

// In RenderGraph/RGPass.h
class RGPass {
public:
    // User declares how they will use a resource
    void reads(RGResource* res, NRI::ResourceState state = NRI::ResourceState::eReadOnly);
    void writes(RGResource* res, NRI::ResourceState state = NRI::ResourceState::eRenderTarget);

    // User provides the GPU work
    void setExecuteCallback(std::function<void(NRI::CommandList&)>&& callback);
};
```

### The Compilation Process
The `RenderGraph::compile()` method is the heart of this architecture. It performs several steps:

1.  **Build Dependency Graph**: Analyzes the `reads` and `writes` declarations to build a DAG of passes.
2.  **Cull Unused Passes**: Any pass that doesn't contribute to the final output is removed.
3.  **Resource Allocation**: Allocates physical `NRI::Texture` and `NRI::Buffer` resources from the device. It can perform aliasing to save memory.
4.  **State Tracking & Barrier Insertion**: This is the key step. It iterates through the sorted passes and tracks the state of each physical resource. When a pass requires a resource in a different state than it was left in by the previous pass, it automatically injects a `commandList->resourceBarrier()` call.
5.  **Produce Final Command Sequence**: The output of `compile()` is a linear list of executable "jobs" for the `TaskGraph`.

---

## 4. The TaskGraph: A Smarter Executor

The `TaskGraph` is no longer a naive scheduler. It understands the needs of a renderer.

### `RenderJob`
The `RenderGraph` compiles into a list of `RenderJob`s, not simple `TaskNode`s.

```cpp
// In Tasking/TaskGraph.h
namespace Tasking {

enum class QueueType {
    eGraphics,
    eCompute,
    eTransfer
};

struct RenderJob {
    std::function<void(NRI::CommandList&)> execute;
    QueueType queue = QueueType::eGraphics;
    std::vector<RenderJob*> dependencies;

    // GPU-GPU synchronization
    std::vector<NRI::Semaphore*> waitSemaphores;
    std::vector<NRI::Semaphore*> signalSemaphores;
};

}
```

### Execution Flow
1.  The `RenderGraph` produces a `std::vector<RenderJob>`. This vector contains not only the drawing commands but also the **automatically generated barrier commands**.
2.  The `TaskGraph::submit()` method takes this list of jobs.
3.  It uses a tasking library like **Taskflow** to parallelize the recording of `NRI::CommandList`s on the CPU.
4.  It submits the recorded command lists to the correct `NRI::Device` hardware queues (e.g., Graphics work to the graphics queue).
5.  It manages GPU synchronization by submitting the `waitSemaphores` and `signalSemaphores` along with the command lists.

## 5. Summary: The Final Picture

This v3 architecture resolves the conceptual tension and creates a powerful, modern rendering pipeline:

-   **Users** interact with the high-level `RenderGraph`, declaring passes and their resource intentions simply.
-   The **`RenderGraph`** acts as an intelligent compiler, handling the complex, error-prone work of resource allocation, state tracking, and barrier insertion.
-   The **`TaskGraph`** acts as a sophisticated executor, efficiently scheduling the compiled work across all available CPU and GPU resources.
-   The **`NRI`** provides a clean, safe, and expressive set of low-level primitives for the `RenderGraph` to target.

This design is significantly more robust and scalable. I am now ready to guide you through implementing the v3 header files.












my version 


# NoxEngine Rendering Architecture (NRI)

This document outlines the current decoupled architectural design of the **Nox Rendering Interface (NRI)**, which abstracts explicit graphics APIs (currently tracking Vulkan 1.3 / API Version 1.4) away from the main application logic.
descriptor heaps, buffer device adress all latest tech basicly
---

## 1. Core Architectural Principles
* **Single Source of Truth**: Subsystems track only what they own. `DeviceVK` remains entirely headless after creation; it does not retain a long-term pointer to the windowing system.
* **Abstract Factory Pattern**: The high-level `Renderer` interacts exclusively with pure abstract base classes (`Device`, `Swapchain`, `CommandBuffer`). It never directly includes Vulkan headers or performs unsafe `static_cast` routines in the frame loop.
* **Pragmatic Platform Handling**: `SDL3` is leveraged as our unified cross-platform abstraction window handle (`SDL_Window*`), bridging the RHI to platform surfaces uniformly.

---

## 2. Directory & Blueprint Mapping

```text
NoxEngine/
├── Presentation/
│   ├── Renderer.h / .cpp       # High-level engine coordinator; orchestrates the frame loop
│
└── NRI/                         # Pure Abstract RHI Layer (Zero Graphics API Leaks)
    ├── Device.h                # Virtual factory for Swapchains, Command Allocators, etc.
    ├── Swapchain.h             # Abstract representation of backbuffers and resizing
    ├── CommandAllocator.h      # Abstract interface for Command Pools
    ├── CommandBuffer.h         # Abstract pipeline recording state interface
    │
    └── vulkan/                 # Hidden Concrete Driver Implementation
        ├── DeviceVK.h / .cpp   # Controls Vulkan Instance, Surfaces, Queues, Device limits
        └── SwapchainVK.h / .cpp# Inherits from Swapchain; manages vk::raii objects and resizes