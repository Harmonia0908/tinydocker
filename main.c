#include <stdlib.h>
#include <argp.h>
#include "logger/log.h"
#include "docker/container.h"
#include "cmdparser/cmdparser.h"
#include "docker/network.h"

int main(int argc, char **argv)
{
    struct docker_cmd result = parse_docker_cmd(argc, argv);
    switch (result.cmd_type)
    {
    case DOCKER_RUN:
        if (init_docker_env() != 0) {
            return EXIT_FAILURE;
        }
        print_docker_cmds(result);
        docker_run(result.arguments);
        break;
    case DOCKER_COMMIT:
        print_docker_cmds(result);
        docker_commit(result.arguments);
        break;
    case DOCKER_PS:
        print_docker_cmds(result);
        docker_ps(result.arguments);
        break;
    case DOCKER_TOP:
        print_docker_cmds(result);
        docker_top(result.arguments);
        break;
    case DOCKER_EXEC:
        print_docker_cmds(result);
        docker_exec(result.arguments);
        break;
    case DOCKER_STOP:
        print_docker_cmds(result);
        docker_stop(result.arguments);
        break;
    case DOCKER_RM:
        print_docker_cmds(result);
        docker_rm(result.arguments);
        break;
    case DOCKER_INSPECT:
        docker_inspect(result.arguments);
        break;
    case DOCKER_STATS:
        docker_stats(result.arguments);
        break;
    case DOCKER_NETWORK_CREATE:
        if (init_runtime_dirs() != 0) {
            return EXIT_FAILURE;
        }
        print_docker_cmds(result);
        struct docker_network_create *cmd = (struct docker_network_create *) result.arguments;
        create_network(cmd->name, cmd->cider, "bridge");
        break;
    case DOCKER_NETWORK_LIST:
        list_network();
        break;
    case DOCKER_NETWORK_RM:
        print_docker_cmds(result);
        remove_docker_network(result.arguments);
        break;
    default:
        puts("not support");
        exit(-1);
    }

    return 0;
}
