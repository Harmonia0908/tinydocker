#ifndef TINYDOCKER_CORE_CGROUP_PARSE_H
#define TINYDOCKER_CORE_CGROUP_PARSE_H

#include <stddef.h>

int td_parse_cgroup_stat(const char *contents, const char *wanted_key,
                         char *value, size_t value_size,
                         char *error, size_t error_size);
int td_format_bytes(const char *raw, char *output, size_t output_size,
                    char *error, size_t error_size);
int td_parse_cgroup_pid_list(const char *contents, int *pid_list,
                             size_t capacity, size_t *pid_count,
                             char *error, size_t error_size);

#endif
