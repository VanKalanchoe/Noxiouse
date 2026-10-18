#pragma once

#include <Coral/Attribute.hpp>

// Include this header instead of Coral/Attribute.hpp when reading attributes.
// Coral d53b268 defines the string and bool specializations in Attribute.cpp
// but omits their declarations from its header. Declare them before any calls
// so consumers use Coral's implementations instead of instantiating the generic
// template (duplicate symbols and an incorrect managed value layout).
// This covers all attribute fields; no per-field declarations are needed.
// Remove these declarations once the pinned Coral header provides them.
namespace Coral
{
    template<>
    std::string Attribute::GetFieldValue<std::string>(std::string_view fieldName);

    template<>
    bool Attribute::GetFieldValue<bool>(std::string_view fieldName);
}
