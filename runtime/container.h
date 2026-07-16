#ifndef TINYDOCKER_RUNTIME_CONTAINER_H
#define TINYDOCKER_RUNTIME_CONTAINER_H

#include "../docker/cgroup.h"

struct key_val_pair;
struct port_map;
struct volume_config;

enum container_stage {
    CONTAINER_STAGE_CONFIG_PREPARE,
    CONTAINER_STAGE_EXISTENCE_CHECK,
    CONTAINER_STAGE_FILESYSTEM_PREPARE,
    CONTAINER_STAGE_VOLUME_MOUNT,
    CONTAINER_STAGE_CGROUP_PREPARE,
    CONTAINER_STAGE_SYNC_PIPE_CREATE,
    CONTAINER_STAGE_PROCESS_CREATE,
    CONTAINER_STAGE_CGROUP_APPLY,
    CONTAINER_STAGE_NETWORK_PREPARE,
    CONTAINER_STAGE_PORT_MAPPING,
    CONTAINER_STAGE_METADATA_WRITE,
    CONTAINER_STAGE_CHILD_RELEASE,
    CONTAINER_STAGE_PROCESS_WAIT,
    CONTAINER_STAGE_CLEANUP,
    CONTAINER_STAGE_COMPLETE
};

/*
 * A non-owning view of the validated run arguments. The command adapter owns
 * every referenced string and array for the duration of docker_run().
 */
struct container_config {
    const char *name;
    const char *image;
    int interactive;
    int detach;
    struct cgroup_config cgroup;
    int volume_count;
    const struct volume_config *volumes;
    int environment_count;
    const struct key_val_pair *environment;
    int command_argc;
    char *const *command_argv;
    int port_mapping_count;
    const struct port_map *port_mappings;
};

/*
 * Acquisition state for one startup attempt. Subsystem state is added here
 * only when that subsystem is migrated into the staged runtime.
 */
struct container_runtime_state {
    enum container_stage stage;
    struct cgroup_state cgroup;
};

#endif
