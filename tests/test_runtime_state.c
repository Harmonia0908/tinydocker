#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../cmdparser/cmdparser.h"
#include "../core/config.h"
#include "../core/container_state.h"
#include "../core/fs.h"
#include "../core/network_state.h"
#include "../docker/network.h"
#include "../docker/status_info.h"
#include "../logger/log.h"
#include "../runtime/container.h"

static int failures;
static int runtime_directory_created;
static int workspace_created;
static int workspace_entered;
static char original_directory[PATH_MAX];
static char test_workspace[] = "build/test-runtime-state.XXXXXX";

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static int write_text_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    int result = 0;

    if (file == NULL) {
        return -1;
    }
    if (fputs(contents, file) == EOF) {
        result = -1;
    }
    if (fclose(file) != 0) {
        result = -1;
    }
    return result;
}

static char *read_text_file(const char *path)
{
    FILE *file = fopen(path, "r");
    long length;
    char *contents;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return NULL;
    }
    contents = calloc((size_t)length + 1U, 1U);
    if (contents == NULL) {
        (void)fclose(file);
        return NULL;
    }
    if (fread(contents, 1U, (size_t)length, file) != (size_t)length) {
        free(contents);
        contents = NULL;
    }
    if (fclose(file) != 0) {
        free(contents);
        contents = NULL;
    }
    return contents;
}

static int prepare_runtime_directory(void)
{
    if (getcwd(original_directory, sizeof(original_directory)) == NULL ||
        mkdtemp(test_workspace) == NULL) {
        return -1;
    }
    workspace_created = 1;
    if (chdir(test_workspace) != 0) {
        return -1;
    }
    workspace_entered = 1;
    if (mkdir(TINYDOCKER_RUNTIME_DIR, 0700) != 0) {
        return -1;
    }
    runtime_directory_created = 1;
    if (mkdir(CONTAINER_STATUS_INFO_DIR, 0700) != 0) {
        return -1;
    }
    return 0;
}

static int cleanup_runtime_directory(void)
{
    int result = 0;

    if (runtime_directory_created != 0 &&
        td_remove_tree(TINYDOCKER_RUNTIME_DIR) != 0) {
        result = -1;
    }
    runtime_directory_created = 0;
    if (workspace_entered != 0 && chdir(original_directory) != 0) {
        result = -1;
    }
    workspace_entered = 0;
    if (workspace_created != 0 && rmdir(test_workspace) != 0) {
        result = -1;
    }
    workspace_created = 0;
    return result;
}

static void test_container_metadata_public_interface(void)
{
    static const char expected[] =
        "pid=12345\n"
        "pid_start_time=67890\n"
        "detach=1\n"
        "container_id=baseline-id\n"
        "image=busybox.tar.xz\n"
        "command=/bin/sh -c echo baseline\n"
        "created=2026-07-16 12:00:00\n"
        "status=EXITED\n"
        "ip_addr=10.77.0.2\n"
        "name=baseline-meta\n"
        "volume_cnt=1\n"
        "/tmp/host:/data:ro\n";
    struct container_info input;
    struct container_info output;
    struct container_info listed[4];
    char metadata_path[1024];
    char *serialized;

    memset(&input, 0, sizeof(input));
    input.pid = 12345;
    input.pid_start_time = UINT64_C(67890);
    input.detach = 1;
    (void)snprintf(input.container_id, sizeof(input.container_id), "%s",
                   "baseline-id");
    (void)snprintf(input.image, sizeof(input.image), "%s", "busybox.tar.xz");
    (void)snprintf(input.command, sizeof(input.command), "%s",
                   "/bin/sh -c echo baseline");
    (void)snprintf(input.created, sizeof(input.created), "%s",
                   "2026-07-16 12:00:00");
    (void)snprintf(input.status, sizeof(input.status), "%s", "EXITED");
    (void)snprintf(input.ip_addr, sizeof(input.ip_addr), "%s", "10.77.0.2");
    (void)snprintf(input.name, sizeof(input.name), "%s", "baseline-meta");
    input.volume_cnt = 1;
    (void)snprintf(input.volumes[0], sizeof(input.volumes[0]), "%s",
                   "/tmp/host:/data:ro");

    CHECK(write_container_info(input.name, &input) == 0);
    CHECK(snprintf(metadata_path, sizeof(metadata_path), "%s/%s",
                   CONTAINER_STATUS_INFO_DIR, input.name) > 0);
    serialized = read_text_file(metadata_path);
    CHECK(serialized != NULL);
    if (serialized != NULL) {
        CHECK(strcmp(serialized, expected) == 0);
    }
    free(serialized);

    memset(&output, 0, sizeof(output));
    CHECK(read_container_info(input.name, &output) == 0);
    CHECK(output.pid == input.pid);
    CHECK(output.pid_start_time == input.pid_start_time);
    CHECK(strcmp(output.command, input.command) == 0);
    CHECK(strcmp(output.status, "EXITED") == 0);
    CHECK(output.volume_cnt == 1);
    CHECK(strcmp(output.volumes[0], input.volumes[0]) == 0);

    memset(listed, 0, sizeof(listed));
    CHECK(list_containers_info(listed, 4U) == 1);
    CHECK(strcmp(listed[0].name, input.name) == 0);
    CHECK(remove_status_info(input.name) == 0);
    CHECK(access(metadata_path, F_OK) != 0 && errno == ENOENT);
}

static void test_network_state_allocation_and_release(void)
{
    static const char empty_state[] =
        "baseline0:bridge:10.77.0.0/29:\n";
    static const char allocated_state[] =
        "baseline0:bridge:10.77.0.0/29:172818434;\n";
    char ip[64] = {0};
    char *serialized;

    CHECK(write_text_file(CONTAINER_NETWORKS_FILE, empty_state) == 0);
    CHECK(alloc_new_ip("baseline0", ip, (int)sizeof(ip)) == UINT32_C(172818434));
    CHECK(strcmp(ip, "10.77.0.2") == 0);
    serialized = read_text_file(CONTAINER_NETWORKS_FILE);
    CHECK(serialized != NULL);
    if (serialized != NULL) {
        CHECK(strcmp(serialized, allocated_state) == 0);
    }
    free(serialized);

    CHECK(release_used_ip("baseline0", ip) == 0);
    serialized = read_text_file(CONTAINER_NETWORKS_FILE);
    CHECK(serialized != NULL);
    if (serialized != NULL) {
        CHECK(strcmp(serialized, empty_state) == 0);
    }
    free(serialized);
}

static void test_network_state_serializes_concurrent_allocations(void)
{
    static const char empty_state[] =
        "baseline0:bridge:10.77.0.0/29:\n";
    pid_t children[2] = {-1, -1};
    char *serialized;
    struct td_network_record record;
    char error[160] = {0};

    CHECK(write_text_file(CONTAINER_NETWORKS_FILE, empty_state) == 0);
    for (size_t index = 0U; index < 2U; index++) {
        children[index] = fork();
        CHECK(children[index] >= 0);
        if (children[index] == 0) {
            char ip[64] = {0};
            _exit(alloc_new_ip("baseline0", ip, (int)sizeof(ip)) == 0U ? 1 : 0);
        }
    }
    for (size_t index = 0U; index < 2U; index++) {
        if (children[index] > 0) {
            int status = 0;
            CHECK(waitpid(children[index], &status, 0) == children[index]);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
    }

    serialized = read_text_file(CONTAINER_NETWORKS_FILE);
    CHECK(serialized != NULL);
    if (serialized != NULL) {
        serialized[strcspn(serialized, "\n")] = '\0';
        CHECK(td_parse_network_record(serialized, &record, error,
                                      sizeof(error)) == 0);
        CHECK(record.used_ip_count == 2U);
        if (record.used_ip_count == 2U) {
            CHECK(record.used_ips[0] != record.used_ips[1]);
        }
    }
    free(serialized);
}

static void test_cgroup_lifecycle_public_interface(void)
{
    struct cgroup_config config = {
        .cpu = -1,
        .memory = -1,
        .cpuset = NULL
    };
    struct cgroup_state state;
    char *contents;

    memset(&state, 0, sizeof(state));
    CHECK(mkdir("unowned-cgroup", 0700) == 0);
    (void)snprintf(state.path, sizeof(state.path), "%s", "unowned-cgroup");
    CHECK(cgroup_cleanup(&state) == 0);
    CHECK(access("unowned-cgroup", F_OK) == 0);
    CHECK(rmdir("unowned-cgroup") == 0);

    memset(&state, 0, sizeof(state));
    CHECK(mkdir("owned-cgroup", 0700) == 0);
    CHECK(write_text_file("owned-cgroup/cgroup.procs", "") == 0);
    (void)snprintf(state.path, sizeof(state.path), "%s", "owned-cgroup");
    state.created = 1;
    CHECK(cgroup_apply(&state, 4242) == 0);
    contents = read_text_file("owned-cgroup/cgroup.procs");
    CHECK(contents != NULL);
    if (contents != NULL) {
        CHECK(strcmp(contents, "4242\n") == 0);
    }
    free(contents);
    errno = 0;
    CHECK(cgroup_prepare("second-cgroup", &config, &state) == -1);
    CHECK(errno == EBUSY);
    CHECK(state.created == 1);
    CHECK(strcmp(state.path, "owned-cgroup") == 0);
    CHECK(cgroup_cleanup(&state) == -1);
    CHECK(state.created == 1);
    CHECK(strcmp(state.path, "owned-cgroup") == 0);
    CHECK(unlink("owned-cgroup/cgroup.procs") == 0);
    CHECK(cgroup_cleanup(&state) == 0);
    CHECK(state.created == 0);
    CHECK(access("owned-cgroup", F_OK) != 0 && errno == ENOENT);

    memset(&state, 0, sizeof(state));
    CHECK(cgroup_apply(&state, 4242) == -1);
    CHECK(cgroup_prepare(NULL, &config, &state) == -1);
    CHECK(state.created == 0);
}

int main(void)
{
    log_set_quiet(true);
    CHECK(prepare_runtime_directory() == 0);
    if (failures == 0) {
        test_container_metadata_public_interface();
        test_network_state_allocation_and_release();
        test_network_state_serializes_concurrent_allocations();
        test_cgroup_lifecycle_public_interface();
    }
    CHECK(cleanup_runtime_directory() == 0);

    if (failures != 0) {
        fprintf(stderr, "%d runtime state assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all non-privileged runtime state tests passed");
    return EXIT_SUCCESS;
}
