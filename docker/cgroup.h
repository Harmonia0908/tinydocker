#ifndef CGROUP_H
#define CGROUP_H

#include <stddef.h>
#include <sys/types.h>

int init_cgroup(const char *container_name);
int remove_cgroup(const char *container_name);
int apply_cgroup_limit_to_pid(const char *container_name, int pid);
int set_cgroup_limits(const char *container_name, int cpu, int memory,
                      const char *cpuset);
int get_container_processes_id(const char *container_name, int *pid_list,
                               size_t capacity);
int write_pid_to_cgroup_procs(int pid, const char *cgroup_procs_path);
int get_cgroup_files(pid_t pid, char *cgroup_files[], int limit);
int get_container_cgroup_path(const char *container_name, char *cgroup_path,
                              size_t cgroup_path_size);

#endif
