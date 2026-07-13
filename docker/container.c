#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include "../logger/log.h"
#include "../util/utils.h"
#include "cgroup.h"
#include "container.h"
#include "volumes.h"
#include "workspace.h"
#include "status_info.h"
#include "network.h"
#include "../core/cgroup_parse.h"
#include "../core/safety.h"
#include "../core/process.h"


extern char **environ;
static int pipe_fd[2];
static volatile sig_atomic_t foreground_child_pid = -1;

static void forward_signal_to_container(int signal_number) {
    pid_t child = (pid_t)foreground_child_pid;
    if (child > 0) {
        (void)kill(child, signal_number);
    }
}

int init_runtime_dirs(void) {
    const char *directories[] = {
        TINYDOCKER_RUNTIME_DIR,
        CONTAINER_STATUS_INFO_DIR,
        CONTAINER_LOG_DIR,
        TINYDOCKER_RUNTIME_DIR "/containers",
        TINYDOCKER_RUNTIME_DIR "/images",
        NULL
    };
    for (size_t index = 0U; directories[index] != NULL; index++) {
        if (!path_exist(directories[index]) && make_path(directories[index]) != 0) {
            log_error("failed to create runtime directory %s: %s",
                      directories[index], strerror(errno));
            return -1;
        }
    }
    return 0;
}

//初始化 run 所需目录和宿主机网络；只读命令不得调用。
int init_docker_env(void) {
    if (init_runtime_dirs() != 0) {
        return -1;
    }

    int r = create_default_bridge();
    if (r != 0) {
        log_error("failed to create detault net bridge");
        return -1;
    }

    //添加iptables支持外部响应可以返回到容器内部，不经过 shell。
    char *const check_rule[] = {
        "iptables", "-t", "nat", "-C", "POSTROUTING", "-s",
        TINYDOCKER_DEFAULT_NETWORK_CIDR, "-j", "MASQUERADE", NULL
    };
    if (td_run_command(check_rule) != 0) {
        char *const add_rule[] = {
            "iptables", "-t", "nat", "-A", "POSTROUTING", "-s",
            TINYDOCKER_DEFAULT_NETWORK_CIDR, "-j", "MASQUERADE", NULL
        };
        if (td_run_command(add_rule) != 0) {
            log_error("failed to ensure default MASQUERADE rule");
            return -1;
        }
    }
    return 0;
}

static int build_container_runtime_paths(const char *container_name,
                                         char *container_dir,
                                         size_t container_dir_size,
                                         char *mountpoint,
                                         size_t mountpoint_size,
                                         char *status_path,
                                         size_t status_path_size) {
    char error[160] = {0};
    int written;

    if (td_build_named_path(TINYDOCKER_RUNTIME_DIR, "containers", container_name,
                            container_dir, container_dir_size,
                            error, sizeof(error)) != 0 ||
        td_build_named_path(TINYDOCKER_RUNTIME_DIR, "container_info", container_name,
                            status_path, status_path_size,
                            error, sizeof(error)) != 0) {
        log_error("failed to build container runtime path: %s", error);
        return -1;
    }
    written = snprintf(mountpoint, mountpoint_size, "%s/mountpoint", container_dir);
    if (written < 0 || (size_t)written >= mountpoint_size) {
        log_error("container mountpoint path is too long: %s", container_name);
        return -1;
    }
    return 0;
}

static int container_exists(char *container_name) {
    char status_path[PATH_MAX] = {0};
    char container_dir[PATH_MAX] = {0};
    char mountpoint[PATH_MAX] = {0};
    if (build_container_runtime_paths(container_name, container_dir,
                                      sizeof(container_dir), mountpoint,
                                      sizeof(mountpoint), status_path,
                                      sizeof(status_path)) != 0) {
        return 1;
    }
    //容器cgroup
    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return 1;
    }
    // 检查容器是否已经存在, 已经存在就报错
    if (path_exist(cgroup_path) || path_exist(status_path) || path_exist(container_dir)) {
        return 1;
    }
    return 0;
}

static int clean_container_runtime(char *container_name,
                                   const struct container_info *info) {
    char status_path[PATH_MAX] = {0};
    char container_dir[PATH_MAX] = {0};
    char mountpoint[PATH_MAX] = {0};
    struct volume_config *volumes = NULL;
    int volume_count = info == NULL ? 0 : info->volume_cnt;
    int cleanup_failed = 0;
    int mount_cleanup_failed = 0;

    if (build_container_runtime_paths(container_name, container_dir,
                                      sizeof(container_dir), mountpoint,
                                      sizeof(mountpoint), status_path,
                                      sizeof(status_path)) != 0) {
        return -1;
    }
    if (volume_count > 0) {
        volumes = calloc((size_t)volume_count, sizeof(*volumes));
        if (volumes == NULL) {
            log_error("failed alloc volume config buffer");
            return -1;
        }
        for (int i = 0; i < volume_count; i++) {
            volumes[i] = parse_volume_config(info->volumes[i]);
            if (volumes[i].ro < 0) {
                log_error("corrupt volume metadata for container %s", container_name);
                free(volumes);
                return -1;
            }
        }
    }

    if (umount_volumes(mountpoint, volume_count, volumes) != 0) {
        cleanup_failed = 1;
        mount_cleanup_failed = 1;
    }
    free(volumes);
    if (umount(mountpoint) != 0 && errno != EINVAL && errno != ENOENT) {
        log_error("failed to unmount container root %s: %s",
                  mountpoint, strerror(errno));
        cleanup_failed = 1;
        mount_cleanup_failed = 1;
    }
    if (info != NULL && info->ip_addr[0] != '\0') {
        if (unset_container_port_map((char *)info->ip_addr) != 0 ||
            disconnect_container(container_name,
                                 TINYDOCKER_DEFAULT_NETWORK_NAME) != 0 ||
            release_used_ip(TINYDOCKER_DEFAULT_NETWORK_NAME,
                            (char *)info->ip_addr) != 0) {
            cleanup_failed = 1;
        }
    }
    if (mount_cleanup_failed != 0) {
        log_error("refusing to delete workspace while mounts may still be active: %s",
                  container_dir);
    } else if (remove_dir(container_dir) != 0) {
        log_error("failed to remove container workspace %s: %s",
                  container_dir, strerror(errno));
        cleanup_failed = 1;
    }
    if (remove_cgroup(container_name) != 0) {
        cleanup_failed = 1;
    }
    if (cleanup_failed == 0 && remove_status_info(container_name) != 0) {
        log_error("failed to remove container metadata %s: %s",
                  status_path, strerror(errno));
        cleanup_failed = 1;
    }
    return cleanup_failed == 0 ? 0 : -1;
}

static void cleanup_run_failure(struct docker_run_arguments *args, char *mountpoint, char *ip_addr, pid_t child_pid, int pipe_created) {
    int mount_cleanup_failed = 0;
    if (child_pid > 0) {
        pid_t waited;
        if (kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
            log_warn("failed to kill container child process %d: %s", child_pid, strerror(errno));
        }
        do {
            waited = waitpid(child_pid, NULL, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno != ECHILD) {
            log_warn("failed to wait container child process %d: %s", child_pid, strerror(errno));
        }
    }

    if (pipe_created) {
        if (close(pipe_fd[0]) != 0) {
            log_warn("failed to close run sync pipe read end: %s", strerror(errno));
        }
        if (close(pipe_fd[1]) != 0) {
            log_warn("failed to close run sync pipe write end: %s", strerror(errno));
        }
    }

    if (ip_addr != NULL && strlen(ip_addr) > 0) {
        (void)unset_container_port_map(ip_addr);
        if (disconnect_container(args->name, TINYDOCKER_DEFAULT_NETWORK_NAME) != 0) {
            log_warn("failed to disconnect container network: %s", args->name);
        }
        if (release_used_ip(TINYDOCKER_DEFAULT_NETWORK_NAME, ip_addr) != 0) {
            log_warn("failed to release container ip %s", ip_addr);
        }
    }

    if (mountpoint != NULL && strlen(mountpoint) > 0) {
        if (args->volume_cnt > 0 && args->volumes != NULL) {
            if (umount_volumes(mountpoint, args->volume_cnt, args->volumes) != 0) {
                log_warn("failed to unmount container volumes: %s", mountpoint);
                mount_cleanup_failed = 1;
            }
        }
        if (umount(mountpoint) != 0 && errno != EINVAL && errno != ENOENT) {
            log_warn("failed to unmount container mountpoint %s: %s", mountpoint, strerror(errno));
            mount_cleanup_failed = 1;
        }
    }

    char container_dir[PATH_MAX] = {0};
    char ignored_mountpoint[PATH_MAX] = {0};
    char ignored_status_path[PATH_MAX] = {0};
    if (build_container_runtime_paths(args->name, container_dir,
                                      sizeof(container_dir), ignored_mountpoint,
                                      sizeof(ignored_mountpoint),
                                      ignored_status_path,
                                      sizeof(ignored_status_path)) != 0) {
        log_warn("container dir path too long, skip cleanup: %s", args->name);
    } else if (mount_cleanup_failed != 0) {
        log_warn("refusing workspace deletion while mounts may still be active: %s",
                 container_dir);
    } else if (remove_dir(container_dir) != 0) {
        log_warn("failed to remove container workspace: %s", container_dir);
    }

    if (remove_cgroup(args->name) != 0) {
        log_warn("failed to remove cgroup: %s", args->name);
    }

    if (mount_cleanup_failed != 0) {
        log_warn("partial metadata kept because mount cleanup failed: %s",
                 args->name);
    } else if (remove_status_info(args->name) != 0) {
        log_warn("failed to remove partial container info: %s", args->name);
    }
}

static char **load_process_env(int pid, struct key_val_pair *user_envs, int user_env_cnt) {
    enum { MAX_ENV_COUNT = 256, MAX_ENV_BYTES = 1024 * 1024 };
    const int max_env_cnt = MAX_ENV_COUNT + 1;
    char **envs = calloc((size_t)max_env_cnt, sizeof(*envs));
    int env_idx = 0;
    if (envs == NULL || user_env_cnt < 0 ||
        (user_env_cnt > 0 && user_envs == NULL)) {
        free(envs);
        return NULL;
    }

    // 复制父进程的环境变量
    if (getpid() == pid) {  //获取当前进程自己的环境变量, 直接使用environ变量
        for (int i = 0; env_idx < MAX_ENV_COUNT && environ[i] != NULL; i++) {
            envs[env_idx++] = environ[i];
        }
    } else {
        char path[256] = {0};
        int path_length = snprintf(path, sizeof(path), "/proc/%d/environ", pid);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            free(envs);
            return NULL;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
            log_error("failed to open environ file: %s, err:%s", path, strerror(errno));
            free(envs);
            return NULL;
        }

        size_t capacity = 4096U;
        size_t used = 0U;
        char *contents = malloc(capacity);
        if (contents == NULL) {
            (void)close(fd);
            free(envs);
            return NULL;
        }
        for (;;) {
            if (used == capacity) {
                size_t next_capacity = capacity * 2U;
                char *larger;
                if (next_capacity > MAX_ENV_BYTES) {
                    log_error("environment for pid %d exceeds %d bytes",
                              pid, MAX_ENV_BYTES);
                    free(contents);
                    (void)close(fd);
                    free(envs);
                    return NULL;
                }
                larger = realloc(contents, next_capacity);
                if (larger == NULL) {
                    free(contents);
                    (void)close(fd);
                    free(envs);
                    return NULL;
                }
                contents = larger;
                capacity = next_capacity;
            }
            ssize_t count = read(fd, contents + used, capacity - used);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                free(contents);
                (void)close(fd);
                free(envs);
                return NULL;
            }
            if (count == 0) {
                break;
            }
            used += (size_t)count;
        }
        (void)close(fd);
        size_t start = 0U;
        while (start < used && env_idx < MAX_ENV_COUNT) {
            char *end = memchr(contents + start, '\0', used - start);
            size_t length = end == NULL ? used - start :
                (size_t)(end - (contents + start));
            if (length > 0U) {
                char *entry = malloc(length + 1U);
                if (entry == NULL) {
                    free(contents);
                    free(envs);
                    return NULL;
                }
                memcpy(entry, contents + start, length);
                entry[length] = '\0';
                envs[env_idx++] = entry;
            }
            start += length + (end == NULL ? 0U : 1U);
            if (end == NULL) {
                break;
            }
        }
        free(contents);
    }

    // 复制用户设置的环境变量
    for (int i = 0; env_idx < MAX_ENV_COUNT && i < user_env_cnt; i++) {
        char *key = user_envs[i].key;
        char *val = user_envs[i].val;
        if (key == NULL || val == NULL) {
            free(envs);
            return NULL;
        }
        size_t env_length = strlen(key) + strlen(val) + 2U;
        char *env_kv = malloc(env_length);
        if (env_kv == NULL) {
            free(envs);
            return NULL;
        }
        (void)snprintf(env_kv, env_length, "%s=%s", key, val);
        envs[env_idx++] = env_kv;
    }
    envs[env_idx] = NULL;

    return envs;
}

static char child_stack[8 * 1024 * 1024];

static int child_fn(void *args) {
    struct docker_run_arguments *run_args = (struct docker_run_arguments *) args;
    //如果容器是后台运行, 将日志日志重定向到指定目录
    if (run_args->detach) {
        char log_file_path[PATH_MAX] = {0};
        int path_length = snprintf(log_file_path, sizeof(log_file_path), "%s/%s",
                                   CONTAINER_LOG_DIR, run_args->name);
        if (path_length < 0 || (size_t)path_length >= sizeof(log_file_path)) {
            _exit(126);
        }
        log_info("container log file: %s", log_file_path);
        int fd = open(log_file_path,
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
        if (fd < 0) {
            log_error("failed to open container log file: %s", log_file_path);
            _exit(126);
        }
        if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            (void)close(fd);
            _exit(126);
        }
        (void)close(fd);
    }

    // 为什么放在这个地方, 因为如果pivot_root后如果拿指定进程的环境变量就会报找不到文件了, 因为进入了容器里面, 但这里其实无所谓, 因为是拿当前进程自己的
    char **envs = load_process_env(getpid(), run_args->env, run_args->env_cnt);
    if (envs == NULL) {
        _exit(126);
    }

    log_info("start init inner process");
    if (init_and_set_new_root(run_args->mountpoint) != 0) {
        log_error("init docker error");
        _exit(126);
    }

    //这里阻塞等收到父进程的命令后才开始运行, 为的是等待父进程设置cgroup
    close(pipe_fd[1]);
    char start_message = '\0';
    ssize_t read_count;
    do {
        read_count = read(pipe_fd[0], &start_message, 1U);
    } while (read_count < 0 && errno == EINTR);
    close(pipe_fd[0]);
    if (read_count != 1 || start_message != '1') {
        log_error("parent did not authorize container command start");
        _exit(125);
    }

    //开始运行用户命令
    char **cmds = (char **) run_args->container_argv;
    log_info("start to run %s", cmds[0]);
    execve(cmds[0], cmds, envs);
    perror("exec error");
    _exit(127);
}

int docker_run(struct docker_run_arguments *args) {
    int ret = 0;
    int pipe_created = 0;
    pid_t child_pid = -1;
    char ip_addr[20] = {0};
    char *mountpoint = NULL;

    if (container_exists(args->name)) {
        log_error("container %s has exists", args->name);
        return -1;
    }
    mountpoint = (char *) malloc(PATH_MAX);
    if (mountpoint == NULL) {
        log_error("failed to allocate mountpoint");
        return -1;
    }
    mountpoint[0] = '\0';
    if (init_container_workerspace(args, mountpoint) == -1) {
        log_error("failed to init_container_workerspace");
        goto fail_cleanup_run;
    }
    log_info("create overlay filesystem mountpoint: %s", mountpoint);
    args->mountpoint = mountpoint;

    if (mount_volumes(mountpoint, args->volume_cnt, args->volumes) == -1) {
        goto fail_cleanup_run;
    }

    ret = init_cgroup(args->name); //创建一个test容器
    if (ret != 0) {
        perror("failed to init cgroup");
        goto fail_cleanup_run;
    }

    // 注意这里cpu和mem如果设置的太小, 容器可能起不来, cpu最小要求1000, 也就是1%, mem测试能起来的最小值是204800
    if (set_cgroup_limits(args->name, args->cpu, args->memory, NULL) != 0) {
        log_error("failed to set_cgroup_limits for %s", args->name);
        goto fail_cleanup_run;
    }
    log_info("set_cgroup_limits cpu=%d, mem=%d", args->cpu, args->memory);

    if (pipe(pipe_fd) == -1) {
        goto fail_cleanup_run;
    }
    pipe_created = 1;

    // 这里不要加CLONE_NEWUSER, 否则会导致pivot_root权限不足, 
    child_pid = clone(child_fn, child_stack+(8 * 1024 * 1024), CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | SIGCHLD, args);
    if (child_pid == -1) {
        perror("clone subprocess error");
        goto fail_cleanup_run;
    }
    log_info("docker process pid=%d", child_pid);

    //应用cgroup限制
    if (apply_cgroup_limit_to_pid(args->name, child_pid) != 0) {
        log_error("failed apply_cgroup_limit_to_pid, container: %s, pid:%d", args->name, child_pid);
        goto fail_cleanup_run;
    }

    //为容器分配IP, 并将容器链接到默认的网桥
    if (connect_container(args->name, TINYDOCKER_DEFAULT_NETWORK_NAME, ip_addr) == -1) {
        log_warn("failed to connect %s to brige %s, container_pid: %s", args->name, TINYDOCKER_DEFAULT_NETWORK_NAME, child_pid);
        goto fail_cleanup_run;
    }

    //设置端口映射
    if (set_container_port_map(ip_addr, args->port_mapping_cnt,
                               args->port_mapping) != 0) {
        log_error("failed to configure port mappings for %s", args->name);
        goto fail_cleanup_run;
    }

    //记录容器信息
    time_t start_time = time(NULL);
    struct container_info info;
    if (create_container_info(args, child_pid, CONTAINER_RUNNING, ip_addr, start_time, &info) != 0) {
        log_error("failed to create container info for: %s", args->name);
        goto fail_cleanup_run;
    }
    if (write_container_info(args->name, &info) != 0) {
        log_error("failed to write container info for: %s", args->name);
        goto fail_cleanup_run;
    }

    //发送任意消息解除子进程的阻塞
    close(pipe_fd[0]);  //这里的关闭一定要放在创建子进程后面, 如果放在创建子进程前面, 由于继承关系,直接给子进程的读关闭了
    char start_message = '1';
    ssize_t write_count;
    do {
        write_count = write(pipe_fd[1], &start_message, 1U);
    } while (write_count < 0 && errno == EINTR);
    if (write_count != 1) {
        log_error("failed to release container child: %s", strerror(errno));
        (void)close(pipe_fd[1]);
        pipe_created = 0;
        goto fail_cleanup_run;
    }
    close(pipe_fd[1]);

    // 如果配置了it任一参数, 等待容器退出
    if (args->detach == 1) {
        log_info("leave container process running in background");
        free(mountpoint);
        return 0;
    }

    int exit_status = 0;
    const int forwarded_signals[] = {SIGINT, SIGTERM, SIGHUP};
    struct sigaction previous_actions[3];
    int action_installed[3] = {0};
    struct sigaction forward_action;
    memset(&forward_action, 0, sizeof(forward_action));
    forward_action.sa_handler = forward_signal_to_container;
    (void)sigemptyset(&forward_action.sa_mask);
    for (size_t index = 0U; index < 3U; index++) {
        if (sigaction(forwarded_signals[index], &forward_action,
                      &previous_actions[index]) != 0) {
            log_warn("failed to install signal forwarding for %d: %s",
                     forwarded_signals[index], strerror(errno));
        } else {
            action_installed[index] = 1;
        }
    }
    foreground_child_pid = child_pid;
    pid_t waited;
    do {
        waited = waitpid(child_pid, &exit_status, 0);
    } while (waited < 0 && errno == EINTR);
    foreground_child_pid = -1;
    for (size_t index = 0U; index < 3U; index++) {
        if (action_installed[index] != 0) {
            (void)sigaction(forwarded_signals[index], &previous_actions[index], NULL);
        }
    }
    if (waited < 0) {
        log_error("waitpid failed for container %s: %s",
                  args->name, strerror(errno));
        free(mountpoint);
        return -1;
    }

    if (WIFSIGNALED(exit_status)) { //容器进程被信号杀死
        printf("container was killed by signal %d\n", WTERMSIG(exit_status));
    } else { //正常退出
        log_info("container process exit");
    }
    if (update_container_status(args->name, CONTAINER_EXITED) != 0) {
        log_warn("failed to persist EXITED status for %s", args->name);
    }
    free(mountpoint);
    if (WIFEXITED(exit_status)) {
        return WEXITSTATUS(exit_status);
    }
    return 128 + WTERMSIG(exit_status);

fail_cleanup_run:
    cleanup_run_failure(args, mountpoint, ip_addr, child_pid, pipe_created);
    free(mountpoint);
    return -1;
}



int docker_commit(struct docker_commit_arguments *args) {
    // 检查容器是否存在
    char container_mountpoint[512] = {0};
    sprintf(container_mountpoint, "%s/containers/%s/mountpoint", TINYDOCKER_RUNTIME_DIR, args->container_name);
    if (path_exist(container_mountpoint) == 0) {
        log_error("container is not exists: %s", args->container_name);
        return -1;
    }
    
    char tar_path[512] ={0};
    if (args->tar_path == NULL) {
        sprintf(tar_path, "%s.tar", args->container_name);
    }

    if (create_tar(container_mountpoint, args->tar_path != NULL ? args->tar_path : tar_path) != 0) {
        log_error("failed to commit container: %s", args->container_name);
        return -1;
    }

    return 0;
}


int docker_ps(struct docker_ps_arguments *args) {
    size_t info_list_capacity = 128;
    struct container_info *info_list = (struct container_info *) malloc(sizeof(struct container_info) * info_list_capacity);
    if (info_list == NULL) {
        log_error("failed to alloc container info list");
        return -1;
    }
    int cnt = list_containers_info(info_list, info_list_capacity);
    if (cnt < 0) {
        free(info_list);
        return -1;
    }
    char *titles[] = {"CONTAINER_ID", "IMAGE", "COMMAND", "CREATED", "STATUS", "NAMES", NULL};
    int spans[6] =   {12,              5,       7,         7,         6,        5}; //记录每个字段输出的最大宽度, 与titles一一对应
    for (int i = 0; i < cnt; i++) {
        if (args->list_all == 0 && strcmp(info_list[i].status, "RUNNING") != 0) {
            continue;
        }
        size_t container_id_len = strlen(info_list[i].container_id);
        spans[0] = container_id_len > (size_t)spans[0] ? (int)container_id_len : spans[0];

        size_t image_len = strlen(info_list[i].image);
        spans[1] = image_len > (size_t)spans[1] ? (int)image_len : spans[1];

        size_t command_len = strlen(info_list[i].command);
        spans[2] = command_len > (size_t)spans[2] ? (int)command_len : spans[2];

        spans[3] = 19; //2023-12-12 12:12:12这样的形式, 固定19长度
        spans[4] = 7; //RUNING|STOPPING|EXITED, 最长7

        size_t name_len = strlen(info_list[i].name);
        spans[5] = name_len > (size_t)spans[5] ? (int)name_len : spans[5];
    }

    for (int i = 0; titles[i] != NULL; i++) {
        printf("%-*s\t", spans[i], titles[i]);
    }
    printf("\n");
    for (int i = 0; i < cnt; i++) {
        if (args->list_all == 0 && strcmp(info_list[i].status, "RUNNING") != 0) {
            continue;
        }
        printf("%-*s\t%-*s\t%-*s\t%-*s\t%-*s\t%-*s\n", spans[0], info_list[i].container_id, spans[1], info_list[i].image, \
         spans[2], info_list[i].command, spans[3], info_list[i].created, spans[4], info_list[i].status, spans[5], info_list[i].name);
    }
    free(info_list);
    return 0;
}

int docker_top(struct docker_top_arguments *args) {
    int pid_list[4096];
    int pid_cnt = get_container_processes_id(args->container_name, pid_list,
                                             sizeof(pid_list) / sizeof(pid_list[0]));
    if (pid_cnt == -1) {
        log_error("failed to get container process list");
        return -1;
    }

    if (pid_cnt == 0) {
        log_warn("container %s has no processes", args->container_name);
        return 0;
    }

    size_t pid_buffer_size = (size_t)pid_cnt * 32U + 1U;
    char *pid_str_list = calloc(pid_buffer_size, 1U);
    if (pid_str_list == NULL) {
        return -1;
    }
    size_t used = 0U;
    for (int i = 0; i < pid_cnt; i++) {
        int written = snprintf(pid_str_list + used, pid_buffer_size - used,
                               "%s%d", i == 0 ? "" : ",", pid_list[i]);
        if (written < 0 || (size_t)written >= pid_buffer_size - used) {
            free(pid_str_list);
            return -1;
        }
        used += (size_t)written;
    }

    char *const command[] = {"ps", "-f", "-p", pid_str_list, NULL};
    log_info("run ps without a shell for container %s", args->container_name);
    int ret = td_run_command(command);

    free(pid_str_list);
    return ret;
}


int docker_exec(struct docker_exec_arguments *args) {
    struct container_info info;
    if (read_container_info(args->container_name, &info) != 0 ||
        refresh_container_status_if_needed(args->container_name, &info) < 0 ||
        strcmp(info.status, "RUNNING") != 0) {
        log_error("container %s is not running", args->container_name);
        return -1;
    }

    int pid_list[4096];
    int pid_cnt = get_container_processes_id(args->container_name, pid_list,
                                             sizeof(pid_list) / sizeof(pid_list[0]));
    if (pid_cnt <= 0) {
        log_error("failed to get container process list");
        return -1;
    }
    int target_is_member = 0;
    for (int index = 0; index < pid_cnt; index++) {
        if (pid_list[index] == info.pid) {
            target_is_member = 1;
            break;
        }
    }
    if (target_is_member == 0) {
        log_error("container init pid %d is not in its cgroup", info.pid);
        return -1;
    }

    char **envs = load_process_env(info.pid, args->env, args->env_cnt);
    if (envs == NULL) {
        return -1;
    }

    pid_t namespace_worker = fork();
    if (namespace_worker < 0) {
        log_error("failed to fork namespace worker: %s", strerror(errno));
        return -1;
    }
    if (namespace_worker == 0) {
        const char *namespace_types[] = {"ipc", "uts", "net", "mnt", "pid", NULL};
        int namespace_fds[5] = {-1, -1, -1, -1, -1};

        if (apply_cgroup_limit_to_pid(args->container_name, getpid()) != 0) {
            _exit(125);
        }
        for (size_t index = 0U; index < 5U; index++) {
            char namespace_path[128] = {0};
            int path_length = snprintf(namespace_path, sizeof(namespace_path),
                                       "/proc/%d/ns/%s", info.pid,
                                       namespace_types[index]);
            if (path_length < 0 || (size_t)path_length >= sizeof(namespace_path)) {
                for (size_t opened = 0U; opened < index; opened++) {
                    (void)close(namespace_fds[opened]);
                }
                _exit(125);
            }
            namespace_fds[index] = open(namespace_path, O_RDONLY | O_CLOEXEC);
            if (namespace_fds[index] < 0) {
                for (size_t opened = 0U; opened < index; opened++) {
                    (void)close(namespace_fds[opened]);
                }
                _exit(125);
            }
        }
        for (size_t index = 0U; index < 5U; index++) {
            int setns_result = setns(namespace_fds[index], 0);
            int saved_errno = errno;
            (void)close(namespace_fds[index]);
            if (setns_result != 0) {
                for (size_t remaining = index + 1U; remaining < 5U; remaining++) {
                    (void)close(namespace_fds[remaining]);
                }
                log_error("failed to join %s namespace: %s",
                          namespace_types[index], strerror(saved_errno));
                _exit(125);
            }
        }
        if (chdir("/") != 0) {
            _exit(125);
        }

        pid_t command_pid = fork();
        if (command_pid < 0) {
            _exit(125);
        }
        if (command_pid == 0) {
            execve(args->container_argv[0], args->container_argv, envs);
            _exit(127);
        }
        if (args->detach != 0) {
            _exit(0);
        }
        int command_status = 0;
        pid_t waited;
        do {
            waited = waitpid(command_pid, &command_status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            _exit(125);
        }
        if (WIFEXITED(command_status)) {
            _exit(WEXITSTATUS(command_status));
        }
        _exit(128 + WTERMSIG(command_status));
    }

    int worker_status = 0;
    pid_t waited;
    do {
        waited = waitpid(namespace_worker, &worker_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        log_error("failed to wait namespace worker: %s", strerror(errno));
        return -1;
    }
    if (WIFEXITED(worker_status)) {
        int exit_code = WEXITSTATUS(worker_status);
        if (exit_code != 0) {
            log_error("exec command failed with exit code %d", exit_code);
        }
        return exit_code;
    }
    return 128 + WTERMSIG(worker_status);
}

static int container_process_count(const char *container_name,
                                   int *pid_list, size_t capacity) {
    char cgroup_path[PATH_MAX] = {0};
    char procs_path[PATH_MAX] = {0};
    int written;

    if (get_container_cgroup_path(container_name, cgroup_path,
                                  sizeof(cgroup_path)) != 0) {
        return -1;
    }
    written = snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs",
                       cgroup_path);
    if (written < 0 || (size_t)written >= sizeof(procs_path)) {
        return -1;
    }
    if (!path_exist(procs_path)) {
        return 0;
    }
    return get_container_processes_id(container_name, pid_list, capacity);
}

static int signal_container_processes(const char *container_name,
                                      int signal_number) {
    int pid_list[1024];
    int pid_count = container_process_count(container_name, pid_list,
                                            sizeof(pid_list) / sizeof(pid_list[0]));
    int result = 0;

    if (pid_count < 0) {
        return -1;
    }
    for (int index = 0; index < pid_count; index++) {
        if (pid_list[index] <= 0) {
            result = -1;
            continue;
        }
        if (kill(pid_list[index], signal_number) != 0 && errno != ESRCH) {
            log_error("failed to signal pid %d in container %s: %s",
                      pid_list[index], container_name, strerror(errno));
            result = -1;
        }
    }
    return result;
}

int docker_stop(struct docker_stop_arguments *args) {
    int overall_result = 0;
    const struct timespec poll_interval = {.tv_sec = 0, .tv_nsec = 100000000L};

    for (int index = 0; index < args->container_cnt; index++) {
        char *container_name = args->container_names[index];
        struct container_info info;
        if (read_container_info(container_name, &info) != 0) {
            overall_result = -1;
            continue;
        }
        if (refresh_container_status_if_needed(container_name, &info) < 0) {
            overall_result = -1;
            continue;
        }
        if (strcmp(info.status, "RUNNING") != 0) {
            log_info("container %s is already %s", container_name, info.status);
            continue;
        }
        if (signal_container_processes(container_name, SIGTERM) != 0) {
            overall_result = -1;
            continue;
        }

        int pid_list[1024];
        int pid_count = container_process_count(container_name, pid_list,
                                                sizeof(pid_list) / sizeof(pid_list[0]));
        for (int poll = 0; pid_count > 0 && poll < args->time * 10; poll++) {
            (void)nanosleep(&poll_interval, NULL);
            pid_count = container_process_count(container_name, pid_list,
                                                sizeof(pid_list) / sizeof(pid_list[0]));
        }
        if (pid_count > 0) {
            if (signal_container_processes(container_name, SIGKILL) != 0) {
                overall_result = -1;
                continue;
            }
            for (int poll = 0; pid_count > 0 && poll < 20; poll++) {
                (void)nanosleep(&poll_interval, NULL);
                pid_count = container_process_count(container_name, pid_list,
                                                    sizeof(pid_list) / sizeof(pid_list[0]));
            }
        }
        if (pid_count != 0) {
            log_error("container %s still has processes; status not changed",
                      container_name);
            overall_result = -1;
            continue;
        }
        if (update_container_status(container_name, CONTAINER_STOPPED) != 0) {
            overall_result = -1;
        }
    }
    return overall_result;
}


int docker_rm(struct docker_rm_arguments *args) {
    int overall_result = 0;
    for (int i = 0; i < args->container_cnt; i++) {
        char *container_name = args->containers[i];
        if (!container_exists(container_name)) {
            log_info("container %s is already absent", container_name);
            continue;
        }

        struct container_info info;
        if (read_container_info(container_name, &info) == -1) {
            log_error("refusing to remove %s because its metadata is missing or corrupt",
                      container_name);
            overall_result = -1;
            continue;
        }
        if (refresh_container_status_if_needed(container_name, &info) < 0) {
            overall_result = -1;
            continue;
        }

        if (strcmp(info.status, "RUNNING") == 0) {
            log_warn("container %s running, ignore remove", container_name);
            overall_result = -1;
            continue;
        }

        if (clean_container_runtime(container_name, &info) != 0) {
            log_error("container %s cleanup is incomplete; metadata kept for retry",
                      container_name);
            overall_result = -1;
        } else {
            log_info("finished cleanup for container: %s", container_name);
        }
    }
    return overall_result;
}

int docker_inspect(struct docker_inspect_arguments *args) {
    struct container_info info;
    memset(&info, 0, sizeof(info));
    if (read_container_info(args->container_name, &info) == -1) {
        fprintf(stderr, "container not found: %s\n", args->container_name);
        return -1;
    }
    if (refresh_container_status_if_needed(args->container_name, &info) < 0) {
        fprintf(stderr, "warning: failed to refresh container status: %s\n", args->container_name);
    }

    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(args->container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return -1;
    }

    printf("Name: %s\n", info.name);
    printf("PID: %d\n", info.pid);
    printf("Status: %s\n", info.status);
    printf("Image: %s\n", info.image);
    printf("Command: %s\n", info.command);
    printf("Created: %s\n", info.created);
    printf("IP: %s\n", strlen(info.ip_addr) > 0 ? info.ip_addr : "none");
    printf("CgroupPath: %s\n", cgroup_path);
    printf("CgroupAvailable: %s\n", path_exist(cgroup_path) ? "yes" : "no");

    char container_dir[PATH_MAX] = {0};
    char rootfs_path[PATH_MAX] = {0};
    char status_path[PATH_MAX] = {0};
    if (build_container_runtime_paths(args->container_name, container_dir,
                                      sizeof(container_dir), rootfs_path,
                                      sizeof(rootfs_path), status_path,
                                      sizeof(status_path)) == 0) {
        printf("RootfsPath: %s\n", rootfs_path);
        printf("RootfsAvailable: %s\n", path_exist(rootfs_path) ? "yes" : "no");
    }

    if (info.volume_cnt > 0) {
        printf("Volumes:\n");
        for (int i = 0; i < info.volume_cnt; i++) {
            printf("  %s\n", info.volumes[i]);
        }
    } else {
        printf("Volumes: none\n");
    }

    printf("PortMappings: unavailable\n");
    return 0;
}

static int read_cgroup_value(char *cgroup_path, char *file_name, char *buf, int buf_size) {
    char path[1024] = {0};
    int path_len = snprintf(path, sizeof(path), "%s/%s", cgroup_path, file_name);
    if (path_len < 0 || (size_t) path_len >= sizeof(path)) {
        snprintf(buf, buf_size, "N/A");
        return -1;
    }
    if (!path_exist(path)) {
        snprintf(buf, buf_size, "N/A");
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        snprintf(buf, buf_size, "N/A");
        return -1;
    }

    if (fgets(buf, buf_size, file) == NULL) {
        snprintf(buf, buf_size, "N/A");
        fclose(file);
        return -1;
    }

    buf[strcspn(buf, "\n")] = '\0';
    fclose(file);
    return 0;
}

static void read_cpu_usage_usec(char *cgroup_path, char *buf, int buf_size) {
    char path[1024] = {0};
    int path_len = snprintf(path, sizeof(path), "%s/%s", cgroup_path, "cpu.stat");
    if (path_len < 0 || (size_t) path_len >= sizeof(path)) {
        snprintf(buf, buf_size, "N/A");
        return;
    }
    if (!path_exist(path)) {
        snprintf(buf, buf_size, "N/A");
        return;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        snprintf(buf, buf_size, "N/A");
        return;
    }

    char contents[4096] = {0};
    char error[160] = {0};
    snprintf(buf, buf_size, "N/A");
    (void)fread(contents, 1, sizeof(contents) - 1U, file);
    if (ferror(file) == 0 &&
        td_parse_cgroup_stat(contents, "usage_usec", buf, (size_t)buf_size,
                             error, sizeof(error)) != 0) {
        log_warn("failed to parse %s: %s", path, error);
    }

    fclose(file);
}

static void format_bytes(char *raw, char *buf, int buf_size) {
    char error[160] = {0};
    if (td_format_bytes(raw, buf, (size_t)buf_size,
                        error, sizeof(error)) != 0) {
        log_warn("failed to format cgroup byte value '%s': %s", raw, error);
        (void)snprintf(buf, (size_t)buf_size, "N/A");
    }
}

int docker_stats(struct docker_stats_arguments *args) {
    struct container_info info;
    memset(&info, 0, sizeof(info));
    if (read_container_info(args->container_name, &info) == -1) {
        fprintf(stderr, "container not found: %s\n", args->container_name);
        return -1;
    }
    if (refresh_container_status_if_needed(args->container_name, &info) < 0) {
        fprintf(stderr, "warning: failed to refresh container status: %s\n", args->container_name);
    }

    char cgroup_path[1024] = {0};
    if (get_container_cgroup_path(args->container_name, cgroup_path, sizeof(cgroup_path)) != 0) {
        return -1;
    }
    if (!path_exist(cgroup_path)) {
        fprintf(stderr, "warning: cgroup is unavailable for %s; metrics are N/A\n",
                args->container_name);
    }

    char cpu_usec[128] = {0};
    char memory_current[128] = {0};
    char memory_max[128] = {0};
    char memory_current_text[128] = {0};
    char memory_max_text[128] = {0};
    char cpu_max[128] = {0};
    char pids_current[128] = {0};
    char pids_max[128] = {0};
    char pids[256] = {0};

    read_cpu_usage_usec(cgroup_path, cpu_usec, sizeof(cpu_usec));
    read_cgroup_value(cgroup_path, "memory.current", memory_current, sizeof(memory_current));
    read_cgroup_value(cgroup_path, "memory.max", memory_max, sizeof(memory_max));
    read_cgroup_value(cgroup_path, "cpu.max", cpu_max, sizeof(cpu_max));
    read_cgroup_value(cgroup_path, "pids.current", pids_current, sizeof(pids_current));
    read_cgroup_value(cgroup_path, "pids.max", pids_max, sizeof(pids_max));

    format_bytes(memory_current, memory_current_text, sizeof(memory_current_text));
    format_bytes(memory_max, memory_max_text, sizeof(memory_max_text));
    snprintf(pids, sizeof(pids), "%s/%s", pids_current, pids_max);

    printf("%-12s%-15s%-17s%-13s%-12s%-20s\n", "NAME", "CPU_USEC", "MEM_CURRENT", "MEM_MAX", "PIDS", "CPU_MAX");
    printf("%-12s%-15s%-17s%-13s%-12s%-20s\n", info.name, cpu_usec, memory_current_text, memory_max_text, pids, cpu_max);
    return 0;
}
