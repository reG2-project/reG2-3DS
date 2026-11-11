#include "platform.hpp"

// The 3DS answers in port/3ds/platform_3ds.cpp.
#if !defined(__3DS__)
namespace PortPlatform
{
bool Has(Feature feature)
{
    return feature != kConsoleControls && feature != kFpsOverlay;
}

const wchar_t* PadButtonName(int code)
{
    static const wchar_t* const kNames[12] = {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"BACK", L"START", L"L3", L"R3", L"LT", L"RT"};
    return code >= 0 && code < 12 ? kNames[code] : L"-";
}

int MenuSelectButton()
{
    return 0;
}

int MenuBackButton()
{
    return 1;
}
} // namespace PortPlatform
#endif
