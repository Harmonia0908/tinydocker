#include <limits.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "../logger/log.h"
#include "../util/utils.h"
#include "../cmdparser/cmdparser.h"
#include "container.h"
#include "status_info.h"


char *str_status[] = {"RUNNING", "STOPPED", "EXITED"};

static int copy_string_field(char *dst, size_t dst_size, const char *src, const char *field_name) {
    int ret = 0;

    if (dst == NULL || dst_size == 0) {
        log_error("invalid metadata field buffer: %s", field_name);
        return -1;
    }

    ret = snprintf(dst, dst_size, "%s", src == NULL ? "" : src);
    if (ret < 0 || (size_t) ret >= dst_size) {
        dst[0] = '\0';
        log_error("metadata field too long: %s", field_name);
        return -1;
    }

    return 0;
}

static int append_string_field(char *dst, size_t dst_size, const char *src, const char *field_name) {
    size_t used = 0;
    int ret = 0;

    if (dst == NULL || dst_size == 0) {
        log_error("invalid metadata field buffer: %s", field_name);
        return -1;
    }

    used = strlen(dst);
    if (used >= dst_size) {
        log_error("metadata field has no remaining capacity: %s", field_name);
        return -1;
    }

    ret = snprintf(dst + used, dst_size - used, "%s", src == NULL ? "" : src);
    if (ret < 0 || (size_t) ret >= dst_size - used) {
        dst[used] = '\0';
        log_error("metadata field too long: %s", field_name);
        return -1;
    }

    return 0;
}

static int parse_int_field(const char *value, int *out) {
    char *end = NULL;
    long parsed = 0;

    if (value == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }

    *out = (int) parsed;
    return 0;
}

int create_container_info(struct docker_run_arguments *args, int pid, enum container_status status, char *ip_addr, int created_timestamp, struct container_info *info) {
    /*
    int pid; //容器进程ID;
    int detach//
    char container_id[128]; //容器ID
    char image[128]; //容器镜像
    char command[512]; //容器运行的命令
    char created[20]; //容器创建时间
    char status[10]; //容器状态
    char name[128]; //容器名字
    int volume_cnt; //卷挂载数量
    char volumes[128][1024]; //卷挂载信息
    */
    if (args == NULL || info == NULL) {
        log_error("invalid container info arguments");
        return -1;
    }
    if (args->image == NULL || args->name == NULL || args->container_argv == NULL || args->container_argc <= 0 || args->container_argv[0] == NULL) {
        log_error("missing required container metadata field");
        return -1;
    }
    if (status < CONTAINER_RUNNING || status > CONTAINER_EXITED) {
        log_error("invalid container status: %d", status);
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->pid = pid;
    info->detach = args->detach;

    int container_id_len = snprintf(info->container_id, sizeof(info->container_id), "%d", created_timestamp);
    if (container_id_len < 0 || (size_t) container_id_len >= sizeof(info->container_id)) {
        log_error("metadata field too long: container_id");
        return -1;
    }
    if (copy_string_field(info->image, sizeof(info->image), args->image, "image") != 0) {
        return -1;
    }
    if (copy_string_field(info->command, sizeof(info->command), args->container_argv[0], "command") != 0) {
        return -1;
    }
    for (int i = 1; i < args->container_argc; i++) {
        if (append_string_field(info->command, sizeof(info->command), " ", "command") != 0) {
            return -1;
        }
        if (append_string_field(info->command, sizeof(info->command), args->container_argv[i], "command") != 0) {
            return -1;
        }
    }
    timestamp_to_string(created_timestamp, info->created, sizeof(info->created));
    if (strlen(info->created) != sizeof(info->created) - 1) {
        log_error("failed to format created timestamp");
        return -1;
    }
    if (copy_string_field(info->status, sizeof(info->status), str_status[status], "status") != 0) {
        return -1;
    }
    if (copy_string_field(info->ip_addr, sizeof(info->ip_addr), ip_addr, "ip_addr") != 0) {
        return -1;
    }
    if (copy_string_field(info->name, sizeof(info->name), args->name, "name") != 0) {
        return -1;
    }

    int max_volumes = sizeof(info->volumes) / sizeof(info->volumes[0]);
    info->volume_cnt = args->volume_cnt;
    if (info->volume_cnt < 0) {
        log_warn("invalid volume_cnt %d, use 0", info->volume_cnt);
        info->volume_cnt = 0;
    }
    if (info->volume_cnt > max_volumes) {
        log_warn("volume_cnt %d exceeds limit %d, truncate", info->volume_cnt, max_volumes);
        info->volume_cnt = max_volumes;
    }

    for (int i = 0; i < info->volume_cnt; i++) {
        char *rw = "rw";
        char *ro = "ro";
        char *mode = rw;
        if (args->volumes == NULL || args->volumes[i].host[0] == '\0' || args->volumes[i].container[0] == '\0') {
            log_error("missing volume metadata field");
            return -1;
        }
        if (args->volumes[i].ro == 1) {
            mode = ro;
        }
        int volume_len = snprintf(info->volumes[i], sizeof(info->volumes[i]), "%s:%s:%s", args->volumes[i].host, args->volumes[i].container, mode);
        if (volume_len < 0 || (size_t) volume_len >= sizeof(info->volumes[i])) {
            log_error("metadata field too long: volume");
            return -1;
        }
    }
    return 0;
}

int write_container_info(char *container_name, struct container_info *info) {
    if (!path_exist(CONTAINER_STATUS_INFO_DIR)) {
        make_path(CONTAINER_STATUS_INFO_DIR);
    }
    
    char status_file_path[1024] = {0};
    int path_len = snprintf(status_file_path, sizeof(status_file_path), "%s/%s", CONTAINER_STATUS_INFO_DIR, container_name);
    if (path_len < 0 || (size_t) path_len >= sizeof(status_file_path)) {
        log_error("container info path too long: %s", container_name);
        return -1;
    }

    FILE *file = fopen(status_file_path, "w");
    if (file == NULL) {
        log_error("failed to open file: %s", status_file_path);
        return -1;
    }

    int volume_cnt = info->volume_cnt;
    int max_volumes = sizeof(info->volumes) / sizeof(info->volumes[0]);
    if (volume_cnt < 0) {
        log_warn("container %s has invalid volume_cnt %d, write as 0", container_name, volume_cnt);
        volume_cnt = 0;
    }
    if (volume_cnt > max_volumes) {
        log_warn("container %s volume_cnt %d exceeds limit %d, truncate", container_name, volume_cnt, max_volumes);
        volume_cnt = max_volumes;
    }

    if (fprintf(file, "pid=%d\ndetach=%d\ncontainer_id=%s\nimage=%s\ncommand=%s\ncreated=%s\nstatus=%s\nip_addr=%s\nname=%s\nvolume_cnt=%d", \
    info->pid, info->detach, info->container_id, info->image, info->command, info->created, info->status, info->ip_addr, info->name, volume_cnt) < 0) {
        log_error("failed to write container info header: %s", status_file_path);
        fclose(file);
        return -1;
    }

    for (int i = 0; i < volume_cnt; i++) {
        if (fprintf(file, "\n%s", info->volumes[i]) < 0) {
            log_error("failed to write container info volume: %s", status_file_path);
            fclose(file);
            return -1;
        }
    }

    if (fclose(file) != 0) {
        log_error("failed to close container info file: %s", status_file_path);
        return -1;
    }

    log_info("write container info into %s", status_file_path);
    return 0;
}

int read_container_info(const char *container_name, struct container_info *info) {
    char container_info_file[1024] = {0};
    int path_len = snprintf(container_info_file, sizeof(container_info_file), "%s/%s", CONTAINER_STATUS_INFO_DIR, container_name);
    if (path_len < 0 || (size_t) path_len >= sizeof(container_info_file)) {
        log_error("container info path too long: %s", container_name);
        return -1;
    }

    FILE *file = fopen(container_info_file, "r");
    if (file == NULL) {
        log_error("failed to open container info file: %s", container_info_file);
        return -1;
    }
    
    memset(info, 0, sizeof(*info));
    info->volume_cnt = -1;
    int vol_idx = 0;
    int max_volumes = sizeof(info->volumes) / sizeof(info->volumes[0]);

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        // Remove newline character from the end of the line
        line[strcspn(line, "\n")] = '\0';

        if (info->volume_cnt != -1) {
            if (vol_idx < max_volumes) {
                if (copy_string_field(info->volumes[vol_idx], sizeof(info->volumes[vol_idx]), line, "volume") != 0) {
                    log_warn("invalid volume in container info %s, ignore", container_name);
                } else {
                    vol_idx++;
                }
            } else {
                log_warn("container %s has too many volume entries, ignore: %s", container_name, line);
            }
            continue;
        }
        // Split only on the first '=' so values such as command arguments can contain '='.
        char *sep = strchr(line, '=');
        if (sep == NULL) {
            continue;
        }
        *sep = '\0';
        char *key = line;
        char *value = sep + 1;

        if (strcmp(key, "pid") == 0) {
            if (parse_int_field(value, &info->pid) == -1) {
                log_warn("invalid pid in container info %s: %s", container_name, value);
                info->pid = -1;
            }
        }
        if (strcmp(key, "detach") == 0) {
            if (parse_int_field(value, &info->detach) == -1) {
                log_warn("invalid detach in container info %s: %s", container_name, value);
            }
        }
        if (strcmp(key, "container_id") == 0) {
            if (copy_string_field(info->container_id, sizeof(info->container_id), value, "container_id") != 0) {
                log_warn("invalid container_id in container info %s, ignore", container_name);
            }
        }
        if (strcmp(key, "image") == 0) {
            if (copy_string_field(info->image, sizeof(info->image), value, "image") != 0) {
                log_warn("invalid image in container info %s, ignore", container_name);
            }
        }
        if (strcmp(key, "command") == 0) {
            if (copy_string_field(info->command, sizeof(info->command), value, "command") != 0) {
                log_warn("invalid command in container info %s, ignore", container_name);
            }
        }
        if (strcmp(key, "created") == 0) {
            if (copy_string_field(info->created, sizeof(info->created), value, "created") != 0) {
                log_warn("invalid created time in container info %s, ignore", container_name);
            }
        }
        if (strcmp(key, "status") == 0) {
            if (copy_string_field(info->status, sizeof(info->status), value, "status") != 0) {
                log_warn("invalid status in container info %s, use UNKNOWN", container_name);
                copy_string_field(info->status, sizeof(info->status), "UNKNOWN", "status");
            }
        }
        if (strcmp(key, "ip_addr") == 0) {
            if (copy_string_field(info->ip_addr, sizeof(info->ip_addr), value, "ip_addr") != 0) {
                log_warn("invalid ip_addr in container info %s, ignore", container_name);
            }
        }
        if (strcmp(key, "name") == 0) {
            if (copy_string_field(info->name, sizeof(info->name), value, "name") != 0) {
                log_warn("invalid name in container info %s, keep empty", container_name);
            }
        }
        if (strcmp(key, "volume_cnt") == 0) {
            if (parse_int_field(value, &info->volume_cnt) == -1 || info->volume_cnt < 0) {
                log_warn("invalid volume_cnt in container info %s: %s", container_name, value);
                info->volume_cnt = 0;
            }
            if (info->volume_cnt > max_volumes) {
                log_warn("container %s volume_cnt %d exceeds limit %d", container_name, info->volume_cnt, max_volumes);
                info->volume_cnt = max_volumes;
            }
        }
    }
    fclose(file);

    if (info->volume_cnt == -1) {
        info->volume_cnt = 0; //从头到位没有读到卷挂载信息, 恢复为0;
    } else if (info->volume_cnt > vol_idx) {
        info->volume_cnt = vol_idx;
    }
    return 0;
}

int update_container_status(char *container_name, enum container_status status) {
    struct container_info info;
    if (read_container_info(container_name, &info) == -1) {
        log_error("failed load container info for %s", container_name);
        return -1;
    }
    strcpy(info.status, str_status[status]);
    return write_container_info(container_name, &info);
}

int refresh_container_status_if_needed(const char *container_name, struct container_info *info) {
    if (info == NULL) {
        log_warn("container info is null, skip status refresh");
        return -1;
    }

    if (container_name == NULL || strlen(container_name) == 0) {
        log_warn("container name is empty, skip status refresh");
        return -1;
    }

    if (strcmp(info->status, "RUNNING") != 0) {
        return 0;
    }

    if (info->pid <= 0) {
        log_warn("container %s has invalid pid %d, skip status refresh", info->name, info->pid);
        return 0;
    }

    char proc_path[128] = {0};
    sprintf(proc_path, "/proc/%d", info->pid);
    if (path_exist(proc_path)) {
        return 0;
    }

    strcpy(info->status, str_status[CONTAINER_EXITED]);
    if (write_container_info((char *) container_name, info) != 0) {
        log_error("failed to update container %s status to EXITED", container_name);
        return -1;
    }

    log_info("container %s pid %d not exists, mark status as EXITED", container_name, info->pid);
    return 1;
}


int list_containers_info(struct container_info *container_info_list, size_t capacity) {
    if (container_info_list == NULL || capacity == 0) {
        log_warn("invalid container info list buffer");
        return -1;
    }

    DIR *dir = opendir(CONTAINER_STATUS_INFO_DIR);
    if (dir == NULL) {
        log_error("failed to open directory: %s", CONTAINER_STATUS_INFO_DIR);
        return -1;
    }

    size_t container_cnt = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".." directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Construct the absolute path of the file
        char file_path[PATH_MAX];
        int path_len = snprintf(file_path, sizeof(file_path), "%s/%s", CONTAINER_STATUS_INFO_DIR, entry->d_name);
        if (path_len < 0 || (size_t) path_len >= sizeof(file_path)) {
            log_warn("container info path too long, skip: %s", entry->d_name);
            continue;
        }
        //printf("%s\n", file_path);
        // Check if the entry is a file
        struct stat file_stat;
        if (stat(file_path, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
            if (container_cnt >= capacity) {
                log_warn("container info list capacity %zu reached, skip remaining entries", capacity);
                break;
            }

            log_info("load container info from: %s", file_path);

            struct container_info info; // 文件名对应的就是一个容器名
            int ret = read_container_info(entry->d_name, &info);
            if (ret == -1) {
                log_info("failed to load container info from: %s", file_path);
                continue;
            }
            if (refresh_container_status_if_needed(entry->d_name, &info) < 0) {
                log_warn("failed to refresh container status: %s", entry->d_name);
            }
            container_info_list[container_cnt++] = info;
        }
    }

    closedir(dir);
    return (int) container_cnt;
}


int remove_status_info(char *container_name) {
    char status_path[1024] = {0};
    int path_len = snprintf(status_path, sizeof(status_path), "%s/container_info/%s", TINYDOCKER_RUNTIME_DIR, container_name);
    if (path_len < 0 || (size_t) path_len >= sizeof(status_path)) {
        log_error("container info path too long: %s", container_name);
        return -1;
    }
    return remove(status_path);
}
