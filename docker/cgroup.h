#ifndef CGROUP_H
#define CGROUP_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

struct cgroup_config {
    int cpu;
    int memory;
    const char *cpuset;
};

struct cgroup_state {
    char path[PATH_MAX];
    int created;
};

/* state must be zero-initialized and cleaned before it is prepared again. */
int cgroup_prepare(const char *container_name,
                   const struct cgroup_config *config,
                   struct cgroup_state *state);
int cgroup_apply(struct cgroup_state *state, pid_t pid);
int cgroup_cleanup(struct cgroup_state *state);

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
