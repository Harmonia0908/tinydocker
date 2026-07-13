#ifndef TINYDOCKER_CORE_SAFETY_H
#define TINYDOCKER_CORE_SAFETY_H

#include <stddef.h>
#include <stdint.h>

int td_validate_name(const char *name, size_t max_length,
                     char *error, size_t error_size);
int td_parse_long(const char *text, long minimum, long maximum, long *value,
                  char *error, size_t error_size);
int td_parse_port_mapping(const char *text, int *host_port, int *container_port,
                          char *error, size_t error_size);
int td_parse_ipv4_cidr(const char *text, uint32_t *network, unsigned int *prefix,
                       char *error, size_t error_size);
int td_build_named_path(const char *base, const char *directory, const char *name,
                        char *output, size_t output_size,
                        char *error, size_t error_size);
int td_join_rootfs_path(const char *rootfs, const char *container_path,
                        char *output, size_t output_size,
                        char *error, size_t error_size);
int td_parse_proc_stat_start_time(const char *stat_line,
                                  unsigned long long *start_time,
                                  char *error, size_t error_size);
int td_make_veth_name(const char *container_name, char *output,
                      size_t output_size);
int td_make_veth_peer_name(const char *container_name, char *output,
                           size_t output_size);
int td_archive_entry_is_safe(const char *entry_name);

#endif
