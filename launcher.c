#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <fcntl.h>

int main(int argc, char *argv[]) {
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

    // Fork to detach from parent process
    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed
        return 1;
    }

    if (pid > 0) {
        // Parent process - exit immediately
        exit(0);
    }

    // Child process continues
    // Create new session to detach from terminal
    setsid();

    // Redirect stdin, stdout, stderr to /dev/null
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) {
            close(devnull);
        }
    }

    // Execute the real application
    execv(exe_path, argv);

    // If execv returns, there was an error
    return 1;
}
