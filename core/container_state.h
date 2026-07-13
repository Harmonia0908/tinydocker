#ifndef TINYDOCKER_CORE_CONTAINER_STATE_H
#define TINYDOCKER_CORE_CONTAINER_STATE_H

#include <stdint.h>

enum container_status {
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_EXITED
};

struct container_info {
    int pid;
    uint64_t pid_start_time;
    int detach;
    char container_id[128];
    char image[128];
    char command[512];
    char created[20];
    char status[10];
    char name[128];
    char ip_addr[16];
    int volume_cnt;
    char volumes[32][512];
};

#endif
