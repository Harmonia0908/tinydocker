#ifndef STATUS_INFO_H
#define STATUS_INFO_H

#include <stddef.h>
#include "../core/container_state.h"


int create_container_info(struct docker_run_arguments *args, int pid, enum container_status status, char *ip_addr, int created_timestamp, struct container_info *info);
int write_container_info(char *container_name, struct container_info *info);
int read_container_info(const char *container_name, struct container_info *info);
int update_container_status(char *container_name, enum container_status status);
int refresh_container_status_if_needed(const char *container_name, struct container_info *info);
int list_containers_info(struct container_info *container_info_list, size_t capacity);
int remove_status_info(char *container_name);
#endif
