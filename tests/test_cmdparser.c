#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../cmdparser/cmdparser.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static char *capture_command_print(struct docker_cmd command)
{
    FILE *capture = tmpfile();
    int saved_stdout;
    long length;
    char *output;

    if (capture == NULL) {
        return NULL;
    }
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || fflush(stdout) != 0 ||
        dup2(fileno(capture), STDOUT_FILENO) < 0) {
        if (saved_stdout >= 0) {
            (void)close(saved_stdout);
        }
        (void)fclose(capture);
        return NULL;
    }
    print_docker_cmds(command);
    if (fflush(stdout) != 0 || dup2(saved_stdout, STDOUT_FILENO) < 0) {
        (void)close(saved_stdout);
        (void)fclose(capture);
        return NULL;
    }
    (void)close(saved_stdout);
    if (fseek(capture, 0, SEEK_END) != 0 || (length = ftell(capture)) < 0 ||
        fseek(capture, 0, SEEK_SET) != 0) {
        (void)fclose(capture);
        return NULL;
    }
    output = calloc((size_t)length + 1U, 1U);
    if (output == NULL ||
        fread(output, 1U, (size_t)length, capture) != (size_t)length) {
        free(output);
        output = NULL;
    }
    (void)fclose(capture);
    return output;
}

static void test_run_configuration_and_printed_output(void)
{
    static const char expected_printed[] =
        "interactive=0\n"
        "detach=1\n"
        "cpu=2000\n"
        "memory=4096\n"
        "image=busybox.tar.xz\n"
        "name=baseline\n"
        "host_dir:/tmp, container_dir:/data, ro:1\n"
        "env_key:BASELINE, env_val:value\n"
        "args_count=3\n"
        "/bin/sh -c echo baseline \n"
        "port_mapping=1\n"
        "host:18080->container:8080\n"
        "\n";
    char program[] = "tinydocker";
    char action[] = "run";
    char detach[] = "-d";
    char cpu_option[] = "-c";
    char cpu[] = "2000";
    char memory_option[] = "-m";
    char memory[] = "4096";
    char name_option[] = "-n";
    char name[] = "baseline";
    char env_option[] = "-e";
    char env[] = "BASELINE=value=tail";
    char volume_option[] = "-v";
    char volume[] = "/tmp:/data:ro";
    char port_option[] = "-p";
    char port[] = "18080:8080";
    char image[] = "busybox.tar.xz";
    char shell[] = "/bin/sh";
    char shell_option[] = "-c";
    char shell_command[] = "echo baseline";
    char *argv[] = {
        program, action, detach, cpu_option, cpu, memory_option, memory,
        name_option, name, env_option, env, volume_option, volume,
        port_option, port, image, shell, shell_option, shell_command, NULL
    };
    struct docker_cmd command = parse_docker_cmd(19, argv);
    struct docker_run_arguments *arguments = command.arguments;
    char *printed;

    CHECK(command.cmd_type == DOCKER_RUN);
    CHECK(arguments != NULL);
    if (arguments == NULL) {
        return;
    }
    CHECK(arguments->detach == 1);
    CHECK(arguments->interactive == 0);
    CHECK(arguments->cpu == 2000);
    CHECK(arguments->memory == 4096);
    CHECK(strcmp(arguments->image, "busybox.tar.xz") == 0);
    CHECK(strcmp(arguments->name, "baseline") == 0);
    CHECK(arguments->env_cnt == 1);
    CHECK(strcmp(arguments->env[0].key, "BASELINE") == 0);
    CHECK(strcmp(arguments->env[0].val, "value") == 0);
    CHECK(arguments->volume_cnt == 1);
    CHECK(strcmp(arguments->volumes[0].host, "/tmp") == 0);
    CHECK(strcmp(arguments->volumes[0].container, "/data") == 0);
    CHECK(arguments->volumes[0].ro == 1);
    CHECK(arguments->port_mapping_cnt == 1);
    CHECK(arguments->port_mapping[0].host_port == 18080);
    CHECK(arguments->port_mapping[0].container_port == 8080);
    CHECK(arguments->container_argc == 3);
    CHECK(strcmp(arguments->container_argv[0], "/bin/sh") == 0);
    CHECK(strcmp(arguments->container_argv[1], "-c") == 0);
    CHECK(strcmp(arguments->container_argv[2], "echo baseline") == 0);

    printed = capture_command_print(command);
    CHECK(printed != NULL);
    if (printed != NULL) {
        CHECK(strcmp(printed, expected_printed) == 0);
    }
    free(printed);
}

static void test_stop_accepts_zero_timeout(void)
{
    char program[] = "tinydocker";
    char action[] = "stop";
    char timeout_option[] = "-t";
    char timeout[] = "0";
    char first[] = "first";
    char second[] = "second";
    char *argv[] = {
        program, action, timeout_option, timeout, first, second, NULL
    };
    struct docker_cmd command = parse_docker_cmd(6, argv);
    struct docker_stop_arguments *arguments = command.arguments;

    CHECK(command.cmd_type == DOCKER_STOP);
    CHECK(arguments != NULL);
    if (arguments != NULL) {
        CHECK(arguments->time == 0);
        CHECK(arguments->container_cnt == 2);
        CHECK(strcmp(arguments->container_names[0], "first") == 0);
        CHECK(strcmp(arguments->container_names[1], "second") == 0);
        CHECK(arguments->container_names[2] == NULL);
    }
}

static void check_parse_failure(int argc, char **argv,
                                const char *expected_output)
{
    int output_pipe[2];
    pid_t child;
    char output[2048] = {0};
    size_t used = 0U;
    int status = 0;

    if (pipe(output_pipe) != 0) {
        CHECK(0);
        return;
    }
    if (fflush(NULL) != 0) {
        CHECK(0);
        (void)close(output_pipe[0]);
        (void)close(output_pipe[1]);
        return;
    }
    child = fork();
    if (child < 0) {
        CHECK(0);
        (void)close(output_pipe[0]);
        (void)close(output_pipe[1]);
        return;
    }
    if (child == 0) {
        (void)close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        (void)close(output_pipe[1]);
        (void)parse_docker_cmd(argc, argv);
        _exit(0);
    }
    (void)close(output_pipe[1]);
    while (used + 1U < sizeof(output)) {
        ssize_t count = read(output_pipe[0], output + used,
                             sizeof(output) - used - 1U);
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
    }
    (void)close(output_pipe[0]);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 255);
    CHECK(strcmp(output, expected_output) == 0);
}

static void test_invalid_commands_exit_with_current_messages(void)
{
    char program[] = "tinydocker";
    char *missing_command[] = {program, NULL};
    char network[] = "network";
    char connect[] = "connect";
    char network_name[] = "baseline0";
    char container_name[] = "baseline";
    char *unsupported_connect[] = {
        program, network, connect, network_name, container_name, NULL
    };

    check_parse_failure(1, missing_command, "请输入有效的docker命令\n");
    check_parse_failure(5, unsupported_connect, "wrong input command\n");
}

int main(void)
{
    test_run_configuration_and_printed_output();
    test_stop_accepts_zero_timeout();
    test_invalid_commands_exit_with_current_messages();

    if (failures != 0) {
        fprintf(stderr, "%d command parser assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all command parser behavior tests passed");
    return EXIT_SUCCESS;
}
