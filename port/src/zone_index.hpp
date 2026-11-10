// The map zones whose rect meets each 8x8 block cell, in the zone list's order, for the lookups by position.
#pragma once
#include "types.hpp"

struct gmp_zone_list;

namespace PortZoneIndex
{
const u16 kEnd = 0xFFFF;

void Build(const gmp_zone_list* pZones);
// The zone indices of the cell holding (x, y), ending with kEnd; NULL if the index is not of pZones.
const u16* At(const gmp_zone_list* pZones, u8 x, u8 y);
} // namespace PortZoneIndex
