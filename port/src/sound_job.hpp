// GTA2_3DS_SOUND_CORE1=1: a step's sound effects pass (sound_obj::ServiceSoundEffects) runs on the system core
// while the frame draws, joined before the next step (port/3ds/sound_job.cpp). Elsewhere it stays in the step.
#pragma once

namespace PortSoundJob
{
#if defined(__3DS__)
// Hands this step's pass to the system core; false when it must run here (option off, paused, menu, interpolation).
bool Defer();
// Waits for the pass handed on: before the next step, a message, an exit.
void Join();
// Only the steps of the port's frame loop hand their pass on, as it joins it before anything else.
void AllowInStep(bool allow);
// Whether steps hand their pass on; a change joins the pass in flight first (GTA2_3DS_PROF_AB, between blocks).
void SetEnabled(bool on);
bool Enabled();
#else
inline bool Defer()
{
    return false;
}
inline void Join()
{
}
inline void AllowInStep(bool)
{
}
inline void SetEnabled(bool)
{
}
inline bool Enabled()
{
    return false;
}
#endif
} // namespace PortSoundJob
