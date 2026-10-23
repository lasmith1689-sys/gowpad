// GOW-Port: iOS EAGL contexts for the GS renderer (gs_gl.cpp, PS2X_IOS). Compiled with ARC.
#import <OpenGLES/EAGL.h>

// share == nullptr: a new sharegroup (the renderer's main context); else a context in share's sharegroup (a worker)
extern "C" void *ps2xEaglCreate(void *share)
{
    EAGLContext *parent = (__bridge EAGLContext *)share;
    EAGLContext *context = parent ? [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3 sharegroup:parent.sharegroup]
                                  : [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    return context ? (__bridge_retained void *)context : nullptr;
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
