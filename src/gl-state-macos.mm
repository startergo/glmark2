#include "gl-state-macos.h"
#include "log.h"
#include "options.h"

#include <dlfcn.h>

#import <Cocoa/Cocoa.h>

// Compatibility with the 10.6 SDK:
//  - NSOpenGLPFAOpenGLProfile / NSOpenGLProfileVersion* are 10.7+; on 10.6
//    only the legacy profile exists, so no profile attribute is requested.
//  - The 10.6 ObjC runtime has no objc_autoreleasePoolPush/Pop (10.7+), so
//    @autoreleasepool would produce unloadable symbols; NSAutoreleasePool is
//    used instead throughout this file.
#ifndef __MAC_OS_X_VERSION_MAX_ALLOWED
#define __MAC_OS_X_VERSION_MAX_ALLOWED 1060
#endif

struct GLStateMacOS::Impl
{
    NSOpenGLPixelFormat* pixel_format = nil;
    NSOpenGLContext* context = nil;
    bool core_profile_requested = false;
    GLuint vao = 0;
};

GLStateMacOS::GLStateMacOS()
    : view_(nullptr), requested_visual_config_(), active_visual_config_(), impl_(new Impl)
{
}

GLStateMacOS::~GLStateMacOS()
{
    reset();
    delete impl_;
    impl_ = nullptr;
}

bool GLStateMacOS::init_display(void* native_display, GLVisualConfig& config_pref)
{
    (void)native_display;
    requested_visual_config_ = config_pref;

    // OpenGL symbols are exported from OpenGL.framework
    if (!lib_.open_from_alternatives({
            "/System/Library/Frameworks/OpenGL.framework/OpenGL",
            "OpenGL.framework/OpenGL",
            "libGL.dylib",
        }))
    {
        Log::warning("Could not open OpenGL.framework for symbol loading; falling back to RTLD_DEFAULT\n");
    }

    return true;
}

bool GLStateMacOS::init_surface(void* native_window)
{
    view_ = native_window;
    return view_ != nullptr;
}

bool GLStateMacOS::init_gl_extensions()
{
    auto load = [this](const char* name) {
        return reinterpret_cast<void*>(load_proc(this, name));
    };

    // glad in this tree is primarily set up around extension entry points.
    // On macOS core profile the non-EXT symbols may be available even when
    // the EXT aliases aren't, so resolve both. Legacy drivers (e.g. 10.6
    // GL 2.1) may only implement buffer mapping via the ARB/EXT entry points.
    GLExtensions::MapBuffer = reinterpret_cast<decltype(GLExtensions::MapBuffer)>(load("glMapBuffer"));
    if (!GLExtensions::MapBuffer)
        GLExtensions::MapBuffer = reinterpret_cast<decltype(GLExtensions::MapBuffer)>(load("glMapBufferARB"));
    if (!GLExtensions::MapBuffer)
        GLExtensions::MapBuffer = reinterpret_cast<decltype(GLExtensions::MapBuffer)>(load("glMapBufferEXT"));

    GLExtensions::UnmapBuffer = reinterpret_cast<decltype(GLExtensions::UnmapBuffer)>(load("glUnmapBuffer"));
    if (!GLExtensions::UnmapBuffer)
        GLExtensions::UnmapBuffer = reinterpret_cast<decltype(GLExtensions::UnmapBuffer)>(load("glUnmapBufferARB"));
    if (!GLExtensions::UnmapBuffer)
        GLExtensions::UnmapBuffer = reinterpret_cast<decltype(GLExtensions::UnmapBuffer)>(load("glUnmapBufferEXT"));

    GLExtensions::GenFramebuffers = reinterpret_cast<decltype(GLExtensions::GenFramebuffers)>(load("glGenFramebuffers"));
    if (!GLExtensions::GenFramebuffers)
        GLExtensions::GenFramebuffers = reinterpret_cast<decltype(GLExtensions::GenFramebuffers)>(load("glGenFramebuffersEXT"));

    GLExtensions::DeleteFramebuffers = reinterpret_cast<decltype(GLExtensions::DeleteFramebuffers)>(load("glDeleteFramebuffers"));
    if (!GLExtensions::DeleteFramebuffers)
        GLExtensions::DeleteFramebuffers = reinterpret_cast<decltype(GLExtensions::DeleteFramebuffers)>(load("glDeleteFramebuffersEXT"));

    GLExtensions::BindFramebuffer = reinterpret_cast<decltype(GLExtensions::BindFramebuffer)>(load("glBindFramebuffer"));
    if (!GLExtensions::BindFramebuffer)
        GLExtensions::BindFramebuffer = reinterpret_cast<decltype(GLExtensions::BindFramebuffer)>(load("glBindFramebufferEXT"));

    GLExtensions::FramebufferTexture2D = reinterpret_cast<decltype(GLExtensions::FramebufferTexture2D)>(load("glFramebufferTexture2D"));
    if (!GLExtensions::FramebufferTexture2D)
        GLExtensions::FramebufferTexture2D = reinterpret_cast<decltype(GLExtensions::FramebufferTexture2D)>(load("glFramebufferTexture2DEXT"));

    GLExtensions::FramebufferRenderbuffer = reinterpret_cast<decltype(GLExtensions::FramebufferRenderbuffer)>(load("glFramebufferRenderbuffer"));
    if (!GLExtensions::FramebufferRenderbuffer)
        GLExtensions::FramebufferRenderbuffer = reinterpret_cast<decltype(GLExtensions::FramebufferRenderbuffer)>(load("glFramebufferRenderbufferEXT"));

    GLExtensions::CheckFramebufferStatus = reinterpret_cast<decltype(GLExtensions::CheckFramebufferStatus)>(load("glCheckFramebufferStatus"));
    if (!GLExtensions::CheckFramebufferStatus)
        GLExtensions::CheckFramebufferStatus = reinterpret_cast<decltype(GLExtensions::CheckFramebufferStatus)>(load("glCheckFramebufferStatusEXT"));

    GLExtensions::GenRenderbuffers = reinterpret_cast<decltype(GLExtensions::GenRenderbuffers)>(load("glGenRenderbuffers"));
    if (!GLExtensions::GenRenderbuffers)
        GLExtensions::GenRenderbuffers = reinterpret_cast<decltype(GLExtensions::GenRenderbuffers)>(load("glGenRenderbuffersEXT"));

    GLExtensions::DeleteRenderbuffers = reinterpret_cast<decltype(GLExtensions::DeleteRenderbuffers)>(load("glDeleteRenderbuffers"));
    if (!GLExtensions::DeleteRenderbuffers)
        GLExtensions::DeleteRenderbuffers = reinterpret_cast<decltype(GLExtensions::DeleteRenderbuffers)>(load("glDeleteRenderbuffersEXT"));

    GLExtensions::BindRenderbuffer = reinterpret_cast<decltype(GLExtensions::BindRenderbuffer)>(load("glBindRenderbuffer"));
    if (!GLExtensions::BindRenderbuffer)
        GLExtensions::BindRenderbuffer = reinterpret_cast<decltype(GLExtensions::BindRenderbuffer)>(load("glBindRenderbufferEXT"));

    GLExtensions::RenderbufferStorage = reinterpret_cast<decltype(GLExtensions::RenderbufferStorage)>(load("glRenderbufferStorage"));
    if (!GLExtensions::RenderbufferStorage)
        GLExtensions::RenderbufferStorage = reinterpret_cast<decltype(GLExtensions::RenderbufferStorage)>(load("glRenderbufferStorageEXT"));

    GLExtensions::GenerateMipmap = reinterpret_cast<decltype(GLExtensions::GenerateMipmap)>(load("glGenerateMipmap"));
    if (!GLExtensions::GenerateMipmap)
        GLExtensions::GenerateMipmap = reinterpret_cast<decltype(GLExtensions::GenerateMipmap)>(load("glGenerateMipmapEXT"));

    return true;
}

bool GLStateMacOS::valid()
{
    if (!ensure_context())
        return false;

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    [impl_->context makeCurrentContext];

    if (gladLoadGLUserPtr(load_proc, this) == 0) {
        Log::error("Failed to load GL entry points\n");
        [pool drain];
        return false;
    }

    // Core profile requires a VAO bound for vertex attribute arrays.
    using PFNGLGENVERTEXARRAYSPROC = void (*)(GLsizei, GLuint*);
    using PFNGLBINDVERTEXARRAYPROC = void (*)(GLuint);
    auto gen_vaos = reinterpret_cast<PFNGLGENVERTEXARRAYSPROC>(load_proc(this, "glGenVertexArrays"));
    auto bind_vao = reinterpret_cast<PFNGLBINDVERTEXARRAYPROC>(load_proc(this, "glBindVertexArray"));
    if (gen_vaos && bind_vao && impl_->vao == 0) {
        gen_vaos(1, &impl_->vao);
        bind_vao(impl_->vao);
    }

    if (!init_gl_extensions()) {
        [pool drain];
        return false;
    }

    // Set swap interval according to swap mode
    GLint interval = (Options::swap_mode == Options::SwapModeFIFO) ? 1 : 0;
    [impl_->context setValues:&interval forParameter:NSOpenGLCPSwapInterval];

    update_visual_config();
    [pool drain];
    return true;
}

bool GLStateMacOS::reset()
{
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    if (impl_ && impl_->context) {
        /* Make the context current so the GL cleanup below is issued to it
         * (reset() may also run from the destructor with no context bound). */
        [impl_->context makeCurrentContext];

        if (impl_->vao != 0) {
            using PFNGLDELETEVERTEXARRAYSPROC = void (*)(GLsizei, const GLuint*);
            auto del_vaos = reinterpret_cast<PFNGLDELETEVERTEXARRAYSPROC>(load_proc(this, "glDeleteVertexArrays"));
            if (del_vaos)
                del_vaos(1, &impl_->vao);
            impl_->vao = 0;
        }

        /* Drain in-flight commands before destroying the context. The
         * benchmark loop swaps with swap interval 0, so batches touching
         * texture state may still be pending; destroying the context then
         * crashes 10.6-era GLEngine in gleFreeTextureState during
         * gliDestroyContext (observed on the Snow Leopard build). */
        using PFNGLFINISHPROC = void (*)(void);
        auto finish = reinterpret_cast<PFNGLFINISHPROC>(load_proc(this, "glFinish"));
        if (finish)
            finish();

        /* Detach the drawable before releasing the context. Destroying a
         * context that is still attached to a view is unsupported and makes
         * GLEngine free surface-associated texture state at destroy time,
         * which is the state corrupted in the gleFreeTextureState crash. */
        [impl_->context clearDrawable];
        [NSOpenGLContext clearCurrentContext];
        [impl_->context release];
        impl_->context = nil;
    }
    if (impl_ && impl_->pixel_format) {
        [impl_->pixel_format release];
        impl_->pixel_format = nil;
    }
    [pool drain];

    return true;
}

void GLStateMacOS::swap()
{
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    if (impl_->context) {
        [impl_->context flushBuffer];
    }
    [pool drain];
}

bool GLStateMacOS::gotNativeConfig(intptr_t& vid, std::vector<uint64_t>& mods)
{
    vid = 0;
    mods.clear();
    return true;
}

void GLStateMacOS::getVisualConfig(GLVisualConfig& vc)
{
    vc = active_visual_config_;
}

bool GLStateMacOS::supports_sync()
{
    return false;
}

std::unique_ptr<GLStateSync> GLStateMacOS::sync()
{
    return nullptr;
}

GLADapiproc GLStateMacOS::load_proc(void* userptr, const char* name)
{
    GLStateMacOS* self = reinterpret_cast<GLStateMacOS*>(userptr);

    if (self->lib_.handle()) {
        void* sym = self->lib_.load(name);
        if (sym) {
            return reinterpret_cast<GLADapiproc>(sym);
        }
    }

    void* sym = dlsym(RTLD_DEFAULT, name);
    return reinterpret_cast<GLADapiproc>(sym);
}

bool GLStateMacOS::ensure_context()
{
    if (impl_->context)
        return true;

    if (!view_) {
        Log::error("NSOpenGL view has not been initialized\n");
        return false;
    }

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    /* No lightweight generics in the 10.6 SDK's NSArray declarations. */
    NSMutableArray* attrs = [NSMutableArray array];

    const bool want_legacy = (Options::macos_gl_profile == Options::MacOSGLProfileLegacy);

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 1070
    [attrs addObject:@(NSOpenGLPFAOpenGLProfile)];
    if (want_legacy) {
        [attrs addObject:@(NSOpenGLProfileVersionLegacy)];
        impl_->core_profile_requested = false;
    } else {
        [attrs addObject:@(NSOpenGLProfileVersion3_2Core)];
        impl_->core_profile_requested = true;
    }
#else
    /* 10.6 has only the legacy profile; the profile attribute is 10.7+. */
    (void)want_legacy;
    impl_->core_profile_requested = false;
#endif

    [attrs addObject:@(NSOpenGLPFAAccelerated)];
    [attrs addObject:@(NSOpenGLPFADoubleBuffer)];

    // Color/depth/stencil preferences
    if (requested_visual_config_.buffer > 0) {
        [attrs addObject:@(NSOpenGLPFAColorSize)];
        [attrs addObject:@(requested_visual_config_.buffer)];
    }
    if (requested_visual_config_.alpha > 0) {
        [attrs addObject:@(NSOpenGLPFAAlphaSize)];
        [attrs addObject:@(requested_visual_config_.alpha)];
    }
    if (requested_visual_config_.depth > 0) {
        [attrs addObject:@(NSOpenGLPFADepthSize)];
        [attrs addObject:@(requested_visual_config_.depth)];
    }
    if (requested_visual_config_.stencil >= 0) {
        [attrs addObject:@(NSOpenGLPFAStencilSize)];
        [attrs addObject:@(requested_visual_config_.stencil)];
    }

    if (requested_visual_config_.samples > 0) {
        [attrs addObject:@(NSOpenGLPFAMultisample)];
        [attrs addObject:@(NSOpenGLPFASampleBuffers)];
        [attrs addObject:@(1)];
        [attrs addObject:@(NSOpenGLPFASamples)];
        [attrs addObject:@(requested_visual_config_.samples)];
    }

    NSOpenGLPixelFormatAttribute c_attrs[64];
    const NSUInteger maxAttrs = sizeof(c_attrs) / sizeof(c_attrs[0]);
    NSUInteger idx = 0;
    for (NSNumber* n in attrs) {
        // Reserve space for the terminating 0 in c_attrs.
        if (idx >= maxAttrs - 1)
            break;
        c_attrs[idx++] = (NSOpenGLPixelFormatAttribute)[n intValue];
    }
    c_attrs[idx] = 0;

    impl_->pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:c_attrs];

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 1070
    if (!impl_->pixel_format && !want_legacy) {
        // Fallback: try without explicit profile request
        Log::warning("macOS core profile request failed; falling back to legacy context\n");

        NSOpenGLPixelFormatAttribute fallback_attrs[8] = {
            NSOpenGLPFAAccelerated,
            NSOpenGLPFADoubleBuffer,
            0,
        };
        impl_->pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:fallback_attrs];
        impl_->core_profile_requested = false;
    }
#endif

    if (!impl_->pixel_format) {
        Log::error("Failed to create NSOpenGLPixelFormat\n");
        [pool drain];
        return false;
    }

    impl_->context = [[NSOpenGLContext alloc] initWithFormat:impl_->pixel_format shareContext:nil];
    if (!impl_->context) {
        Log::error("Failed to create NSOpenGLContext\n");
        [pool drain];
        return false;
    }

    NSView* view = (NSView*)view_;
    [impl_->context setView:view];
    [impl_->context update];
    [impl_->context makeCurrentContext];

    [pool drain];
    return true;
}

void GLStateMacOS::update_visual_config()
{
    active_visual_config_ = GLVisualConfig{};

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];

    if (impl_->pixel_format) {
        GLint value = 0;
        [impl_->pixel_format getValues:&value forAttribute:NSOpenGLPFAColorSize forVirtualScreen:0];
        active_visual_config_.buffer = value;

        [impl_->pixel_format getValues:&value forAttribute:NSOpenGLPFAAlphaSize forVirtualScreen:0];
        active_visual_config_.alpha = value;

        [impl_->pixel_format getValues:&value forAttribute:NSOpenGLPFADepthSize forVirtualScreen:0];
        active_visual_config_.depth = value;

        [impl_->pixel_format getValues:&value forAttribute:NSOpenGLPFAStencilSize forVirtualScreen:0];
        active_visual_config_.stencil = value;

        [impl_->pixel_format getValues:&value forAttribute:NSOpenGLPFASamples forVirtualScreen:0];
        active_visual_config_.samples = value;
    }
    [pool drain];

    // Best-effort estimates for channel sizes
    active_visual_config_.red = 8;
    active_visual_config_.green = 8;
    active_visual_config_.blue = 8;
    if (active_visual_config_.alpha <= 0)
        active_visual_config_.alpha = 0;
}
