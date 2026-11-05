// The 3D provider (miles3d.cpp) as the 2D mixer (miles.cpp) drives it.
#pragma once

#ifdef __3DS__
// The DSP's output rate (32728 Hz, rounded down): the DSP then plays the mix without resampling it.
const int Miles3D_kRate = 32728;
#else
const int Miles3D_kRate = 44100;
#endif

// Adds the 3D samples to an interleaved stereo float mix; false when no provider is open.
bool Miles3D_Render(float* pMix, int frames);

void Miles3D_SetMasterVolume(S32 volume);
