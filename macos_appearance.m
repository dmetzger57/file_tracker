// Light/Dark appearance for File Tracker Unified on macOS.
//
// GTK's macOS backend does not report the system appearance, so GTK always draws
// light. This reads NSApp's effective appearance and calls back whenever the user
// switches between Light and Dark (or Auto flips it).
//
// Copyright (c) 2026 Dennis Metzger
// SPDX-License-Identifier: MIT

#import <Cocoa/Cocoa.h>

static void (*appearance_changed_callback)(int dark);

int macos_dark_mode(void) {
    NSAppearanceName name = [[NSApp effectiveAppearance]
        bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    return [name isEqualToString:NSAppearanceNameDarkAqua];
}

// Call after GtkApplication startup. callback runs on the main thread.
void macos_watch_appearance(void (*callback)(int dark)) {
    if (appearance_changed_callback) return;
    appearance_changed_callback = callback;
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                    (void)note;
                    // NSApp's effective appearance updates on the next run loop pass
                    dispatch_async(dispatch_get_main_queue(), ^{
                        appearance_changed_callback(macos_dark_mode());
                    });
                }];
}
