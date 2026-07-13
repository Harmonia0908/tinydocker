#define _POSIX_C_SOURCE 200809L

#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for_child(pid_t child)
{
    int status = 0;

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

int td_run_command(char *const arguments[])
{
    pid_t child;

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
    return wait_for_child(child);
}

int td_capture_command(char *const arguments[], char **output,
                       size_t *output_size)
{
    enum { INITIAL_CAPACITY = 4096, MAX_CAPTURE_SIZE = 1024 * 1024 };
    int descriptors[2] = {-1, -1};
    pid_t child;
    char *buffer = NULL;
    size_t capacity = INITIAL_CAPACITY;
    size_t used = 0U;
    int command_result;

    if (arguments == NULL || arguments[0] == NULL || output == NULL ||
        output_size == NULL) {
        errno = EINVAL;
        return -1;
    }
    *output = NULL;
    *output_size = 0U;
    if (pipe(descriptors) != 0) {
        return -1;
    }
    child = fork();
    if (child < 0) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return -1;
    }
    if (child == 0) {
        (void)close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        (void)close(descriptors[1]);
        execvp(arguments[0], arguments);
        _exit(127);
    }
    (void)close(descriptors[1]);
    buffer = malloc(capacity);
    if (buffer == NULL) {
        (void)close(descriptors[0]);
        (void)kill(child, SIGTERM);
        (void)wait_for_child(child);
        return -1;
    }
    for (;;) {
        ssize_t count;

        if (used + 1U == capacity) {
            size_t new_capacity = capacity * 2U;
            char *larger;

            if (new_capacity > MAX_CAPTURE_SIZE) {
                free(buffer);
                (void)close(descriptors[0]);
                (void)kill(child, SIGTERM);
                (void)wait_for_child(child);
                errno = EOVERFLOW;
                return -1;
            }
            larger = realloc(buffer, new_capacity);
            if (larger == NULL) {
                free(buffer);
                (void)close(descriptors[0]);
                (void)kill(child, SIGTERM);
                (void)wait_for_child(child);
                return -1;
            }
            buffer = larger;
            capacity = new_capacity;
        }
        count = read(descriptors[0], buffer + used, capacity - used - 1U);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            free(buffer);
            (void)close(descriptors[0]);
            (void)wait_for_child(child);
            return -1;
        }
        if (count == 0) {
            break;
        }
        used += (size_t)count;
    }
    (void)close(descriptors[0]);
    command_result = wait_for_child(child);
    buffer[used] = '\0';
    *output = buffer;
    *output_size = used;
    return command_result;
}
