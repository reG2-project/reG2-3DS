#include "prefetch.hpp"
#include "Car.hpp"
#include "CarPhysics.hpp"
#include "Object_5C.hpp"
#include "Ped.hpp"
#include "char.hpp"
#include "sprite.hpp"

namespace PortPrefetch
{
namespace
{
// PLD does not fault: a stale pointer of a free pool slot costs a useless line fill at most.
void Lines(const void* p, unsigned bytes)
{
    const char* c = static_cast<const char*>(p);
    for (unsigned i = 0; i < bytes; i += 32)
    {
        __builtin_prefetch(c + i);
    }
}

void Sprite(const ::Sprite* pSprite)
{
    if (pSprite)
    {
        Lines(pSprite, sizeof(::Sprite));
    }
}
} // namespace

// At item i the walk asks for item i + 3, what item i + 2 points to and the shapes or sprites of item i + 1:
// each read here is of memory asked for one item before.
void Ahead(const Car* pNext)
{
    const Car* p2 = pNext->mpNext;
    if (p2)
    {
        if (p2->mpNext)
        {
            Lines(p2->mpNext, sizeof(Car));
        }
        Sprite(p2->mCarSprite);
        if (p2->mPhysics)
        {
            Lines(p2->mPhysics, sizeof(CarPhysics));
        }
        if (p2->mAI)
        {
            Lines(p2->mAI, sizeof(CarAI));
        }
    }
    if (pNext->mCarSprite && pNext->mCarSprite->mpCollisionShape)
    {
        Lines(pNext->mCarSprite->mpCollisionShape, sizeof(SpriteShape));
    }
}

void Ahead(const Ped* pNext)
{
    const Ped* p2 = pNext->mpNext;
    if (p2)
    {
        if (p2->mpNext)
        {
            Lines(p2->mpNext, sizeof(Ped));
        }
        if (p2->mGameObject)
        {
            Lines(p2->mGameObject, sizeof(Char));
        }
    }
    if (pNext->mGameObject)
    {
        Sprite(pNext->mGameObject->mSpritePtr);
    }
}

void Ahead(const Object* pNext)
{
    const Object* p2 = pNext->mpNext;
    if (p2)
    {
        if (p2->mpNext)
        {
            Lines(p2->mpNext, sizeof(Object));
        }
        Sprite(p2->mSprite);
    }
    if (pNext->mSprite && pNext->mSprite->mpCollisionShape)
    {
        Lines(pNext->mSprite->mpCollisionShape, sizeof(SpriteShape));
    }
}
} // namespace PortPrefetch
