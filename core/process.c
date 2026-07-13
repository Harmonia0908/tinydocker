#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

int td_run_command(char *const arguments[])
{
    pid_t child;
    int status = 0;

    if (arguments == NULL || arguments[0] == NULL || arguments[0][0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execvp(arguments[0], arguments);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (!WIFEXITED(status)) {
        errno = EINTR;
        return -1;
    }
    return WEXITSTATUS(status);
}
