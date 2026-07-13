#ifndef TINYDOCKER_CORE_FS_H
#define TINYDOCKER_CORE_FS_H

#include <stddef.h>

int td_remove_tree(const char *path);
int td_ensure_directory_beneath(const char *rootfs, const char *container_path,
                                char *output, size_t output_size);

#endif
