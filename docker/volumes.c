#include <sys/mount.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include "../util/utils.h"
#include "../logger/log.h"
#include "../core/fs.h"
#include "../core/safety.h"
#include "volumes.h"

int mount_volumes(char *mountpoint, int vol_cnt, struct volume_config *vol_config) {
    if (vol_cnt < 0 || (vol_cnt > 0 && vol_config == NULL)) {
        log_error("invalid volume configuration");
        return -1;
    }
    // 先检查挂载的源目录是否都存在
    for (int i = 0; i < vol_cnt; i++) {
        char resolved_host[PATH_MAX] = {0};
        struct stat status;
        if (vol_config[i].host[0] != '/' ||
            realpath(vol_config[i].host, resolved_host) == NULL ||
            stat(resolved_host, &status) != 0 || !S_ISDIR(status.st_mode)) {
            log_error("volume source must be an existing absolute directory: %s",
                      vol_config[i].host);
            return -1;
        }
    }

    // 按顺序挂载目录到容器工作目录
    char container_dir[1024] = {0};
    for (int i = 0; i < vol_cnt; i++) {
        struct volume_config vol = vol_config[i];
        char resolved_host[PATH_MAX] = {0};
        if (realpath(vol.host, resolved_host) == NULL) {
            log_error("failed to resolve volume source %s: %s",
                      vol.host, strerror(errno));
            goto rollback;
        }
        if (td_ensure_directory_beneath(mountpoint, vol.container,
                                        container_dir,
                                        sizeof(container_dir)) != 0) {
            log_error("failed to create volume mount dir for %s", vol.container);
            goto rollback;
        }

        // 挂载目录
        if (mount(resolved_host, container_dir, NULL, MS_BIND, NULL) == -1) {
            log_error("failed to mount volume from %s to %s", vol.host, vol.container);
            goto rollback;
        }

        // 设置只读挂载
        if (vol.ro == 1) {
            if (mount(container_dir, container_dir, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == -1) {
                log_error("failed to set mount volume %s as readonly", vol.container);
                (void)umount(container_dir);
                goto rollback;
            }
        }
        continue;

rollback:
        for (int mounted = i - 1; mounted >= 0; mounted--) {
            char mounted_path[1024] = {0};
            char error[160] = {0};
            if (td_join_rootfs_path(mountpoint, vol_config[mounted].container,
                                    mounted_path, sizeof(mounted_path),
                                    error, sizeof(error)) == 0) {
                (void)umount(mounted_path);
            }
        }
        return -1;
    }
    return 0;
}


int umount_volumes(char *mountpoint, int vol_cnt, struct volume_config *vol_config) {
    // 按顺序卸载挂载到容器中的目录
    char container_dir[1024] = {0};
    int result = 0;
    for (int i = vol_cnt - 1; i >= 0; i--) {
        struct volume_config vol = vol_config[i];
        memset(container_dir, 0, 1024 * sizeof(char));
        char error[160] = {0};
        if (td_join_rootfs_path(mountpoint, vol.container, container_dir,
                                sizeof(container_dir), error,
                                sizeof(error)) != 0) {
            log_error("refusing unsafe volume cleanup path: %s", error);
            result = -1;
            continue;
        }
        // 卸载挂载点
        if (umount(container_dir) == -1 && errno != EINVAL && errno != ENOENT) {
            log_error("failed to unmount volume %s: %s",
                      container_dir, strerror(errno));
            result = -1;
        }
        log_info("umount volume %s", container_dir);
    }
    return result;
}
