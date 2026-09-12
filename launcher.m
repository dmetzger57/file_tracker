#import <Cocoa/Cocoa.h>
#include <unistd.h>
#include <libgen.h>
#include <mach-o/dyld.h>

int main(int argc, char *argv[]) {
    @autoreleasepool {
        char path[4096];
        uint32_t size = sizeof(path);

        // Get the path to this executable
        if (_NSGetExecutablePath(path, &size) != 0) {
            return 1;
        }

        // Make a copy for dirname (it modifies the string)
        char path_copy[4096];
        strncpy(path_copy, path, sizeof(path_copy));

        // Get the directory containing this executable
        char *dir = dirname(path_copy);

        // Build path to the actual executable in Resources
        char exe_path[4096];
        snprintf(exe_path, sizeof(exe_path), "%s/../Resources/APP_EXECUTABLE", dir);

        // Convert to NSString
        NSString *executablePath = [NSString stringWithUTF8String:exe_path];

        // Build arguments array
        NSMutableArray *args = [NSMutableArray array];
        for (int i = 1; i < argc; i++) {
            [args addObject:[NSString stringWithUTF8String:argv[i]]];
        }

        // Launch the task without showing terminal
        NSTask *task = [[NSTask alloc] init];
        [task setLaunchPath:executablePath];
        [task setArguments:args];

        // Redirect all output to /dev/null
        NSFileHandle *devNull = [NSFileHandle fileHandleWithNullDevice];
        [task setStandardInput:devNull];
        [task setStandardOutput:devNull];
        [task setStandardError:devNull];

        // Launch and exit immediately
        [task launch];

        return 0;
    }
}
