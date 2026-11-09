// What the machine the port runs on offers: the settings menu leaves out what it lacks, and the controls
// keep their defaults where they cannot be bound. port/src/platform.cpp answers for PC, port/3ds for the 3DS.
#pragma once

namespace PortPlatform
{
enum Feature
{
    kWindow, // a window or full screen, display modes and vsync
    kFastFrames, // frame limits above the original's 30
    kGamma, // BRIGHTNESS
    kSafeArea, // a margin for TVs that crop the picture
    kRendererOptions, // the OpenGL renderer's GRAPHICS page and the headlights; the other lights need its pixel lighting
    k3dSound, // positional sound (OpenAL Soft) to turn on or off
    kRebinding, // keys, mouse and pad inputs bound in the menu
    kTurningView, // the CHINATOWN camera, turned and tilted with the player's vehicle
    kHudScale, // a HUD drawn over the world, at a scale to choose
    kKeyboard,
    kTwinStick, // a second stick to aim with
};

bool Has(Feature feature);

// A pad button's name as printed on the pad, by pad binding code 0 to 11 (compat/src/pad.cpp: SDL's south,
// east, west and north buttons, the shoulders, back, start, the stick presses, the triggers).
const wchar_t* PadButtonName(int code);
} // namespace PortPlatform
