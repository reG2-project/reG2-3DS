#include "zone_index.hpp"
#include "map_0x370.hpp"
#include <vector>

namespace PortZoneIndex
{
namespace
{
const int kShift = 3;
const int kCells = 256 >> kShift;

const gmp_zone_list* spBuilt;
u16 sNumZones;
std::vector<u32> sFirst;
std::vector<u16> sList;
} // namespace

void Build(const gmp_zone_list* pZones)
{
    spBuilt = pZones;
    sNumZones = pZones->mNumZones;
    std::vector<std::vector<u16>> cells(kCells * kCells);
    for (u16 i = 0; i < sNumZones; i++)
    {
        const gmp_map_zone* pZone = pZones->mZoneArray[i];
        const int right = pZone->mX + pZone->mW - 1 < 255 ? pZone->mX + pZone->mW - 1 : 255;
        const int bottom = pZone->mY + pZone->mH - 1 < 255 ? pZone->mY + pZone->mH - 1 : 255;
        for (int cy = pZone->mY >> kShift; cy <= bottom >> kShift; cy++)
        {
            for (int cx = pZone->mX >> kShift; cx <= right >> kShift; cx++)
            {
                cells[cy * kCells + cx].push_back(i);
            }
        }
    }
    sFirst.assign(kCells * kCells, 0);
    sList.clear();
    for (int c = 0; c < kCells * kCells; c++)
    {
        sFirst[c] = sList.size();
        sList.insert(sList.end(), cells[c].begin(), cells[c].end());
        sList.push_back(kEnd);
    }
}

const u16* At(const gmp_zone_list* pZones, u8 x, u8 y)
{
    if (pZones != spBuilt || pZones->mNumZones != sNumZones)
    {
        return NULL;
    }
    return &sList[sFirst[(y >> kShift) * kCells + (x >> kShift)]];
}
} // namespace PortZoneIndex
