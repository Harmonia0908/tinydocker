#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "fs.h"
#include "safety.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int remove_entry(const char *path, const struct stat *status,
                        int type, struct FTW *walk)
{
    (void)status;
    (void)type;
    (void)walk;
    return remove(path);
}

int td_remove_tree(const char *path)
{
    struct stat status;

    if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0) {
        errno = EINVAL;
        return -1;
    }
    if (lstat(path, &status) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    return nftw(path, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
}

int td_ensure_directory_beneath(const char *rootfs, const char *container_path,
                                char *output, size_t output_size)
{
    char error[160] = {0};
    char *copy = NULL;
    char *save = NULL;
    char *component;
    int directory_fd = -1;
    int result = -1;

    if (td_join_rootfs_path(rootfs, container_path, output, output_size,
                            error, sizeof(error)) != 0) {
        errno = EINVAL;
        return -1;
    }
    directory_fd = open(rootfs, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        return -1;
    }
    copy = strdup(container_path + 1);
    if (copy == NULL) {
        goto cleanup;
    }
    component = strtok_r(copy, "/", &save);
    while (component != NULL) {
        int next_fd;

        if (mkdirat(directory_fd, component, S_IRWXU) != 0 && errno != EEXIST) {
            goto cleanup;
        }
        next_fd = openat(directory_fd, component,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next_fd < 0) {
            goto cleanup;
        }
        (void)close(directory_fd);
        directory_fd = next_fd;
        component = strtok_r(NULL, "/", &save);
    }
    result = 0;

cleanup:
    free(copy);
    if (directory_fd >= 0) {
        (void)close(directory_fd);
    }
    return result;
}
