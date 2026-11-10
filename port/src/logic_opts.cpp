#include "logic_opts.hpp"
#include <stdlib.h>

namespace PortLogic
{
signed char gOn[kOptionCount] = {-1, -1};

namespace
{
const char* const kNames[kOptionCount] = {"GTA2_3DS_PREFETCH", "GTA2_3DS_GRIDKEEP"};
}

bool Read(Option option)
{
    const char* pValue = getenv(kNames[option]);
    gOn[option] = pValue && atoi(pValue) != 0 ? 1 : 0;
    return gOn[option] > 0;
}
} // namespace PortLogic
