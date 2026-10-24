// GOW-Port: iOS EAGL contexts for the GS renderer (gs_gl.cpp, PS2X_IOS). Compiled with ARC.
#import <OpenGLES/EAGL.h>

// share == nullptr: a new sharegroup; else a context in share's sharegroup (the renderer's main context in the host
// window's, a worker in the renderer's)
extern "C" void *ps2xEaglCreate(void *share)
{
    EAGLContext *parent = (__bridge EAGLContext *)share;
    EAGLContext *context = parent ? [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3 sharegroup:parent.sharegroup]
                                  : [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    return context ? (__bridge_retained void *)context : nullptr;
}

// the calling thread's current context (the host window's, SDL's), not retained: SDL keeps it for the window's life
extern "C" void *ps2xEaglCurrent()
{
    return (__bridge void *)[EAGLContext currentContext];
}

extern "C" bool ps2xEaglMakeCurrent(void *context)
{
    return [EAGLContext setCurrentContext:(__bridge EAGLContext *)context];
}

extern "C" void ps2xEaglDestroy(void *context)
{
    if (!context)
        return;
    EAGLContext *owned = (__bridge_transfer EAGLContext *)context;
    if ([EAGLContext currentContext] == owned)
        [EAGLContext setCurrentContext:nil];
    owned = nil;
}
