// libctru's linear heap has no lock: its calls go through one here, as the sound pass on the system core
// (sound_job.cpp) may allocate DSP buffers while the main thread draws. Linked with --wrap=linearAlloc and the others.
#include <3ds.h>

namespace
{
RecursiveLock sLock;

struct Hold
{
    Hold()
    {
        RecursiveLock_Lock(&sLock);
    }
    ~Hold()
    {
        RecursiveLock_Unlock(&sLock);
    }
};
} // namespace

extern "C"
{
void* __real_linearAlloc(size_t size);
void* __real_linearMemAlign(size_t size, size_t alignment);
void* __real_linearRealloc(void* mem, size_t size);
size_t __real_linearGetSize(void* mem);
void __real_linearFree(void* mem);
u32 __real_linearSpaceFree(void);

void* __wrap_linearAlloc(size_t size)
{
    Hold hold;
    return __real_linearAlloc(size);
}

void* __wrap_linearMemAlign(size_t size, size_t alignment)
{
    Hold hold;
    return __real_linearMemAlign(size, alignment);
}

void* __wrap_linearRealloc(void* mem, size_t size)
{
    Hold hold;
    return __real_linearRealloc(mem, size);
}

size_t __wrap_linearGetSize(void* mem)
{
    Hold hold;
    return __real_linearGetSize(mem);
}

void __wrap_linearFree(void* mem)
{
    Hold hold;
    __real_linearFree(mem);
}

u32 __wrap_linearSpaceFree(void)
{
    Hold hold;
    return __real_linearSpaceFree();
}
}
