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
#include "../core/safety.h"
#include "../core/status_codec.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif


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

static int read_process_start_time(pid_t pid, uint64_t *start_time) {
    char path[64] = {0};
    char stat_line[4096] = {0};
    char error[160] = {0};
    FILE *file;
    int path_length;
    unsigned long long parsed_start_time = 0;

    if (pid <= 0 || start_time == NULL) {
        errno = EINVAL;
        return -1;
    }
    path_length = snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    if (fgets(stat_line, sizeof(stat_line), file) == NULL) {
        int saved_errno = errno == 0 ? EIO : errno;
        (void)fclose(file);
        errno = saved_errno;
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    if (td_parse_proc_stat_start_time(stat_line, &parsed_start_time,
                                      error, sizeof(error)) != 0) {
        log_warn("failed to parse process %d start time: %s", pid, error);
        errno = EINVAL;
        return -1;
    }
    *start_time = (uint64_t)parsed_start_time;
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
    if (read_process_start_time(pid, &info->pid_start_time) != 0) {
        log_error("failed to read start time for container process %d: %s",
                  pid, strerror(errno));
        return -1;
    }
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
    char validation_error[160] = {0};
    char temporary_name[256] = {0};
    char final_path[1024] = {0};
    int directory_fd = -1;
    int metadata_fd = -1;
    FILE *file = NULL;
    int result = -1;

    if (container_name == NULL || info == NULL ||
        td_validate_name(container_name, TINYDOCKER_MAX_CONTAINER_NAME,
                         validation_error, sizeof(validation_error)) != 0 ||
        strcmp(container_name, info->name) != 0) {
        log_error("invalid container metadata name: %s", validation_error);
        return -1;
    }
    if (!path_exist(CONTAINER_STATUS_INFO_DIR) &&
        make_path(CONTAINER_STATUS_INFO_DIR) != 0) {
        log_error("failed to create container info directory: %s",
                  CONTAINER_STATUS_INFO_DIR);
        return -1;
    }
    directory_fd = open(CONTAINER_STATUS_INFO_DIR,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        log_error("failed to safely open container info directory: %s",
                  strerror(errno));
        return -1;
    }
    for (unsigned int attempt = 0; attempt < 100U; attempt++) {
        int name_length = snprintf(temporary_name, sizeof(temporary_name),
                                   ".%s.tmp.%ld.%u", container_name,
                                   (long)getpid(), attempt);
        if (name_length < 0 || (size_t)name_length >= sizeof(temporary_name)) {
            log_error("temporary metadata path too long: %s", container_name);
            goto cleanup;
        }
        metadata_fd = openat(directory_fd, temporary_name,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                             S_IRUSR | S_IWUSR);
        if (metadata_fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (metadata_fd < 0) {
        log_error("failed to create temporary metadata for %s: %s",
                  container_name, strerror(errno));
        goto cleanup;
    }
    file = fdopen(metadata_fd, "w");
    if (file == NULL) {
        log_error("failed to open metadata stream for %s: %s",
                  container_name, strerror(errno));
        goto cleanup;
    }
    metadata_fd = -1;
    if (td_write_container_info(file, info, validation_error,
                                sizeof(validation_error)) != 0 ||
        fflush(file) != 0 || fsync(fileno(file)) != 0) {
        log_error("failed to write metadata for %s: %s", container_name,
                  validation_error[0] == '\0' ? strerror(errno) : validation_error);
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = NULL;
        log_error("failed to close metadata for %s: %s",
                  container_name, strerror(errno));
        goto cleanup;
    }
    file = NULL;
    if (renameat(directory_fd, temporary_name, directory_fd, container_name) != 0) {
        log_error("failed to atomically publish metadata for %s: %s",
                  container_name, strerror(errno));
        goto cleanup;
    }
    result = 0;

cleanup:
    if (file != NULL) {
        (void)fclose(file);
    } else if (metadata_fd >= 0) {
        (void)close(metadata_fd);
    }
    if (result != 0 && temporary_name[0] != '\0') {
        (void)unlinkat(directory_fd, temporary_name, 0);
    }
    (void)close(directory_fd);
    if (snprintf(final_path, sizeof(final_path), "%s/%s",
                 CONTAINER_STATUS_INFO_DIR, container_name) > 0 && result == 0) {
        log_info("wrote container info atomically to %s", final_path);
    }
    return result;
}

int read_container_info(const char *container_name, struct container_info *info) {
    enum { MAX_METADATA_SIZE = 1024 * 1024 };
    char validation_error[160] = {0};
    struct stat status;
    char *data = NULL;
    size_t offset = 0U;
    int directory_fd = -1;
    int metadata_fd = -1;
    int result = -1;

    if (info == NULL ||
        td_validate_name(container_name, TINYDOCKER_MAX_CONTAINER_NAME,
                         validation_error, sizeof(validation_error)) != 0) {
        log_error("invalid container metadata name: %s", validation_error);
        return -1;
    }
    directory_fd = open(CONTAINER_STATUS_INFO_DIR,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        log_error("failed to safely open container info directory: %s",
                  strerror(errno));
        goto cleanup;
    }
    metadata_fd = openat(directory_fd, container_name,
                         O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (metadata_fd < 0) {
        log_error("failed to safely open container info for %s: %s",
                  container_name, strerror(errno));
        goto cleanup;
    }
    if (fstat(metadata_fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || status.st_size > MAX_METADATA_SIZE) {
        log_error("container info is not a safe regular file: %s", container_name);
        goto cleanup;
    }
    data = malloc((size_t)status.st_size + 1U);
    if (data == NULL) {
        log_error("failed to allocate metadata buffer for %s", container_name);
        goto cleanup;
    }
    while (offset < (size_t)status.st_size) {
        ssize_t count = read(metadata_fd, data + offset,
                             (size_t)status.st_size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            log_error("short read from container info: %s", container_name);
            goto cleanup;
        }
        offset += (size_t)count;
    }
    if (td_parse_container_info(data, offset, container_name, info,
                                validation_error, sizeof(validation_error)) != 0) {
        log_error("corrupt container info %s: %s",
                  container_name, validation_error);
        goto cleanup;
    }
    result = 0;

cleanup:
    free(data);
    if (metadata_fd >= 0) {
        (void)close(metadata_fd);
    }
    if (directory_fd >= 0) {
        (void)close(directory_fd);
    }
    return result;
}

int update_container_status(char *container_name, enum container_status status) {
    struct container_info info;
    if (status < CONTAINER_RUNNING || status > CONTAINER_EXITED) {
        log_error("invalid container status: %d", status);
        return -1;
    }
    if (read_container_info(container_name, &info) == -1) {
        log_error("failed load container info for %s", container_name);
        return -1;
    }
    if (snprintf(info.status, sizeof(info.status), "%s", str_status[status]) < 0) {
        return -1;
    }
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

    uint64_t current_start_time = 0;
    if (read_process_start_time(info->pid, &current_start_time) == 0) {
        if (info->pid_start_time == 0U) {
            log_warn("container %s has legacy metadata without pid_start_time; "
                     "PID reuse cannot be ruled out", container_name);
            return 0;
        }
        if (current_start_time == info->pid_start_time) {
            return 0;
        }
        log_warn("container %s pid %d was reused (start time changed)",
                 container_name, info->pid);
    } else if (errno != ENOENT && errno != ESRCH) {
        log_warn("failed to verify container %s pid %d identity: %s",
                 container_name, info->pid, strerror(errno));
        return -1;
    }

    (void)snprintf(info->status, sizeof(info->status), "%s",
                   str_status[CONTAINER_EXITED]);
    if (write_container_info((char *) container_name, info) != 0) {
        log_error("failed to update container %s status to EXITED", container_name);
        return -1;
    }

    log_info("container %s pid %d is gone or reused; mark status as EXITED",
             container_name, info->pid);
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
        char validation_error[160] = {0};
        if (td_validate_name(entry->d_name, TINYDOCKER_MAX_CONTAINER_NAME,
                             validation_error, sizeof(validation_error)) != 0) {
            log_warn("skip unsafe container info entry: %s", validation_error);
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
        if (lstat(file_path, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
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
    char validation_error[160] = {0};
    int directory_fd;
    int result;

    if (td_validate_name(container_name, TINYDOCKER_MAX_CONTAINER_NAME,
                         validation_error, sizeof(validation_error)) != 0) {
        log_error("invalid container info name: %s", validation_error);
        return -1;
    }
    directory_fd = open(CONTAINER_STATUS_INFO_DIR,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    result = unlinkat(directory_fd, container_name, 0);
    if (result != 0 && errno == ENOENT) {
        result = 0;
    }
    (void)close(directory_fd);
    return result;
}
