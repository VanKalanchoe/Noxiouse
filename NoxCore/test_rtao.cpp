#include <iostream>
#include <cstdint>

struct mat4 { float m[16]; };
struct vec2 { float x, y; };

struct PushConstantRTAO {
    mat4 invViewProj;
    uint64_t matrixReference;
    uint64_t kernelReference;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t kernelSize;
    float radius;
    float power;
    vec2 viewportSize;
};

int main() {
    std::cout << "invViewProj: " << offsetof(PushConstantRTAO, invViewProj) << "\n";
    std::cout << "matrixReference: " << offsetof(PushConstantRTAO, matrixReference) << "\n";
    std::cout << "kernelReference: " << offsetof(PushConstantRTAO, kernelReference) << "\n";
    std::cout << "depthTextureIndex: " << offsetof(PushConstantRTAO, depthTextureIndex) << "\n";
    std::cout << "normalTextureIndex: " << offsetof(PushConstantRTAO, normalTextureIndex) << "\n";
    std::cout << "kernelSize: " << offsetof(PushConstantRTAO, kernelSize) << "\n";
    std::cout << "radius: " << offsetof(PushConstantRTAO, radius) << "\n";
    std::cout << "power: " << offsetof(PushConstantRTAO, power) << "\n";
    std::cout << "viewportSize: " << offsetof(PushConstantRTAO, viewportSize) << "\n";
    std::cout << "Size: " << sizeof(PushConstantRTAO) << "\n";
}
