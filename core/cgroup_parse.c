#define _POSIX_C_SOURCE 200809L

#include "cgroup_parse.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(args, format);
    (void)vsnprintf(error, error_size, format, args);
    va_end(args);
}

int td_parse_cgroup_stat(const char *contents, const char *wanted_key,
                         char *value, size_t value_size,
                         char *error, size_t error_size)
{
    char *copy;
    char *save = NULL;
    char *line;

    if (contents == NULL || wanted_key == NULL || wanted_key[0] == '\0' ||
        value == NULL || value_size == 0U) {
        set_error(error, error_size, "invalid cgroup stat input");
        return -1;
    }
    copy = strdup(contents);
    if (copy == NULL) {
        set_error(error, error_size, "out of memory parsing cgroup stat");
        return -1;
    }
    line = strtok_r(copy, "\n", &save);
    while (line != NULL) {
        char key[128] = {0};
        char parsed_value[128] = {0};
        char extra[2] = {0};

        if (sscanf(line, "%127s %127s %1s", key, parsed_value, extra) == 2 &&
            strcmp(key, wanted_key) == 0) {
            char *end = NULL;

            errno = 0;
            (void)strtoull(parsed_value, &end, 10);
            if (errno != 0 || end == parsed_value || *end != '\0') {
                free(copy);
                set_error(error, error_size, "cgroup stat %s is not numeric",
                          wanted_key);
                return -1;
            }
            if (snprintf(value, value_size, "%s", parsed_value) < 0 ||
                strlen(parsed_value) >= value_size) {
                free(copy);
                set_error(error, error_size, "cgroup stat value is too long");
                return -1;
            }
            free(copy);
            return 0;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(copy);
    set_error(error, error_size, "cgroup stat key not found: %s", wanted_key);
    return -1;
}

int td_format_bytes(const char *raw, char *output, size_t output_size,
                    char *error, size_t error_size)
{
    char *end = NULL;
    unsigned long long bytes;
    int written;

    if (raw == NULL || output == NULL || output_size == 0U) {
        set_error(error, error_size, "invalid byte value");
        return -1;
    }
    if (strcmp(raw, "max") == 0 || strcmp(raw, "N/A") == 0) {
        written = snprintf(output, output_size, "%s", raw);
    } else {
        errno = 0;
        bytes = strtoull(raw, &end, 10);
        if (errno != 0 || end == raw || *end != '\0') {
            set_error(error, error_size, "byte value must be numeric or max");
            return -1;
        }
        if (bytes < 1024ULL) {
            written = snprintf(output, output_size, "%lluB", bytes);
        } else if (bytes < 1024ULL * 1024ULL) {
            written = snprintf(output, output_size, "%.1fKB",
                               (double)bytes / 1024.0);
        } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
            written = snprintf(output, output_size, "%.1fMB",
                               (double)bytes / 1024.0 / 1024.0);
        } else {
            written = snprintf(output, output_size, "%.1fGB",
                               (double)bytes / 1024.0 / 1024.0 / 1024.0);
        }
    }
    if (written < 0 || (size_t)written >= output_size) {
        set_error(error, error_size, "formatted byte value is too long");
        return -1;
    }
    return 0;
}
