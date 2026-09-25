// Dock menu for File Tracker Unified on macOS.
//
// GTK4 installs its own NSApplication delegate (GtkApplicationQuartzDelegate),
// which does not implement applicationDockMenu:. This adds that method to the
// delegate's class at runtime so right-clicking the Dock icon offers a
// "New Window" item, like Terminal's "New Terminal".

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

static NSMenu *dock_menu;
static void (*dock_new_window_callback)(void);

@interface FTDockMenuTarget : NSObject
- (void)newWindow:(id)sender;
@end

@implementation FTDockMenuTarget
- (void)newWindow:(id)sender {
    (void)sender;
    if (dock_new_window_callback) dock_new_window_callback();
}
@end

static NSMenu *ft_application_dock_menu(id self, SEL cmd, NSApplication *sender) {
    (void)self; (void)cmd; (void)sender;
    return dock_menu;
}

// Call after GtkApplication startup, once GTK has set the NSApp delegate
void macos_install_dock_menu(const char *title, void (*callback)(void)) {
    id delegate = [NSApp delegate];
    if (!delegate || dock_menu) return;

    dock_new_window_callback = callback;

    // Target and menu live for the life of the process
    FTDockMenuTarget *target = [[FTDockMenuTarget alloc] init];
    dock_menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:title]
                                                  action:@selector(newWindow:)
                                           keyEquivalent:@""];
    [item setTarget:target];
    [dock_menu addItem:item];
    [item release];

    class_addMethod([delegate class], @selector(applicationDockMenu:),
                    (IMP)ft_application_dock_menu, "@@:@");

    // Re-set the delegate so AppKit re-checks which methods it implements
    [NSApp setDelegate:nil];
    [NSApp setDelegate:delegate];
}
