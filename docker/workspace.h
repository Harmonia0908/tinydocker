#ifndef WORKERSPACE_H
#define WORKERSPACE_H

#include "../cmdparser/cmdparser.h"


//设置容器新的根目录
int init_and_set_new_root(const char *new_root);

// 初始化容器工作目录
int init_container_workerspace(struct docker_run_arguments *args, char *mountpoint);


#endif
