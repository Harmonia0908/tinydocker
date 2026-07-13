#define _POSIX_C_SOURCE 200809L

#include "safety.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
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

int td_validate_name(const char *name, size_t max_length,
                     char *error, size_t error_size)
{
    size_t length;
    const unsigned char *cursor;

    if (name == NULL || name[0] == '\0') {
        set_error(error, error_size, "name must not be empty");
        return -1;
    }
    length = strlen(name);
    if (length > max_length) {
        set_error(error, error_size, "name length must be <= %zu", max_length);
        return -1;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        set_error(error, error_size, "name is a reserved path component");
        return -1;
    }

    for (cursor = (const unsigned char *)name; *cursor != '\0'; cursor++) {
        if (isalnum(*cursor) != 0 || *cursor == '.' || *cursor == '_' ||
            *cursor == '-') {
            continue;
        }
        set_error(error, error_size,
                  "name contains invalid byte 0x%02x; allowed: [A-Za-z0-9._-]",
                  (unsigned int)*cursor);
        return -1;
    }
    return 0;
}

int td_parse_long(const char *text, long minimum, long maximum, long *value,
                  char *error, size_t error_size)
{
    char *end = NULL;
    long parsed;

    if (text == NULL || text[0] == '\0' || value == NULL || minimum > maximum) {
        set_error(error, error_size, "invalid integer input");
        return -1;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        set_error(error, error_size, "value must be a base-10 integer: %s", text);
        return -1;
    }
    if (parsed < minimum || parsed > maximum) {
        set_error(error, error_size, "integer must be in range %ld..%ld",
                  minimum, maximum);
        return -1;
    }
    *value = parsed;
    return 0;
}

int td_parse_port_mapping(const char *text, int *host_port, int *container_port,
                          char *error, size_t error_size)
{
    const char *separator;
    char host[8] = {0};
    char container[8] = {0};
    size_t host_length;
    size_t container_length;
    long parsed_host;
    long parsed_container;

    if (text == NULL || host_port == NULL || container_port == NULL) {
        set_error(error, error_size, "port mapping must be host:container");
        return -1;
    }
    separator = strchr(text, ':');
    if (separator == NULL || separator == text || separator[1] == '\0' ||
        strchr(separator + 1, ':') != NULL) {
        set_error(error, error_size, "port mapping must be host:container");
        return -1;
    }
    host_length = (size_t)(separator - text);
    container_length = strlen(separator + 1);
    if (host_length >= sizeof(host) || container_length >= sizeof(container)) {
        set_error(error, error_size, "port value is too long");
        return -1;
    }
    memcpy(host, text, host_length);
    memcpy(container, separator + 1, container_length);
    if (td_parse_long(host, 1, 65535, &parsed_host, error, error_size) != 0 ||
        td_parse_long(container, 1, 65535, &parsed_container,
                      error, error_size) != 0) {
        return -1;
    }
    *host_port = (int)parsed_host;
    *container_port = (int)parsed_container;
    return 0;
}

int td_parse_ipv4_cidr(const char *text, uint32_t *network, unsigned int *prefix,
                       char *error, size_t error_size)
{
    const char *slash;
    char address_text[INET_ADDRSTRLEN] = {0};
    char prefix_text[4] = {0};
    struct in_addr address;
    size_t address_length;
    size_t prefix_length;
    long parsed_prefix;
    uint32_t host_address;
    uint32_t mask;

    if (text == NULL || network == NULL || prefix == NULL) {
        set_error(error, error_size, "CIDR must be an IPv4 address and prefix");
        return -1;
    }
    slash = strchr(text, '/');
    if (slash == NULL || slash == text || slash[1] == '\0' ||
        strchr(slash + 1, '/') != NULL) {
        set_error(error, error_size, "CIDR must have address/prefix form");
        return -1;
    }
    address_length = (size_t)(slash - text);
    prefix_length = strlen(slash + 1);
    if (address_length >= sizeof(address_text) ||
        prefix_length >= sizeof(prefix_text)) {
        set_error(error, error_size, "CIDR is too long");
        return -1;
    }
    memcpy(address_text, text, address_length);
    memcpy(prefix_text, slash + 1, prefix_length);
    if (inet_pton(AF_INET, address_text, &address) != 1) {
        set_error(error, error_size, "CIDR contains an invalid IPv4 address");
        return -1;
    }
    if (td_parse_long(prefix_text, 0, 32, &parsed_prefix,
                      error, error_size) != 0) {
        return -1;
    }
    host_address = ntohl(address.s_addr);
    mask = parsed_prefix == 0 ? UINT32_C(0) :
        UINT32_MAX << (32U - (unsigned int)parsed_prefix);
    *network = host_address & mask;
    *prefix = (unsigned int)parsed_prefix;
    return 0;
}

int td_build_named_path(const char *base, const char *directory, const char *name,
                        char *output, size_t output_size,
                        char *error, size_t error_size)
{
    int written;

    if (base == NULL || base[0] != '/' || directory == NULL ||
        directory[0] == '\0' || strchr(directory, '/') != NULL ||
        output == NULL || output_size == 0U) {
        set_error(error, error_size, "invalid runtime path arguments");
        return -1;
    }
    if (td_validate_name(name, 127U, error, error_size) != 0) {
        return -1;
    }
    written = snprintf(output, output_size, "%s%s%s/%s", base,
                       base[strlen(base) - 1U] == '/' ? "" : "/",
                       directory, name);
    if (written < 0 || (size_t)written >= output_size) {
        set_error(error, error_size, "runtime path is too long");
        return -1;
    }
    return 0;
}

static int has_unsafe_path_component(const char *path)
{
    const char *component = path;

    while (*component != '\0') {
        const char *end;
        size_t length;

        while (*component == '/') {
            component++;
        }
        if (*component == '\0') {
            break;
        }
        end = strchr(component, '/');
        length = end == NULL ? strlen(component) : (size_t)(end - component);
        if ((length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.')) {
            return 1;
        }
        component = end == NULL ? component + length : end;
    }
    return 0;
}

int td_join_rootfs_path(const char *rootfs, const char *container_path,
                        char *output, size_t output_size,
                        char *error, size_t error_size)
{
    int written;

    if (rootfs == NULL || rootfs[0] != '/' || output == NULL ||
        output_size == 0U) {
        set_error(error, error_size, "invalid rootfs path arguments");
        return -1;
    }
    if (container_path == NULL || container_path[0] != '/') {
        set_error(error, error_size, "container path must be absolute");
        return -1;
    }
    if (strcmp(container_path, "/") == 0) {
        set_error(error, error_size, "mounting over the container root is not allowed");
        return -1;
    }
    if (has_unsafe_path_component(container_path) != 0) {
        set_error(error, error_size, "container path contains traversal component");
        return -1;
    }
    written = snprintf(output, output_size, "%s%s", rootfs,
                       rootfs[strlen(rootfs) - 1U] == '/' ? container_path + 1 :
                       container_path);
    if (written < 0 || (size_t)written >= output_size) {
        set_error(error, error_size, "container path is too long");
        return -1;
    }
    return 0;
}

int td_parse_proc_stat_start_time(const char *stat_line,
                                  unsigned long long *start_time,
                                  char *error, size_t error_size)
{
    const char *open_paren;
    const char *close_paren;
    char *copy;
    char *save = NULL;
    char *token;
    unsigned int token_index = 0;

    if (stat_line == NULL || start_time == NULL) {
        set_error(error, error_size, "invalid /proc stat input");
        return -1;
    }
    open_paren = strchr(stat_line, '(');
    close_paren = strrchr(stat_line, ')');
    if (open_paren == NULL || close_paren == NULL || close_paren < open_paren ||
        close_paren[1] != ' ') {
        set_error(error, error_size, "malformed /proc stat record");
        return -1;
    }
    copy = strdup(close_paren + 2);
    if (copy == NULL) {
        set_error(error, error_size, "out of memory parsing /proc stat");
        return -1;
    }
    token = strtok_r(copy, " \t\r\n", &save);
    while (token != NULL) {
        if (token_index == 19U) {
            char *end = NULL;
            unsigned long long parsed;

            errno = 0;
            parsed = strtoull(token, &end, 10);
            if (errno != 0 || end == token || *end != '\0') {
                free(copy);
                set_error(error, error_size, "invalid process start time");
                return -1;
            }
            *start_time = parsed;
            free(copy);
            return 0;
        }
        token_index++;
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    free(copy);
    set_error(error, error_size, "process start time field is missing");
    return -1;
}

int td_make_veth_name(const char *container_name, char *output,
                      size_t output_size)
{
    uint32_t hash = UINT32_C(2166136261);
    const unsigned char *cursor;
    char error[160] = {0};
    int written;

    if (output == NULL || output_size < 16U ||
        td_validate_name(container_name, 127U, error, sizeof(error)) != 0) {
        errno = EINVAL;
        return -1;
    }
    for (cursor = (const unsigned char *)container_name; *cursor != '\0'; cursor++) {
        hash ^= (uint32_t)*cursor;
        hash *= UINT32_C(16777619);
    }
    written = snprintf(output, output_size, "td%.6s-%06x", container_name,
                       (unsigned int)(hash & UINT32_C(0x00ffffff)));
    return written == 15 ? 0 : -1;
}

int td_make_veth_peer_name(const char *container_name, char *output,
                           size_t output_size)
{
    if (td_make_veth_name(container_name, output, output_size) != 0) {
        return -1;
    }
    output[1] = 'p';
    return 0;
}

int td_archive_entry_is_safe(const char *entry_name)
{
    if (entry_name == NULL || entry_name[0] == '\0' || entry_name[0] == '/' ||
        strchr(entry_name, '\n') != NULL || strchr(entry_name, '\r') != NULL) {
        return 0;
    }
    while (entry_name[0] == '.' && entry_name[1] == '/') {
        entry_name += 2;
    }
    if (entry_name[0] == '\0') {
        return 1;
    }
    return has_unsafe_path_component(entry_name) == 0 ? 1 : 0;
}
