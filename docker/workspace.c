#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <limits.h>
#include <stdio.h>
#include "container.h"
#include "../logger/log.h"
#include "../util/utils.h"
#include "cgroup.h"
#include "container.h"
#include "volumes.h"
#include "workspace.h"
#include "../core/safety.h"
#include "../core/fs.h"


//设置容器新的根目录
int init_and_set_new_root(const char *new_root) {
    char normalized_root[PATH_MAX] = {0};
    size_t root_length;

    if (new_root == NULL || realpath(new_root, normalized_root) == NULL) {
        log_error("invalid new root %s: %s",
                  new_root == NULL ? "(null)" : new_root, strerror(errno));
        return -1;
    }
    root_length = strlen(normalized_root);
    if (root_length == 0U || normalized_root[0] != '/') {
        log_error("new root must be an absolute path");
        return -1;
    }
    /* Ensure that 'new_root' and its parent mount don't have shared propagation (which would cause pivot_root() to
       return an error), and prevent propagation of mount events to the initial mount namespace. 
       保证new_root与它的父挂载点没有共享传播属性, 否则调用pivot_root会报错
       */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        log_error("set / mount point as private error: %s", strerror(errno));
        return -1;
    }

    /* 保证'new_root'是一个独立挂载点, 如果有挂载卷在new_root, 这里务必加上MS_REC参数递归挂载挂载的卷目录, 否则容器里面看不到内容*/
    if (mount(normalized_root, normalized_root, NULL, MS_REC | MS_BIND, NULL) == -1) {
        log_error("mount %s as new point error: %s", normalized_root, strerror(errno));
        return -1;
    }
    log_info("finish --bind mount '%s'", normalized_root);

    char old_root[PATH_MAX] = {0};
    int old_root_length = snprintf(old_root, sizeof(old_root), "%s/old_root",
                                   normalized_root);
    if (old_root_length < 0 || (size_t)old_root_length >= sizeof(old_root)) {
        log_error("old root path is too long");
        return -1;
    }
    char verified_old_root[PATH_MAX] = {0};
    if (td_ensure_directory_beneath(normalized_root, "/old_root",
                                    verified_old_root,
                                    sizeof(verified_old_root)) != 0 ||
        strcmp(verified_old_root, old_root) != 0) {
        log_error("failed to safely create old_root: %s", old_root);
        return -1;
    }

    log_info("set new root as: %s, old_root save to: %s", normalized_root, old_root);

    if (syscall(SYS_pivot_root, normalized_root, old_root) != 0) {
        log_error("SYS_pivot_root new: %s, old: %s error: %s", normalized_root, old_root, strerror(errno));
        return -1;
    }

    if (chdir("/") != 0) {
        log_error("failed to chdir after pivot_root: %s", strerror(errno));
        return -1;
    }

    // 卸载老的root
    char old_new[100] = "/old_root";
    int ret = umount2(old_new, MNT_DETACH);
    if (ret != 0) {
        log_error("failed to umount old_root: %s", old_root);
        return ret;
    }

    if (remove_dir(old_new) == -1) {
        log_info("failed to remove old root dir");
        return -1;
    }

    //检查proc目录挂载进程信息
    struct stat proc_status;
    if (lstat("/proc", &proc_status) != 0) {
        if (errno != ENOENT || mkdir("/proc", 0555) != 0) {
            log_error("failed to create /proc mountpoint: %s", strerror(errno));
            return -1;
        }
    } else if (!S_ISDIR(proc_status.st_mode)) {
        log_error("refusing non-directory /proc mountpoint in rootfs");
        return -1;
    }

    if (mount("proc", "/proc", "proc", MS_NOEXEC|MS_NOSUID|MS_NODEV, NULL) == -1) {
        perror("mount proc error");
        return -1;
    }
    return 0;
}


//创建只读层, 即镜像目录
static int create_readonly_layer(const char *image, char *readonly_dir,
                                 size_t readonly_size) {
    // 传入的镜像就是一个目录, 直接使用
    log_info("create_readonly_layer %s", image);
    if (is_folder(image)) {
        return realpath(image, readonly_dir) == NULL ? -1 : 0;
    }

    //假定传入的都是tar文件, 解压到指定的目录
    char *image_hash = calculate_sha256(image);
    if (image_hash == NULL) {
        return -1;
    }
    log_info("imageg hash:%s", image_hash);
    int path_length = snprintf(readonly_dir, readonly_size, "%s/images/%s",
                               TINYDOCKER_RUNTIME_DIR, image_hash);
    free(image_hash);
    if (path_length < 0 || (size_t)path_length >= readonly_size) {
        return -1;
    }

    //如果传入的tar包已经解压到镜像目录, 直接复用
    if (path_exist(readonly_dir)) {
        return 0;
    }

    char temporary_dir[PATH_MAX] = {0};
    int temporary_length = snprintf(temporary_dir, sizeof(temporary_dir),
                                    "%s.tmp.%ld", readonly_dir, (long)getpid());
    if (temporary_length < 0 ||
        (size_t)temporary_length >= sizeof(temporary_dir)) {
        return -1;
    }
    if (remove_dir(temporary_dir) != 0 || make_path(temporary_dir) != 0) {
        log_error("failed to prepare temporary image directory %s",
                  temporary_dir);
        return -1;
    }
    if (extract_tar(image, temporary_dir) != 0) {
        (void)remove_dir(temporary_dir);
        return -1;
    }
    if (rename(temporary_dir, readonly_dir) != 0) {
        if (errno == EEXIST && path_exist(readonly_dir)) {
            (void)remove_dir(temporary_dir);
            return 0;
        }
        log_error("failed to publish image directory %s: %s",
                  readonly_dir, strerror(errno));
        (void)remove_dir(temporary_dir);
        return -1;
    }
    return 0;
}

//创建读写层, 即overlayfs的upperdir
static int create_readwrite_layer(const char *container_name, char *readwrite_dir,
                                  size_t readwrite_size) {
    char container_dir[PATH_MAX] = {0};
    char error[160] = {0};
    int written;
    if (td_build_named_path(TINYDOCKER_RUNTIME_DIR, "containers", container_name,
                            container_dir, sizeof(container_dir),
                            error, sizeof(error)) != 0) {
        log_error("failed to build read-write layer path: %s", error);
        return -1;
    }
    written = snprintf(readwrite_dir, readwrite_size, "%s/upperdir", container_dir);
    if (written < 0 || (size_t)written >= readwrite_size) {
        log_error("failed to build read-write layer path: %s", error);
        return -1;
    }
    
    int t = make_path(readwrite_dir);
    log_info("create_readwrite_layer %s", readwrite_dir);
    return t;
}

//创建挂载点, 即容器实际的工作目录
static int create_workspace_mount_point(const char *container_name,
                                        const char *lowerdir,
                                        const char *upperdir,
                                        const char *mountpoint) {
    char container_dir[PATH_MAX] = {0};
    char workdir[PATH_MAX] = {0};
    char error[160] = {0};
    int written;
    if (td_build_named_path(TINYDOCKER_RUNTIME_DIR, "containers", container_name,
                            container_dir, sizeof(container_dir), error,
                            sizeof(error)) != 0) {
        log_error("failed to build overlay workdir: %s", error);
        return -1;
    }
    written = snprintf(workdir, sizeof(workdir), "%s/workdir", container_dir);
    if (written < 0 || (size_t)written >= sizeof(workdir)) {
        log_error("failed to build overlay workdir: %s", error);
        return -1;
    }
    if (make_path(workdir) != 0) {
        return -1;
    }
    
    //mount -t overlay overlay -o lowerdir=lower,upperdir=upper,workdir=worker_dir overlay_dir/
    size_t data_size = strlen(lowerdir) + strlen(upperdir) + strlen(workdir) + 40U;
    char *data = malloc(data_size);
    if (data == NULL) {
        return -1;
    }
    written = snprintf(data, data_size, "lowerdir=%s,upperdir=%s,workdir=%s",
                       lowerdir, upperdir, workdir);
    if (written < 0 || (size_t)written >= data_size) {
        free(data);
        return -1;
    }
    int result = mount("overlay", mountpoint, "overlay", MS_REC, data);
    free(data);
    return result;
}


int init_container_workerspace(struct docker_run_arguments *args, char *mountpoint) {
    // 如果是文件夹, 直接用这个文件作为只读层, 否则检查镜像是不是tar包, 然后解压
    char *readonly_dir = calloc(PATH_MAX, 1U);
    char *readwrite_dir = calloc(PATH_MAX, 1U);
    char container_dir[PATH_MAX] = {0};
    char error[160] = {0};
    int result = -1;
    if (readonly_dir == NULL || readwrite_dir == NULL) {
        goto cleanup;
    }
    if (create_readonly_layer(args->image, readonly_dir, PATH_MAX) != 0) {
        goto cleanup;
    }
    log_info("readonly_dir=%s", readonly_dir);

    if (create_readwrite_layer(args->name, readwrite_dir, PATH_MAX) != 0) {
        goto cleanup;
    }
    log_info("readwrite_dir=%s", readwrite_dir);

    if (td_build_named_path(TINYDOCKER_RUNTIME_DIR, "containers", args->name,
                            container_dir, sizeof(container_dir), error,
                            sizeof(error)) != 0) {
        log_error("failed to build mountpoint path: %s", error);
        goto cleanup;
    }
    int mountpoint_length = snprintf(mountpoint, PATH_MAX, "%s/mountpoint",
                                     container_dir);
    if (mountpoint_length < 0 || mountpoint_length >= PATH_MAX) {
        log_error("failed to build mountpoint path: %s", error);
        goto cleanup;
    }
    if (make_path(mountpoint) != 0) {
        goto cleanup;
    }
    log_info("mountpoint=%s", mountpoint);

    result = create_workspace_mount_point(args->name, readonly_dir,
                                          readwrite_dir, mountpoint);

cleanup:
    free(readonly_dir);
    free(readwrite_dir);
    return result;
}
