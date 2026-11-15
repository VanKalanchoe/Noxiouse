#include "TypeMeta.h"

namespace Nox
{
    TypeRegistry& TypeRegistry::Get()
    {
        static TypeRegistry s_Instance;
        return s_Instance;
    }
}
