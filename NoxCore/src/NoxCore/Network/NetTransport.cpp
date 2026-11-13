#include "NetTransport.h"

#include "GnsTransport.h"
#include "LoopbackTransport.h"

namespace Nox::Net
{
    bool NetEmulation::FromPreset(std::string_view name, NetEmulation& emulation)
    {
        if (name == "off" || name == "none")
            emulation = Off();
        else if (name == "average")
            emulation = Average();
        else if (name == "bad")
            emulation = Bad();
        else
            return false;
        return true;
    }

    bool IsLoopbackAddress(std::string_view address)
    {
        constexpr std::string_view prefix = "loopback";
        return address.substr(0, prefix.size()) == prefix && (address.size() == prefix.size() || address[prefix.size()] == ':');
    }

    std::unique_ptr<NetTransport> CreateTransport(bool loopback, const TransportConfig& config)
    {
        if (loopback)
            return std::make_unique<LoopbackTransport>(config);
        return std::make_unique<GnsTransport>(config);
    }
}
