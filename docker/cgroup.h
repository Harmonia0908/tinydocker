#ifndef CGROUP_H
#define CGROUP_H

#include <stddef.h>
#include <sys/types.h>

int init_cgroup(char *container_name);
int remove_cgroup(char *container_name);
int apply_cgroup_limit_to_pid(char *container_name, int pid);
int set_cgroup_limits(char *container_name, int cpu, int memory, char *cpuset);
int get_container_processes_id(char *container_name, int *pid_list);
int write_pid_to_cgroup_procs(int pid, char *cgroup_procs_path);
int get_cgroup_files(pid_t pid, char *cgroup_files[], int limit);
int get_container_cgroup_path(char *container_name, char *cgroup_path, size_t cgroup_path_size);

#endif
