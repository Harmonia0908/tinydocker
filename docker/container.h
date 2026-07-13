#ifndef CONTAINER_H
#define CONTAINER_H

#include "../cmdparser/cmdparser.h"
#include "../core/config.h"


int init_docker_env(void);
int init_runtime_dirs(void);

int docker_run(struct docker_run_arguments *args);

int docker_commit(struct docker_commit_arguments *args);

int docker_ps(struct docker_ps_arguments *args);

int docker_top(struct docker_top_arguments *args);

int docker_exec(struct docker_exec_arguments *args);

int docker_stop(struct docker_stop_arguments *args);

int docker_rm(struct docker_rm_arguments *args);

int docker_inspect(struct docker_inspect_arguments *args);

int docker_stats(struct docker_stats_arguments *args);

#endif
