#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/cgroup_parse.h"
#include "../core/fs.h"
#include "../core/network_state.h"
#include "../core/process.h"
#include "../core/safety.h"
#include "../core/status_codec.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_name_validation(void)
{
    char error[128] = {0};
    char long_name[130];

    memset(long_name, 'a', sizeof(long_name));
    long_name[129] = '\0';

    CHECK(td_validate_name("demo-1.alpha", 127, error, sizeof(error)) == 0);
    CHECK(td_validate_name(".", 127, error, sizeof(error)) == -1);
    CHECK(strstr(error, "reserved") != NULL);
    CHECK(td_validate_name("..", 127, error, sizeof(error)) == -1);
    CHECK(td_validate_name("../../host", 127, error, sizeof(error)) == -1);
    CHECK(td_validate_name(long_name, 127, error, sizeof(error)) == -1);
}

static void test_numeric_and_port_parsing(void)
{
    char error[128] = {0};
    long value = 0;
    int host_port = 0;
    int container_port = 0;
    char host_path[128] = {0};
    char container_path[128] = {0};
    int read_only = -1;

    CHECK(td_parse_long("20000", 1000, 100000, &value, error, sizeof(error)) == 0);
    CHECK(value == 20000);
    CHECK(td_parse_long("20ms", 1, 100000, &value, error, sizeof(error)) == -1);
    CHECK(strstr(error, "integer") != NULL);
    CHECK(td_parse_port_mapping("18080:8080", &host_port, &container_port,
                                error, sizeof(error)) == 0);
    CHECK(host_port == 18080);
    CHECK(container_port == 8080);
    CHECK(td_parse_port_mapping("0:80", &host_port, &container_port,
                                error, sizeof(error)) == -1);
    CHECK(td_parse_port_mapping("80:65536", &host_port, &container_port,
                                error, sizeof(error)) == -1);
    CHECK(td_parse_port_mapping("80:90:100", &host_port, &container_port,
                                error, sizeof(error)) == -1);
    CHECK(td_parse_volume_spec("/tmp/data:/var/lib/data:ro", host_path,
                               sizeof(host_path), container_path,
                               sizeof(container_path), &read_only,
                               error, sizeof(error)) == 0);
    CHECK(strcmp(host_path, "/tmp/data") == 0);
    CHECK(strcmp(container_path, "/var/lib/data") == 0);
    CHECK(read_only == 1);
    CHECK(td_parse_volume_spec("relative:/data", host_path,
                               sizeof(host_path), container_path,
                               sizeof(container_path), &read_only,
                               error, sizeof(error)) == -1);
    CHECK(strstr(error, "absolute") != NULL);
    CHECK(td_parse_volume_spec("/tmp/data:/../../host", host_path,
                               sizeof(host_path), container_path,
                               sizeof(container_path), &read_only,
                               error, sizeof(error)) == -1);
    CHECK(strstr(error, "traversal") != NULL);
    CHECK(td_parse_volume_spec("/tmp/data:/data:execute", host_path,
                               sizeof(host_path), container_path,
                               sizeof(container_path), &read_only,
                               error, sizeof(error)) == -1);
    CHECK(strstr(error, "mode") != NULL);
    CHECK(td_parse_volume_spec("/tmp/data:/data:ro:extra", host_path,
                               sizeof(host_path), container_path,
                               sizeof(container_path), &read_only,
                               error, sizeof(error)) == -1);
}

static void test_safe_paths(void)
{
    char output[256] = {0};
    char error[128] = {0};

    CHECK(td_build_named_path("/runtime", "containers", "demo", output,
                              sizeof(output), error, sizeof(error)) == 0);
    CHECK(strcmp(output, "/runtime/containers/demo") == 0);
    CHECK(td_join_rootfs_path("/runtime/root", "/var/data", output,
                              sizeof(output), error, sizeof(error)) == 0);
    CHECK(strcmp(output, "/runtime/root/var/data") == 0);
    CHECK(td_join_rootfs_path("/runtime/root", "var/data", output,
                              sizeof(output), error, sizeof(error)) == -1);
    CHECK(td_join_rootfs_path("/runtime/root", "/../../etc", output,
                              sizeof(output), error, sizeof(error)) == -1);
    CHECK(strstr(error, "traversal") != NULL);
    CHECK(td_join_rootfs_path("/runtime/root", "/", output,
                              sizeof(output), error, sizeof(error)) == -1);

    char first_veth[16] = {0};
    char second_veth[16] = {0};
    char peer_veth[16] = {0};
    CHECK(td_make_veth_name("same-prefix-container-a", first_veth,
                            sizeof(first_veth)) == 0);
    CHECK(td_make_veth_name("same-prefix-container-b", second_veth,
                            sizeof(second_veth)) == 0);
    CHECK(strlen(first_veth) <= 15U);
    CHECK(strcmp(first_veth, second_veth) != 0);
    CHECK(td_make_veth_peer_name("same-prefix-container-a", peer_veth,
                                 sizeof(peer_veth)) == 0);
    CHECK(strlen(peer_veth) <= 15U);
    CHECK(strcmp(first_veth, peer_veth) != 0);
    CHECK(td_archive_entry_is_safe("usr/bin/tool") == 1);
    CHECK(td_archive_entry_is_safe("./usr/bin/tool") == 1);
    CHECK(td_archive_entry_is_safe("../host") == 0);
    CHECK(td_archive_entry_is_safe("usr/../../host") == 0);
    CHECK(td_archive_entry_is_safe("/absolute/path") == 0);
}

static void test_network_state_codec(void)
{
    const char *valid = "tdnet:bridge:172.18.0.0/24:2886860802;2886860803;";
    struct td_network_record record;
    struct td_network_record round_trip;
    char output[4096] = {0};
    char oversized[4096] = "many:bridge:10.0.0.0/24:";
    char error[160] = {0};

    CHECK(td_parse_network_record(valid, &record, error, sizeof(error)) == 0);
    CHECK(strcmp(record.name, "tdnet") == 0);
    CHECK(strcmp(record.driver, "bridge") == 0);
    CHECK(record.used_ip_count == 2U);
    CHECK(td_format_network_record(&record, output, sizeof(output), error,
                                   sizeof(error)) == 0);
    CHECK(strcmp(output, valid) == 0);
    CHECK(td_parse_network_record(output, &round_trip, error,
                                  sizeof(error)) == 0);
    CHECK(round_trip.used_ip_count == record.used_ip_count);
    CHECK(td_parse_network_record("missing:bridge:172.18.0.0/24", &record,
                                  error, sizeof(error)) == -1);
    CHECK(strstr(error, "missing") != NULL);
    CHECK(td_parse_network_record("bad:bridge:172.18.0.1/24:", &record,
                                  error, sizeof(error)) == -1);
    CHECK(strstr(error, "network address") != NULL);
    CHECK(td_parse_network_record("bad:bridge:172.18.0.0/31:", &record,
                                  error, sizeof(error)) == -1);
    CHECK(td_parse_network_record("bad:bridge:172.18.0.0/24:not-a-number;",
                                  &record, error, sizeof(error)) == -1);
    CHECK(td_parse_network_record("bad:bridge:172.18.0.0/24:42;42;", &record,
                                  error, sizeof(error)) == -1);
    CHECK(td_parse_network_record("name-that-is-too-long:bridge:172.18.0.0/24:",
                                  &record, error, sizeof(error)) == -1);
    for (unsigned int index = 1U; index <= TD_NETWORK_MAX_USED_IPS + 1U;
         index++) {
        size_t used = strlen(oversized);
        int written = snprintf(oversized + used, sizeof(oversized) - used,
                               "%u;", index);
        CHECK(written > 0 && (size_t)written < sizeof(oversized) - used);
    }
    CHECK(td_parse_network_record(oversized, &record, error,
                                  sizeof(error)) == -1);
    CHECK(strstr(error, "too many") != NULL);

    memset(&record, 0, sizeof(record));
    (void)snprintf(record.name, sizeof(record.name), "empty");
    (void)snprintf(record.driver, sizeof(record.driver), "bridge");
    (void)snprintf(record.cidr, sizeof(record.cidr), "10.0.0.0/24");
    CHECK(td_format_network_record(&record, output, sizeof(output), error,
                                   sizeof(error)) == 0);
    CHECK(strcmp(output, "empty:bridge:10.0.0.0/24:") == 0);
}

static void test_cidr_and_cgroup_parsing(void)
{
    char error[128] = {0};
    char value[64] = {0};
    char formatted[64] = {0};
    uint32_t network = 0;
    unsigned int prefix = 0;

    CHECK(td_parse_ipv4_cidr("172.18.0.0/24", &network, &prefix,
                             error, sizeof(error)) == 0);
    CHECK(network == UINT32_C(0xac120000));
    CHECK(prefix == 24U);
    CHECK(td_parse_ipv4_cidr("172.18.0.0/33", &network, &prefix,
                             error, sizeof(error)) == -1);
    CHECK(td_parse_ipv4_cidr("172.18.0.1;id/24", &network, &prefix,
                             error, sizeof(error)) == -1);

    CHECK(td_parse_cgroup_stat("user_usec 4\nusage_usec 12345\nsystem_usec 9\n",
                               "usage_usec", value, sizeof(value),
                               error, sizeof(error)) == 0);
    CHECK(strcmp(value, "12345") == 0);
    CHECK(td_parse_cgroup_stat("usage_usec nope\n", "usage_usec", value,
                               sizeof(value), error, sizeof(error)) == -1);
    CHECK(td_format_bytes("1536", formatted, sizeof(formatted),
                          error, sizeof(error)) == 0);
    CHECK(strcmp(formatted, "1.5KB") == 0);
    CHECK(td_format_bytes("max", formatted, sizeof(formatted),
                          error, sizeof(error)) == 0);
    CHECK(strcmp(formatted, "max") == 0);
    CHECK(td_format_bytes("12oops", formatted, sizeof(formatted),
                          error, sizeof(error)) == -1);

    int pids[2] = {0};
    size_t pid_count = 0U;
    CHECK(td_parse_cgroup_pid_list("12\n34\n", pids, 2U, &pid_count,
                                   error, sizeof(error)) == 0);
    CHECK(pid_count == 2U);
    CHECK(pids[0] == 12 && pids[1] == 34);
    CHECK(td_parse_cgroup_pid_list("12x\n", pids, 2U, &pid_count,
                                   error, sizeof(error)) == -1);
    CHECK(td_parse_cgroup_pid_list("12\n34\n", pids, 1U, &pid_count,
                                   error, sizeof(error)) == -1);
}

static void test_proc_stat_parser(void)
{
    const char *stat_line =
        "4242 (command with ) parenthesis) S 1 2 3 4 5 6 7 8 9 10 11 12 "
        "13 14 15 16 17 18 987654 20\n";
    unsigned long long start_time = 0;
    char error[128] = {0};

    CHECK(td_parse_proc_stat_start_time(stat_line, &start_time,
                                        error, sizeof(error)) == 0);
    CHECK(start_time == 987654ULL);
    CHECK(td_parse_proc_stat_start_time("broken", &start_time,
                                        error, sizeof(error)) == -1);
}

static void test_status_codec(void)
{
    const char *valid =
        "pid=4242\n"
        "pid_start_time=987654\n"
        "detach=1\n"
        "container_id=1700000000\n"
        "image=busybox.tar.xz\n"
        "command=/bin/sh -c sleep 5\n"
        "created=2023-11-14 22:13:20\n"
        "status=RUNNING\n"
        "ip_addr=172.18.0.2\n"
        "name=demo\n"
        "volume_cnt=1\n"
        "/tmp/data:/data:ro\n";
    const char *missing_status =
        "pid=42\ndetach=0\ncontainer_id=id\nimage=x\ncommand=/bin/sh\n"
        "created=2023-11-14 22:13:20\nname=demo\nvolume_cnt=0\n";
    struct container_info info;
    char error[160] = {0};

    CHECK(td_parse_container_info(valid, strlen(valid), "demo", &info,
                                  error, sizeof(error)) == 0);
    CHECK(info.pid == 4242);
    CHECK(info.pid_start_time == 987654ULL);
    CHECK(strcmp(info.status, "RUNNING") == 0);
    CHECK(info.volume_cnt == 1);
    CHECK(td_parse_container_info(valid, strlen(valid), "other", &info,
                                  error, sizeof(error)) == -1);
    CHECK(strstr(error, "name") != NULL);
    CHECK(td_parse_container_info(missing_status, strlen(missing_status),
                                  "demo", &info, error, sizeof(error)) == -1);
    CHECK(strstr(error, "missing") != NULL);

    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        char serialized[4096] = {0};
        CHECK(td_parse_container_info(valid, strlen(valid), "demo", &info,
                                      error, sizeof(error)) == 0);
        CHECK(td_write_container_info(stream, &info, error, sizeof(error)) == 0);
        CHECK(fflush(stream) == 0);
        CHECK(fseek(stream, 0L, SEEK_SET) == 0);
        size_t count = fread(serialized, 1, sizeof(serialized), stream);
        struct container_info round_trip;
        CHECK(td_parse_container_info(serialized, count, "demo", &round_trip,
                                      error, sizeof(error)) == 0);
        CHECK(round_trip.pid_start_time == info.pid_start_time);
        CHECK(strcmp(round_trip.volumes[0], info.volumes[0]) == 0);
        CHECK(fclose(stream) == 0);
    }

    CHECK(td_parse_container_info(valid, strlen(valid), "demo", &info,
                                  error, sizeof(error)) == 0);
    (void)snprintf(info.command, sizeof(info.command), "bad\ncommand");
    stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(td_write_container_info(stream, &info, error, sizeof(error)) == -1);
        CHECK(strstr(error, "invalid") != NULL);
        CHECK(fclose(stream) == 0);
    }
}

static void test_cleanup_idempotency(void)
{
    char template[] = "/tmp/tinydocker-core-test.XXXXXX";
    char nested[256] = {0};
    char file_path[256] = {0};
    char *root = mkdtemp(template);
    int fd;

    CHECK(root != NULL);
    if (root == NULL) {
        return;
    }
    CHECK(snprintf(nested, sizeof(nested), "%s/nested", root) > 0);
    CHECK(mkdir(nested, 0700) == 0);
    CHECK(snprintf(file_path, sizeof(file_path), "%s/data", nested) > 0);
    fd = open(file_path, O_CREAT | O_WRONLY | O_EXCL, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, "ok", 2) == 2);
        CHECK(close(fd) == 0);
    }

    CHECK(td_remove_tree(root) == 0);
    CHECK(td_remove_tree(root) == 0);
}

static void test_directory_creation_stays_beneath_rootfs(void)
{
    char root_template[] = "/tmp/tinydocker-rootfs-test.XXXXXX";
    char outside_template[] = "/tmp/tinydocker-outside-test.XXXXXX";
    char output[512] = {0};
    char link_path[512] = {0};
    char *root = mkdtemp(root_template);
    char *outside = mkdtemp(outside_template);

    CHECK(root != NULL);
    CHECK(outside != NULL);
    if (root == NULL || outside == NULL) {
        return;
    }
    CHECK(td_ensure_directory_beneath(root, "/var/lib/data", output,
                                      sizeof(output)) == 0);
    CHECK(strstr(output, root) == output);
    CHECK(snprintf(link_path, sizeof(link_path), "%s/escape", root) > 0);
    CHECK(symlink(outside, link_path) == 0);
    CHECK(td_ensure_directory_beneath(root, "/escape/owned", output,
                                      sizeof(output)) == -1);
    CHECK(td_remove_tree(root) == 0);
    CHECK(td_remove_tree(outside) == 0);
}

static void test_shell_free_command_runner(void)
{
    char *const arguments[] = {"true", NULL};
    char *const capture_arguments[] = {"printf", "%s", "captured", NULL};
    char *output = NULL;
    size_t output_size = 0U;

    CHECK(td_run_command(arguments) == 0);
    CHECK(td_capture_command(capture_arguments, &output, &output_size) == 0);
    CHECK(output != NULL);
    if (output != NULL) {
        CHECK(output_size == 8U);
        CHECK(strcmp(output, "captured") == 0);
    }
    free(output);
}

int main(void)
{
    test_name_validation();
    test_numeric_and_port_parsing();
    test_safe_paths();
    test_cidr_and_cgroup_parsing();
    test_network_state_codec();
    test_proc_stat_parser();
    test_status_codec();
    test_cleanup_idempotency();
    test_directory_creation_stays_beneath_rootfs();
    test_shell_free_command_runner();

    if (failures != 0) {
        fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all non-privileged core tests passed");
    return EXIT_SUCCESS;
}
