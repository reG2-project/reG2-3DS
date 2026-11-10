// GTA2_3DS_MAPCACHE=1: the city's flat lids and walls built once per 16x16 block chunk into meshes the 3DS's vertex
// shader projects (port/3ds/map_cache.cpp); MapRenderer::Draw still walks the blocks and draws everything else.
#pragma once

#include "types.hpp"

namespace PortMapCache
{
#if defined(__3DS__)
// GTA2_3DS_MAPCACHE until SetEnabled, which applies from the next layer on; while off the built chunks are kept.
bool Enabled();
void SetEnabled(bool on);
// After the layer's lights and projection are set, before its blocks are walked (minX..maxY: the walk's range).
void BeginLayer(s32 z, s32 minX, s32 minY, s32 maxX, s32 maxY);
// RenderFlatBlock's lid at gXCoord, gYCoord, gZCoord: true when the cache draws it at the end of the layer.
bool TakeLid(u16 lid);
// RenderBlockAt(x, y) on the layer would draw nothing but a lid and walls the cache draws: the walk can leave the cell
// out.
bool SkipCell(s32 x, s32 y);
// After the layer's blocks: its cached walls, then its lids, under the next layer's sprites.
void EndLayer(s32 z);
// The block column at (x, y) changed.
void Invalidate(s32 x, s32 y);
#else
inline bool Enabled()
{
    return false;
}
inline void SetEnabled(bool)
{
}
inline void BeginLayer(s32, s32, s32, s32, s32)
{
}
inline bool TakeLid(u16)
{
    return false;
}
inline bool SkipCell(s32, s32)
{
    return false;
}
inline void EndLayer(s32)
{
}
inline void Invalidate(s32, s32)
{
}
#endif
} // namespace PortMapCache
