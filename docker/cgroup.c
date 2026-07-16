#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include "../util/utils.h"
#include "../logger/log.h"
#include "../core/safety.h"
#include "../core/cgroup_parse.h"
#include "../core/config.h"
#include "../cmdparser/cmdparser.h"
#include "cgroup.h"

#define CGROUP_ROOT "/sys/fs/cgroup"
#define TINYDOCKER_PREFIX "tinydocker"
static const char cgroup_base[] = TINYDOCKER_CGROUP_PARENT;

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY|O_TRUNC);
    if (fd < 0) {
        log_error("failed to open cgroup file %s, err:%s", path, strerror(errno));
        return -1;
    }

    size_t value_len = strlen(value);
    size_t offset = 0U;
    int write_failed = 0;
    while (offset < value_len) {
        ssize_t written = write(fd, value + offset, value_len - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            write_failed = 1;
            break;
        }
        offset += (size_t)written;
    }
    if (write_failed) {
        log_error("failed to write cgroup file %s, err:%s", path, strerror(errno));
    }

    if (close(fd) != 0) {
        log_error("failed to close cgroup file %s, err:%s", path, strerror(errno));
        return -1;
    }

    return write_failed ? -1 : 0;
}

int get_container_cgroup_path(const char *container_name, char *cgroup_path, size_t cgroup_path_size) {
    char validation_error[160] = {0};
    if (td_validate_name(container_name, TINYDOCKER_MAX_CONTAINER_NAME,
                         validation_error, sizeof(validation_error)) != 0) {
        log_error("invalid cgroup container name: %s", validation_error);
        return -1;
    }
    int ret = snprintf(cgroup_path, cgroup_path_size, "%s/%s-%s", cgroup_base, TINYDOCKER_PREFIX, container_name);
    if (ret < 0 || (size_t) ret >= cgroup_path_size) {
        log_error("container cgroup path too long: %s", container_name);
        return -1;
    }
    return 0;
}

static int get_container_cgroup_file_path(const char *container_name,
                                          const char *file_name,
                                          char *path, size_t path_size) {
    int ret = snprintf(path, path_size, "%s/%s-%s/%s", cgroup_base, TINYDOCKER_PREFIX, container_name, file_name);
    if (ret < 0 || (size_t) ret >= path_size) {
        log_error("container cgroup file path too long: %s/%s", container_name, file_name);
        return -1;
    }
    return 0;
}

int write_pid_to_cgroup_procs(int pid, const char *cgroup_procs_path) {
    if (pid <= 0 || cgroup_procs_path == NULL) {
        log_error("invalid pid or cgroup.procs path");
        return -1;
    }

    char pid_str[100] = {0};
    int length = snprintf(pid_str, sizeof(pid_str), "%d\n", pid);
    if (length < 0 || (size_t)length >= sizeof(pid_str)) {
        return -1;
    }
    return write_file(cgroup_procs_path, pid_str);
}

int init_cgroup(const char *container_name) {
    if (!path_exist("/sys/fs/cgroup/cgroup.controllers")) {
        log_error("cgroup v2 is not available at /sys/fs/cgroup");
        return -1;
    }
    if (!path_exist(cgroup_base)) {
        log_error("cgroup parent does not exist: %s; refusing to create an "
                  "unknown host cgroup hierarchy", cgroup_base);
        return -1;
    }

    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return -1;
    }
    log_info("creating cgroup at: %s", cgroup_path);
    int ret = mkdir(cgroup_path, S_IRWXU);
    if (ret == -1) {
        log_error("init cgroup error, failed to create %s", cgroup_path);
    }
    return ret;
}

int remove_cgroup(const char *container_name) {
    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return -1;
    }
    if (rmdir(cgroup_path) == 0 || errno == ENOENT) {
        return 0;
    }
    log_error("failed to remove cgroup %s: %s", cgroup_path, strerror(errno));
    return -1;
}

static int set_mem_limit(const char *container_name, int mem_max) {
    if (mem_max <= 0) {
        log_error("invalid mem_max value: %d", mem_max);
        return -1;
    }

    char memory[256] = {0};
    if (get_container_cgroup_file_path(container_name, "memory.max", memory, sizeof(memory)) != 0) {
        return -1;
    }
    char limitation[100] = {0};
    int limitation_len = snprintf(limitation, sizeof(limitation), "%d", mem_max);
    if (limitation_len < 0 || (size_t) limitation_len >= sizeof(limitation)) {
        log_error("memory limitation value too long: %d", mem_max);
        return -1;
    }
    return write_file(memory, limitation);
}


static int set_cpu_limit(const char *container_name, int cpu_time) {
    if (cpu_time < 1000) {
        log_error("invalid cpu.max value: %d", cpu_time);
        return -1;
    }

    char cpu[256] = {0};
    if (get_container_cgroup_file_path(container_name, "cpu.max", cpu, sizeof(cpu)) != 0) {
        return -1;
    }
    char limitation[100] = {0};
    int limitation_len = snprintf(limitation, sizeof(limitation), "%d %d", cpu_time, 100000);
    if (limitation_len < 0 || (size_t) limitation_len >= sizeof(limitation)) {
        log_error("cpu limitation value too long: %d", cpu_time);
        return -1;
    }
    return write_file(cpu, limitation);
}


static int set_cpuset_limit(const char *container_name, const char *cpus) {
    if (cpus == NULL) {
        log_error("invalid cpuset.cpus value, can not be NULL");
        return -1;
    }

    char cpuset_cpus[256] = {0};
    if (get_container_cgroup_file_path(container_name, "cpuset.cpus", cpuset_cpus, sizeof(cpuset_cpus)) != 0) {
        return -1;
    }
    return write_file(cpuset_cpus, cpus);
}


int apply_cgroup_limit_to_pid(const char *container_name, int pid) {
    char procs[256] = {0};
    if (get_container_cgroup_file_path(container_name, "cgroup.procs", procs, sizeof(procs)) != 0) {
        return -1;
    }
    return write_pid_to_cgroup_procs(pid, procs);
}


int set_cgroup_limits(const char *container_name, int cpu, int memory,
                      const char *cpuset) {
    //设置内存限制
    if (memory > 0) {
        if (set_mem_limit(container_name, memory) != 0) {
            log_error("failed set mem limit in cgroup");
            return -1;
        }
    }
    
    //设置cpu限制
    if (cpu >= 1000) {
        if (set_cpu_limit(container_name, cpu) != 0) {
            log_error("failed set cpu limit in cgroup");
            return -1;
        }
    }

    //设置cpuset限制
    if (cpuset != NULL) {
        if (set_cpuset_limit(container_name, cpuset) != 0) {
            log_error("failed set cpu set in cgroup");
            return -1;
        }
    }

    return 0;
}

int cgroup_prepare(const char *container_name,
                   const struct cgroup_config *config,
                   struct cgroup_state *state) {
    int saved_errno;

    if (container_name == NULL || config == NULL || state == NULL) {
        errno = EINVAL;
        log_error("invalid cgroup lifecycle configuration");
        return -1;
    }
    if (state->created != 0) {
        errno = EBUSY;
        log_error("refusing to prepare an already owned cgroup: %s",
                  state->path);
        return -1;
    }
    memset(state, 0, sizeof(*state));
    if (get_container_cgroup_path(container_name, state->path,
                                  sizeof(state->path)) != 0 ||
        init_cgroup(container_name) != 0) {
        perror("failed to init cgroup");
        return -1;
    }
    state->created = 1;
    if (set_cgroup_limits(container_name, config->cpu, config->memory,
                          config->cpuset) == 0) {
        return 0;
    }

    saved_errno = errno == 0 ? EIO : errno;
    log_error("failed to set_cgroup_limits for %s", container_name);
    if (cgroup_cleanup(state) != 0) {
        log_error("failed to roll back cgroup after limit configuration error: %s",
                  state->path);
    }
    errno = saved_errno;
    return -1;
}

int cgroup_apply(struct cgroup_state *state, pid_t pid) {
    char procs_path[PATH_MAX];
    int written;

    if (state == NULL || state->created == 0 || state->path[0] == '\0' ||
        pid <= 0) {
        errno = EINVAL;
        log_error("invalid prepared cgroup state or pid");
        return -1;
    }
    written = snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs",
                       state->path);
    if (written < 0 || (size_t)written >= sizeof(procs_path)) {
        errno = ENAMETOOLONG;
        log_error("cgroup.procs path is too long: %s", state->path);
        return -1;
    }
    return write_pid_to_cgroup_procs((int)pid, procs_path);
}

int cgroup_cleanup(struct cgroup_state *state) {
    if (state == NULL) {
        errno = EINVAL;
        log_error("invalid cgroup state for cleanup");
        return -1;
    }
    if (state->created == 0) {
        return 0;
    }
    if (state->path[0] == '\0') {
        errno = EINVAL;
        log_error("refusing to clean cgroup with an empty owned path");
        return -1;
    }
    if (rmdir(state->path) != 0 && errno != ENOENT) {
        log_error("failed to remove cgroup %s: %s", state->path,
                  strerror(errno));
        return -1;
    }
    state->created = 0;
    state->path[0] = '\0';
    return 0;
}

int get_container_processes_id(const char *container_name, int *pid_list,
                               size_t capacity) {
    char cgroup_procs_path[1024] = {0};
    if (get_container_cgroup_file_path(container_name, "cgroup.procs", cgroup_procs_path, sizeof(cgroup_procs_path)) != 0) {
        return -1;
    }
    if (!path_exist(cgroup_procs_path)) {
        log_error("can not find container by name: %s", container_name);
        return -1;
    }

    FILE *file = fopen(cgroup_procs_path, "r");
    if (file == NULL) {
        log_error("failed to open the file: %s", cgroup_procs_path);
        return -1;
    }

    if (pid_list == NULL || capacity == 0U) {
        log_error("invalid cgroup pid output buffer");
        (void)fclose(file);
        return -1;
    }

    if (capacity > (SIZE_MAX - 1U) / 32U) {
        (void)fclose(file);
        return -1;
    }
    size_t buffer_size = capacity * 32U + 1U;
    char *contents = calloc(buffer_size, 1U);
    if (contents == NULL) {
        (void)fclose(file);
        return -1;
    }
    size_t used = fread(contents, 1, buffer_size - 1U, file);
    if (ferror(file) != 0 || (!feof(file) && used == buffer_size - 1U)) {
        log_error("failed or oversized read from %s", cgroup_procs_path);
        free(contents);
        (void)fclose(file);
        return -1;
    }
    (void)fclose(file);

    char error[160] = {0};
    size_t pid_count = 0U;
    if (td_parse_cgroup_pid_list(contents, pid_list, capacity, &pid_count,
                                 error, sizeof(error)) != 0 ||
        pid_count > (size_t)INT_MAX) {
        log_error("invalid cgroup process list %s: %s", cgroup_procs_path, error);
        free(contents);
        return -1;
    }
    free(contents);
    return (int)pid_count;
}



int get_cgroup_files(pid_t pid, char *cgroup_files[], int limit) {
    enum { MAX_BUF = 1024 };
    char path[MAX_BUF];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        perror("fopen failed");
        return -1;
    }

    int count = 0;
    char line[MAX_BUF];
    while (fgets(line, sizeof(line), file) != NULL && count < limit) {
        char *subsystem = NULL;
        char *cgroup_path = NULL;
        int spliter_cnt = 0;
        size_t len = strlen(line);
        for (size_t i = 0U; i < len; i++) {
            if (line[i] == ':') {
                spliter_cnt++;
                line[i] = '\0';
                if (subsystem == NULL) {
                    subsystem = line + i + 1;
                } else {
                    cgroup_path = line + i + 1;
                }
            }
        }
        
        if (spliter_cnt != 2) {
            continue;
        }

        char *cgroup_file = malloc(strlen(CGROUP_ROOT) + strlen(subsystem) + strlen(cgroup_path) + 16);
        if (cgroup_file == NULL) {
            perror("malloc failed");
            break;
        }

        if (strlen(subsystem) > 0) {
            sprintf(cgroup_file, "%s/%s%s", CGROUP_ROOT, subsystem, cgroup_path);
        } else {
            sprintf(cgroup_file, "%s%s", CGROUP_ROOT, cgroup_path);
        }

        cgroup_file[strcspn(cgroup_file, "\n")] = '\0';
        cgroup_files[count++] = cgroup_file;
    }

    fclose(file);
    return count;
}
