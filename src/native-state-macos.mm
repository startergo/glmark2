#include "native-state-macos.h"
#include "log.h"
#include "options.h"

#import <Cocoa/Cocoa.h>

// Compatibility with the 10.6 SDK:
//  - NSEventMask*/NSEventType* and NSWindowStyleMask* are the 10.12 renames
//    of NSAnyEventMask/NSKeyDown and the old NS*WindowMask constants.
//  - The 10.6 ObjC runtime has no objc_autoreleasePoolPush/Pop (10.7+), so
//    @autoreleasepool would produce unloadable symbols; NSAutoreleasePool is
//    used instead throughout this file.
#ifndef __MAC_OS_X_VERSION_MAX_ALLOWED
#define __MAC_OS_X_VERSION_MAX_ALLOWED 1060
#endif

#if __MAC_OS_X_VERSION_MAX_ALLOWED < 101200
typedef NSUInteger NSWindowStyleMask;
#define NSWindowStyleMaskBorderless NSBorderlessWindowMask
#define NSWindowStyleMaskTitled NSTitledWindowMask
#define NSWindowStyleMaskClosable NSClosableWindowMask
#define NSWindowStyleMaskMiniaturizable NSMiniaturizableWindowMask
#define NSEventMaskAny NSAnyEventMask
#define NSEventTypeKeyDown NSKeyDown
#endif

@interface GLMarkWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) BOOL shouldQuit;
@end

@implementation GLMarkWindowDelegate
- (instancetype)init
{
    self = [super init];
    if (self) {
        _shouldQuit = NO;
    }
    return self;
}

- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    self.shouldQuit = YES;
}
@end

static void pump_events(bool* should_quit)
{
    /* The event pump runs outside any pool otherwise; each polled event
     * and the objects its handling autoreleases would leak with an
     * __NSAutoreleaseNoPool complaint on every frame. */
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    while (true) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (!event)
            break;

        if (event.type == NSEventTypeKeyDown) {
            // Escape key
            if (event.keyCode == 53) {
                *should_quit = true;
            }
        }

        [NSApp sendEvent:event];
    }
    [pool drain];
}

struct NativeStateMacOS::Impl
{
    NSWindow* window = nil;
    NSView* view = nil;
    GLMarkWindowDelegate* delegate = nil;
    WindowProperties props;
    bool initialized = false;

    bool should_quit()
    {
        bool quit = false;
        pump_events(&quit);
        if (delegate && delegate.shouldQuit)
            quit = true;
        return quit;
    }
};

NativeStateMacOS::NativeStateMacOS()
    : impl_(new Impl)
{
    Options::winsys_options_help = "";
}

NativeStateMacOS::~NativeStateMacOS()
{
    if (impl_) {
        NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
        if (impl_->window) {
            [impl_->window orderOut:nil];
            impl_->window = nil;
        }
        impl_->view = nil;
        impl_->delegate = nil;
        [pool drain];
        delete impl_;
        impl_ = nullptr;
    }
}

bool NativeStateMacOS::init_display()
{
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    impl_->initialized = true;
    [pool drain];
    return true;
}

void* NativeStateMacOS::display()
{
    return nullptr;
}

bool NativeStateMacOS::create_window(WindowProperties const& properties)
{
    if (!impl_->initialized) {
        Log::error("Error: macOS display has not been initialized!\n");
        return false;
    }

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    const bool request_fullscreen = properties.fullscreen;

    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) {
        Log::error("Error: Could not get main screen\n");
        [pool drain];
        return false;
    }

    NSRect frame;
    NSWindowStyleMask style;

    if (request_fullscreen) {
        frame = [screen frame];
        style = NSWindowStyleMaskBorderless;
    } else {
        const CGFloat w = properties.width > 0 ? properties.width : 800;
        const CGFloat h = properties.height > 0 ? properties.height : 600;
        frame = NSMakeRect(100, 100, w, h);
        style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    }

    // Recreate only if size/fullscreen changed
    if (impl_->window) {
        if (impl_->props.fullscreen != request_fullscreen ||
            (!request_fullscreen &&
             (impl_->props.width != (int)frame.size.width || impl_->props.height != (int)frame.size.height)))
        {
            [impl_->window orderOut:nil];
            impl_->window = nil;
            impl_->view = nil;
            impl_->delegate = nil;
        } else {
            [pool drain];
            return true;
        }
    }

    impl_->delegate = [[GLMarkWindowDelegate alloc] init];

    impl_->window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    if (!impl_->window) {
        Log::error("Error: Could not create NSWindow\n");
        [pool drain];
        return false;
    }

    [impl_->window setTitle:[NSString stringWithFormat:@"glmark2 %s", GLMARK_VERSION]];
    [impl_->window setDelegate:impl_->delegate];

    // Content view used as the GL drawable
    impl_->view = [[NSView alloc] initWithFrame:[[impl_->window contentView] bounds]];
    [impl_->view setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

    // On Retina displays, NSOpenGL can create a drawable that is larger
    // (in pixels) than the view size (in points). If we keep the default
    // high-DPI surface, but glmark2 sets glViewport to the requested size,
    // rendering only fills the bottom-left portion of the window.
    //
    // Disable the high-resolution OpenGL surface so viewport dimensions
    // match the requested window size.
    if ([impl_->view respondsToSelector:@selector(setWantsBestResolutionOpenGLSurface:)]) {
        [impl_->view setWantsBestResolutionOpenGLSurface:NO];
    }

    [impl_->window setContentView:impl_->view];

    if (request_fullscreen) {
        [impl_->window setLevel:NSMainMenuWindowLevel + 1];
    }

    impl_->props = properties;
    impl_->props.width = (int)frame.size.width;
    impl_->props.height = (int)frame.size.height;
    impl_->props.fullscreen = request_fullscreen;

    [pool drain];
    return true;
}

void* NativeStateMacOS::window(WindowProperties& properties)
{
    properties = impl_->props;
    return (void*)impl_->view;
}

void NativeStateMacOS::visible(bool v)
{
    if (!v || !impl_->window)
        return;

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    [impl_->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [pool drain];
}

bool NativeStateMacOS::should_quit()
{
    return impl_->should_quit();
}

void NativeStateMacOS::flip()
{
    // Event handling is performed in should_quit(); avoid doing it twice per frame.
}
