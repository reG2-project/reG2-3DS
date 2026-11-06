#include "platform.hpp"

// The 3DS answers in port/3ds/platform_3ds.cpp.
#if !defined(__3DS__)
namespace PortPlatform
{
bool Has(Feature)
{
    return true;
}
} // namespace PortPlatform
#endif
