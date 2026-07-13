#define _POSIX_C_SOURCE 200809L

#include "status_codec.h"

#include "safety.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum metadata_field {
    FIELD_PID = 1U << 0,
    FIELD_DETACH = 1U << 1,
    FIELD_ID = 1U << 2,
    FIELD_IMAGE = 1U << 3,
    FIELD_COMMAND = 1U << 4,
    FIELD_CREATED = 1U << 5,
    FIELD_STATUS = 1U << 6,
    FIELD_IP = 1U << 7,
    FIELD_NAME = 1U << 8,
    FIELD_VOLUME_COUNT = 1U << 9,
    FIELD_PID_START = 1U << 10
};

static const unsigned int required_fields =
    FIELD_PID | FIELD_DETACH | FIELD_ID | FIELD_IMAGE | FIELD_COMMAND |
    FIELD_CREATED | FIELD_STATUS | FIELD_IP | FIELD_NAME | FIELD_VOLUME_COUNT;

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

static int copy_field(char *destination, size_t destination_size,
                      const char *value, const char *field,
                      char *error, size_t error_size)
{
    size_t length = strlen(value);

    if (length >= destination_size) {
        set_error(error, error_size, "metadata field is too long: %s", field);
        return -1;
    }
    memcpy(destination, value, length + 1U);
    return 0;
}

static int claim_field(unsigned int *seen, unsigned int field,
                       const char *name, char *error, size_t error_size)
{
    if ((*seen & field) != 0U) {
        set_error(error, error_size, "duplicate metadata field: %s", name);
        return -1;
    }
    *seen |= field;
    return 0;
}

static int parse_unsigned_long_long(const char *text, uint64_t *value,
                                    const char *field,
                                    char *error, size_t error_size)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        set_error(error, error_size, "invalid numeric metadata field: %s", field);
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_key_value(char *key, const char *value,
                           struct container_info *info, unsigned int *seen,
                           char *error, size_t error_size)
{
    long parsed;

    if (strcmp(key, "pid") == 0) {
        if (claim_field(seen, FIELD_PID, key, error, error_size) != 0 ||
            td_parse_long(value, 1, INT_MAX, &parsed, error, error_size) != 0) {
            return -1;
        }
        info->pid = (int)parsed;
    } else if (strcmp(key, "pid_start_time") == 0) {
        if (claim_field(seen, FIELD_PID_START, key, error, error_size) != 0 ||
            parse_unsigned_long_long(value, &info->pid_start_time, key,
                                     error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "detach") == 0) {
        if (claim_field(seen, FIELD_DETACH, key, error, error_size) != 0 ||
            td_parse_long(value, 0, 1, &parsed, error, error_size) != 0) {
            return -1;
        }
        info->detach = (int)parsed;
    } else if (strcmp(key, "container_id") == 0) {
        if (claim_field(seen, FIELD_ID, key, error, error_size) != 0 ||
            copy_field(info->container_id, sizeof(info->container_id), value,
                       key, error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "image") == 0) {
        if (claim_field(seen, FIELD_IMAGE, key, error, error_size) != 0 ||
            copy_field(info->image, sizeof(info->image), value, key,
                       error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "command") == 0) {
        if (claim_field(seen, FIELD_COMMAND, key, error, error_size) != 0 ||
            copy_field(info->command, sizeof(info->command), value, key,
                       error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "created") == 0) {
        if (claim_field(seen, FIELD_CREATED, key, error, error_size) != 0 ||
            copy_field(info->created, sizeof(info->created), value, key,
                       error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "status") == 0) {
        if (claim_field(seen, FIELD_STATUS, key, error, error_size) != 0 ||
            (strcmp(value, "RUNNING") != 0 && strcmp(value, "STOPPED") != 0 &&
             strcmp(value, "EXITED") != 0)) {
            set_error(error, error_size, "invalid metadata status: %s", value);
            return -1;
        }
        (void)copy_field(info->status, sizeof(info->status), value, key,
                         error, error_size);
    } else if (strcmp(key, "ip_addr") == 0) {
        if (claim_field(seen, FIELD_IP, key, error, error_size) != 0 ||
            copy_field(info->ip_addr, sizeof(info->ip_addr), value, key,
                       error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "name") == 0) {
        if (claim_field(seen, FIELD_NAME, key, error, error_size) != 0 ||
            copy_field(info->name, sizeof(info->name), value, key,
                       error, error_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "volume_cnt") == 0) {
        if (claim_field(seen, FIELD_VOLUME_COUNT, key, error, error_size) != 0 ||
            td_parse_long(value, 0,
                          (long)(sizeof(info->volumes) / sizeof(info->volumes[0])),
                          &parsed, error, error_size) != 0) {
            return -1;
        }
        info->volume_cnt = (int)parsed;
    } else {
        set_error(error, error_size, "unknown metadata field: %s", key);
        return -1;
    }
    return 0;
}

int td_parse_container_info(const char *data, size_t data_size,
                            const char *expected_name,
                            struct container_info *info,
                            char *error, size_t error_size)
{
    char *copy;
    char *save = NULL;
    char *line;
    unsigned int seen = 0U;
    int volume_index = 0;

    if (data == NULL || info == NULL || expected_name == NULL) {
        set_error(error, error_size, "invalid metadata input");
        return -1;
    }
    if (memchr(data, '\0', data_size) != NULL) {
        set_error(error, error_size, "metadata contains a NUL byte");
        return -1;
    }
    copy = malloc(data_size + 1U);
    if (copy == NULL) {
        set_error(error, error_size, "out of memory parsing metadata");
        return -1;
    }
    memcpy(copy, data, data_size);
    copy[data_size] = '\0';
    memset(info, 0, sizeof(*info));

    line = strtok_r(copy, "\n", &save);
    while (line != NULL) {
        size_t line_length = strlen(line);

        if (line_length > 0U && line[line_length - 1U] == '\r') {
            line[line_length - 1U] = '\0';
        }
        if ((seen & FIELD_VOLUME_COUNT) != 0U) {
            if (volume_index >= info->volume_cnt) {
                free(copy);
                set_error(error, error_size, "metadata has extra volume entries");
                return -1;
            }
            if (copy_field(info->volumes[volume_index],
                           sizeof(info->volumes[volume_index]), line, "volume",
                           error, error_size) != 0) {
                free(copy);
                return -1;
            }
            volume_index++;
        } else {
            char *separator = strchr(line, '=');

            if (separator == NULL || separator == line) {
                free(copy);
                set_error(error, error_size, "malformed metadata line");
                return -1;
            }
            *separator = '\0';
            if (parse_key_value(line, separator + 1, info, &seen,
                                error, error_size) != 0) {
                free(copy);
                return -1;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    free(copy);

    if ((seen & required_fields) != required_fields) {
        set_error(error, error_size, "metadata is missing required fields");
        return -1;
    }
    if (volume_index != info->volume_cnt) {
        set_error(error, error_size, "metadata volume count does not match entries");
        return -1;
    }
    if (td_validate_name(info->name, 127U, error, error_size) != 0 ||
        strcmp(info->name, expected_name) != 0) {
        set_error(error, error_size, "metadata name does not match file name");
        return -1;
    }
    return 0;
}

static int contains_record_separator(const char *value)
{
    return strchr(value, '\n') != NULL || strchr(value, '\r') != NULL;
}

int td_write_container_info(FILE *stream, const struct container_info *info,
                            char *error, size_t error_size)
{
    int maximum_volumes;

    if (stream == NULL || info == NULL) {
        set_error(error, error_size, "invalid metadata output");
        return -1;
    }
    maximum_volumes = (int)(sizeof(info->volumes) / sizeof(info->volumes[0]));
    if (info->pid <= 0 || info->volume_cnt < 0 ||
        info->volume_cnt > maximum_volumes ||
        (strcmp(info->status, "RUNNING") != 0 &&
         strcmp(info->status, "STOPPED") != 0 &&
         strcmp(info->status, "EXITED") != 0) ||
        contains_record_separator(info->image) != 0 ||
        contains_record_separator(info->command) != 0 ||
        contains_record_separator(info->name) != 0) {
        set_error(error, error_size, "container metadata contains invalid values");
        return -1;
    }
    if (fprintf(stream,
                "pid=%d\npid_start_time=%llu\ndetach=%d\ncontainer_id=%s\n"
                "image=%s\ncommand=%s\ncreated=%s\nstatus=%s\nip_addr=%s\n"
                "name=%s\nvolume_cnt=%d",
                info->pid, (unsigned long long)info->pid_start_time, info->detach,
                info->container_id, info->image, info->command, info->created,
                info->status, info->ip_addr, info->name, info->volume_cnt) < 0) {
        set_error(error, error_size, "failed to write metadata header");
        return -1;
    }
    for (int index = 0; index < info->volume_cnt; index++) {
        if (contains_record_separator(info->volumes[index]) != 0 ||
            fprintf(stream, "\n%s", info->volumes[index]) < 0) {
            set_error(error, error_size, "failed to write metadata volume");
            return -1;
        }
    }
    if (fputc('\n', stream) == EOF) {
        set_error(error, error_size, "failed to finish metadata record");
        return -1;
    }
    return 0;
}
