// Port-only shortcuts in the game logic that leave its results the same, each on unless its variable is 0
// (names in logic_opts.cpp), so that one build compares them on and off.
#pragma once

namespace PortLogic
{
enum Option
{
    kPrefetch,
    kGridKeep,
    kOptionCount
};

extern signed char gOn[kOptionCount];
bool Read(Option option);

inline bool On(Option option)
{
    return gOn[option] > 0 || (gOn[option] < 0 && Read(option));
}
} // namespace PortLogic
