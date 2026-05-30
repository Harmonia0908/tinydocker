#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include "../util/utils.h"
#include "../logger/log.h"

#define CGROUP_ROOT "/sys/fs/cgroup"
#define TINYDOCKER_PREFIX "tinydocker"
static char *cgroup_base = "/sys/fs/cgroup/system.slice";

static int write_file(char *path, char *value) {
    int fd = open(path, O_WRONLY|O_TRUNC);
    if (fd < 0) {
        log_error("failed to open cgroup file %s, err:%s", path, strerror(errno));
        return -1;
    }

    size_t value_len = strlen(value);
    ssize_t written = write(fd, value, value_len);
    int write_failed = written < 0 || (size_t) written != value_len;
    if (write_failed) {
        log_error("failed to write cgroup file %s, err:%s", path, strerror(errno));
    }

    if (close(fd) != 0) {
        log_error("failed to close cgroup file %s, err:%s", path, strerror(errno));
        return -1;
    }

    return write_failed ? -1 : 0;
}

int get_container_cgroup_path(char *container_name, char *cgroup_path, size_t cgroup_path_size) {
    int ret = snprintf(cgroup_path, cgroup_path_size, "%s/%s-%s", cgroup_base, TINYDOCKER_PREFIX, container_name);
    if (ret < 0 || (size_t) ret >= cgroup_path_size) {
        log_error("container cgroup path too long: %s", container_name);
        return -1;
    }
    return 0;
}

static int get_container_cgroup_file_path(char *container_name, char *file_name, char *path, size_t path_size) {
    int ret = snprintf(path, path_size, "%s/%s-%s/%s", cgroup_base, TINYDOCKER_PREFIX, container_name, file_name);
    if (ret < 0 || (size_t) ret >= path_size) {
        log_error("container cgroup file path too long: %s/%s", container_name, file_name);
        return -1;
    }
    return 0;
}

int write_pid_to_cgroup_procs(int pid, char *cgroup_procs_path) {    
    int fd = open(cgroup_procs_path, O_WRONLY|O_APPEND);
    if (fd < 0) {
        log_error("failed to open cgroup file: %s", cgroup_procs_path);
        return -1;
    }

    char pid_str[100] = {0};
    sprintf(pid_str, "%d\n", pid);
    int ret = write(fd, pid_str, strlen(pid_str));
    close(fd);
    return ret < 0 ? -1 : 0;
}

int init_cgroup(char *container_name) {
    if (!path_exist(cgroup_base)) {
        log_error("cgroup_base: %s not exists, created it", cgroup_base);
        make_path(cgroup_base);
    }

    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return -1;
    }
    log_info("creating cgroup at: %s", cgroup_path);
    int ret = make_path(cgroup_path);
    if (ret == -1) {
        log_error("init cgroup error, failed to create %s", cgroup_path);
    }
    return ret;
}

int remove_cgroup(char *container_name) {
    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return -1;
    }
    return rmdir(cgroup_path);
}

int set_mem_limit(char *container_name, int mem_max) {
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


int set_cpu_limit(char *container_name, int cpu_time) {
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


int set_cpuset_limit(char *container_name, char *cpus) {
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


int apply_cgroup_limit_to_pid(char *container_name, int pid) {
    char procs[256] = {0};
    if (get_container_cgroup_file_path(container_name, "cgroup.procs", procs, sizeof(procs)) != 0) {
        return -1;
    }
    return write_pid_to_cgroup_procs(pid, procs);
}


int set_cgroup_limits(char *container_name, int cpu, int memory, char *cpuset) {
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

int get_container_processes_id(char *container_name, int *pid_list) {
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

    char line[32];
    int pid_cnt = 0;
    while (fgets(line, sizeof(line), file)) {
        strtok(line, "\n");  // 去除行尾的换行符
        pid_list[pid_cnt++] = atoi(line); //转为整数
    }

    fclose(file);
    return pid_cnt;
}



int get_cgroup_files(pid_t pid, char *cgroup_files[], int limit) {
    const int MAX_BUF = 1024;
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
        char spliter_cnt = 0;
        int len = strlen(line);
        for (int i = 0; i < len; i++) {
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
