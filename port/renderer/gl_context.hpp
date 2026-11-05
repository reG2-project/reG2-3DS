// The window's OpenGL ES 3.0 context, as the video layer uses it.
#pragma once

class IGLContext
{
  public:
    virtual ~IGLContext() {}
    virtual void* GetProcAddress(const char* name) = 0;
    virtual void GetDrawableSize(int* pW, int* pH) = 0;
    virtual void SwapBuffers() = 0;
    virtual void MakeCurrent() = 0;
};

struct SDL_Window;

// The flags the game window needs for this context (SDL_WindowFlags).
unsigned long long GLWindowFlags();

// Creates the context on a window made with GLWindowFlags().
IGLContext* CreateGLContext(SDL_Window* pWindow, const char** ppError);
