// Pool walks ask for the next items' memory while they update the current one (GTA2_3DS_PREFETCH=1: on).
#pragma once
#include "logic_opts.hpp"

class Car;
class Ped;
class Object;

namespace PortPrefetch
{
// Asks for the items after pNext and what they point to; the template is for the pools left alone.
void Ahead(const Car* pNext);
void Ahead(const Ped* pNext);
void Ahead(const Object* pNext);
template <typename T> inline void Ahead(const T*)
{
}

template <typename T> inline void Next(const T* pNext)
{
    if (pNext && PortLogic::On(PortLogic::kPrefetch))
    {
        Ahead(pNext);
    }
}
} // namespace PortPrefetch
