#ifndef TINYDOCKER_CORE_STATUS_CODEC_H
#define TINYDOCKER_CORE_STATUS_CODEC_H

#include <stddef.h>
#include <stdio.h>

#include "container_state.h"

int td_parse_container_info(const char *data, size_t data_size,
                            const char *expected_name,
                            struct container_info *info,
                            char *error, size_t error_size);
int td_write_container_info(FILE *stream, const struct container_info *info,
                            char *error, size_t error_size);

#endif
