#ifndef TINYDOCKER_CORE_NETWORK_STATE_H
#define TINYDOCKER_CORE_NETWORK_STATE_H

#include <stddef.h>
#include <stdint.h>

#define TD_NETWORK_MAX_RECORDS 128U
#define TD_NETWORK_MAX_USED_IPS 128U
#define TD_NETWORK_NAME_SIZE 16U
#define TD_NETWORK_DRIVER_SIZE 16U
#define TD_NETWORK_CIDR_SIZE 32U

struct td_network_record {
    char name[TD_NETWORK_NAME_SIZE];
    char driver[TD_NETWORK_DRIVER_SIZE];
    char cidr[TD_NETWORK_CIDR_SIZE];
    uint32_t used_ips[TD_NETWORK_MAX_USED_IPS];
    size_t used_ip_count;
};

int td_parse_network_record(const char *line, struct td_network_record *record,
                            char *error, size_t error_size);
int td_format_network_record(const struct td_network_record *record,
                             char *output, size_t output_size,
                             char *error, size_t error_size);
int td_network_record_names_are_unique(const struct td_network_record *records,
                                       size_t record_count,
                                       char *error, size_t error_size);

#endif
