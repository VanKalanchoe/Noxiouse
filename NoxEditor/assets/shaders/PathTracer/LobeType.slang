#pragma once

typedef uint LobeType;

namespace LobeType
{
    static const uint None                    = 0x00u;

    static const uint DiffuseReflection       = 0x01u;
    static const uint SpecularReflection      = 0x02u;
    static const uint DeltaReflection         = 0x04u;

    static const uint DiffuseTransmission     = 0x10u;
    static const uint SpecularTransmission    = 0x20u;
    static const uint DeltaTransmission       = 0x40u;

    static const uint Diffuse                 = 0x11u;
    static const uint Specular                = 0x22u;
    static const uint Delta                   = 0x44u;
    static const uint NonDelta                = 0x33u;

    static const uint Reflection              = 0x0fu;
    static const uint Transmission            = 0xf0u;

    static const uint NonDeltaReflection      = 0x03u;
    static const uint NonDeltaTransmission    = 0x30u;

    static const uint All                     = 0xffu;
}
