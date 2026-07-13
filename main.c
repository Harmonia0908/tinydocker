#include <stdlib.h>
#include <argp.h>
#include "logger/log.h"
#include "docker/container.h"
#include "cmdparser/cmdparser.h"
#include "docker/network.h"

int main(int argc, char **argv)
{
    struct docker_cmd result = parse_docker_cmd(argc, argv);
    int command_result = -1;
    switch (result.cmd_type)
    {
    case DOCKER_RUN:
        if (init_docker_env() != 0) {
            return EXIT_FAILURE;
        }
        print_docker_cmds(result);
        command_result = docker_run(result.arguments);
        break;
    case DOCKER_COMMIT:
        print_docker_cmds(result);
        command_result = docker_commit(result.arguments);
        break;
    case DOCKER_PS:
        print_docker_cmds(result);
        command_result = docker_ps(result.arguments);
        break;
    case DOCKER_TOP:
        print_docker_cmds(result);
        command_result = docker_top(result.arguments);
        break;
    case DOCKER_EXEC:
        print_docker_cmds(result);
        command_result = docker_exec(result.arguments);
        break;
    case DOCKER_STOP:
        print_docker_cmds(result);
        command_result = docker_stop(result.arguments);
        break;
    case DOCKER_RM:
        print_docker_cmds(result);
        command_result = docker_rm(result.arguments);
        break;
    case DOCKER_INSPECT:
        command_result = docker_inspect(result.arguments);
        break;
    case DOCKER_STATS:
        command_result = docker_stats(result.arguments);
        break;
    case DOCKER_NETWORK_CREATE:
        if (init_runtime_dirs() != 0) {
            return EXIT_FAILURE;
        }
        print_docker_cmds(result);
        struct docker_network_create *cmd = (struct docker_network_create *) result.arguments;
        command_result = create_network(cmd->name, cmd->cider, "bridge");
        break;
    case DOCKER_NETWORK_LIST:
        command_result = list_network();
        break;
    case DOCKER_NETWORK_RM:
        print_docker_cmds(result);
        command_result = remove_docker_network(result.arguments);
        break;
    default:
        fputs("not supported\n", stderr);
        return EXIT_FAILURE;
    }

    return command_result < 0 ? EXIT_FAILURE : command_result;
}
