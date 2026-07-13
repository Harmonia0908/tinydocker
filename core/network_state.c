#define _POSIX_C_SOURCE 200809L

#include "network_state.h"

#include "safety.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static int copy_value(char *destination, size_t destination_size,
                      const char *value, const char *field,
                      char *error, size_t error_size)
{
    size_t length = strlen(value);

    if (length == 0U || length >= destination_size) {
        set_error(error, error_size, "invalid network field length: %s", field);
        return -1;
    }
    memcpy(destination, value, length + 1U);
    return 0;
}

static int validate_cidr(const char *cidr, char *error, size_t error_size)
{
    char address_text[INET_ADDRSTRLEN] = {0};
    const char *slash = strchr(cidr, '/');
    struct in_addr address;
    uint32_t network = 0U;
    unsigned int prefix = 0U;
    size_t address_length;

    if (slash == NULL || td_parse_ipv4_cidr(cidr, &network, &prefix,
                                             error, error_size) != 0) {
        return -1;
    }
    if (prefix > 30U) {
        set_error(error, error_size,
                  "bridge CIDR needs room for gateway and container addresses");
        return -1;
    }
    address_length = (size_t)(slash - cidr);
    if (address_length == 0U || address_length >= sizeof(address_text)) {
        set_error(error, error_size, "invalid network CIDR address");
        return -1;
    }
    memcpy(address_text, cidr, address_length);
    if (inet_pton(AF_INET, address_text, &address) != 1 ||
        ntohl(address.s_addr) != network) {
        set_error(error, error_size, "CIDR address must be the network address");
        return -1;
    }
    return 0;
}

static int parse_used_ips(char *text, struct td_network_record *record,
                          char *error, size_t error_size)
{
    char *cursor = text;

    while (*cursor != '\0') {
        char *separator = strchr(cursor, ';');
        char *end = NULL;
        unsigned long long parsed;

        if (record->used_ip_count >= TD_NETWORK_MAX_USED_IPS) {
            set_error(error, error_size, "too many allocated network addresses");
            return -1;
        }
        if (separator != NULL) {
            if (separator == cursor) {
                set_error(error, error_size, "empty allocated network address");
                return -1;
            }
            *separator = '\0';
        }
        errno = 0;
        parsed = strtoull(cursor, &end, 10);
        if (errno != 0 || end == cursor || *end != '\0' || parsed == 0ULL ||
            parsed > UINT32_MAX) {
            set_error(error, error_size, "invalid allocated network address");
            return -1;
        }
        for (size_t index = 0U; index < record->used_ip_count; index++) {
            if (record->used_ips[index] == (uint32_t)parsed) {
                set_error(error, error_size, "duplicate allocated network address");
                return -1;
            }
        }
        record->used_ips[record->used_ip_count++] = (uint32_t)parsed;
        if (separator == NULL) {
            break;
        }
        cursor = separator + 1;
    }
    return 0;
}

static int validate_allocated_addresses(const struct td_network_record *record,
                                        char *error, size_t error_size)
{
    uint32_t network = 0U;
    unsigned int prefix = 0U;
    uint32_t mask;
    uint32_t broadcast;

    if (td_parse_ipv4_cidr(record->cidr, &network, &prefix,
                           error, error_size) != 0) {
        return -1;
    }
    mask = prefix == 0U ? UINT32_C(0) : UINT32_MAX << (32U - prefix);
    broadcast = network | ~mask;
    for (size_t index = 0U; index < record->used_ip_count; index++) {
        if (record->used_ips[index] <= network + 1U ||
            record->used_ips[index] >= broadcast) {
            set_error(error, error_size,
                      "allocated address is outside the usable CIDR range");
            return -1;
        }
        for (size_t previous = 0U; previous < index; previous++) {
            if (record->used_ips[previous] == record->used_ips[index]) {
                set_error(error, error_size,
                          "duplicate allocated network address");
                return -1;
            }
        }
    }
    return 0;
}

int td_parse_network_record(const char *line, struct td_network_record *record,
                            char *error, size_t error_size)
{
    enum { MAX_LINE_SIZE = 4096 };
    char *copy;
    char *fields[4] = {0};
    char *cursor;
    size_t length;

    if (line == NULL || record == NULL) {
        set_error(error, error_size, "invalid network record input");
        return -1;
    }
    length = strlen(line);
    if (length == 0U || length >= MAX_LINE_SIZE ||
        strchr(line, '\n') != NULL || strchr(line, '\r') != NULL) {
        set_error(error, error_size, "invalid network record length");
        return -1;
    }
    copy = strdup(line);
    if (copy == NULL) {
        set_error(error, error_size, "out of memory parsing network record");
        return -1;
    }
    fields[0] = copy;
    cursor = copy;
    for (size_t index = 1U; index < 4U; index++) {
        char *separator = strchr(cursor, ':');
        if (separator == NULL) {
            free(copy);
            set_error(error, error_size, "network record is missing fields");
            return -1;
        }
        *separator = '\0';
        fields[index] = separator + 1;
        cursor = separator + 1;
    }
    if (strchr(fields[3], ':') != NULL) {
        free(copy);
        set_error(error, error_size, "network record has extra fields");
        return -1;
    }

    memset(record, 0, sizeof(*record));
    if (td_validate_name(fields[0], TD_NETWORK_NAME_SIZE - 1U,
                         error, error_size) != 0 ||
        strcmp(fields[1], "bridge") != 0 ||
        copy_value(record->name, sizeof(record->name), fields[0], "name",
                   error, error_size) != 0 ||
        copy_value(record->driver, sizeof(record->driver), fields[1], "driver",
                   error, error_size) != 0 ||
        copy_value(record->cidr, sizeof(record->cidr), fields[2], "cidr",
                   error, error_size) != 0 ||
        validate_cidr(record->cidr, error, error_size) != 0 ||
        parse_used_ips(fields[3], record, error, error_size) != 0 ||
        validate_allocated_addresses(record, error, error_size) != 0) {
        if (strcmp(fields[1], "bridge") != 0 && error != NULL && error_size > 0U) {
            set_error(error, error_size, "unsupported network driver");
        }
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

int td_format_network_record(const struct td_network_record *record,
                             char *output, size_t output_size,
                             char *error, size_t error_size)
{
    size_t used;
    int written;

    if (record == NULL || output == NULL || output_size == 0U ||
        record->used_ip_count > TD_NETWORK_MAX_USED_IPS ||
        td_validate_name(record == NULL ? NULL : record->name,
                         TD_NETWORK_NAME_SIZE - 1U, error, error_size) != 0 ||
        strcmp(record->driver, "bridge") != 0 ||
        validate_cidr(record->cidr, error, error_size) != 0 ||
        validate_allocated_addresses(record, error, error_size) != 0) {
        set_error(error, error_size, "invalid network record values");
        return -1;
    }
    written = snprintf(output, output_size, "%s:%s:%s:", record->name,
                       record->driver, record->cidr);
    if (written < 0 || (size_t)written >= output_size) {
        set_error(error, error_size, "network record output is too small");
        return -1;
    }
    used = (size_t)written;
    for (size_t index = 0U; index < record->used_ip_count; index++) {
        if (record->used_ips[index] == 0U) {
            set_error(error, error_size, "invalid allocated network address");
            return -1;
        }
        written = snprintf(output + used, output_size - used, "%u;",
                           record->used_ips[index]);
        if (written < 0 || (size_t)written >= output_size - used) {
            set_error(error, error_size, "network record output is too small");
            return -1;
        }
        used += (size_t)written;
    }
    return 0;
}
