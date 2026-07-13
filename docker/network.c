#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdint.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "network.h"
#include "cgroup.h"
#include "status_info.h"
#include "../cmdparser/cmdparser.h"
#include "../logger/log.h"
#include "../core/process.h"
#include "../core/network_state.h"
#include "../core/safety.h"


//IP地址转主机序整数
static unsigned int str_ip_to_int(const char *ip) {
    struct in_addr addr;
    if (ip == NULL || inet_pton(AF_INET, ip, &addr) != 1) {
        return 0U;
    }
    return ntohl(addr.s_addr);
}

//主机序列整数转字符串IP
static void int_to_str_ip(unsigned int int_ip, char *ip_buf, int ip_buf_size) {
    struct in_addr addr;
    addr.s_addr = htonl(int_ip);
    inet_ntop(AF_INET, &addr, ip_buf, (socklen_t)ip_buf_size);
}

//获取IP地址的有效范围
static int get_cidr_range(const char *cidr, unsigned int *minimum,
                          unsigned int *maximum, unsigned int *prefix) {
    char error[160] = {0};
    uint32_t network = 0;
    unsigned int parsed_prefix = 0;
    uint32_t mask;

    if (minimum == NULL || maximum == NULL ||
        td_parse_ipv4_cidr(cidr, &network, &parsed_prefix,
                           error, sizeof(error)) != 0) {
        log_error("invalid network CIDR %s: %s",
                  cidr == NULL ? "(null)" : cidr, error);
        return -1;
    }
    mask = parsed_prefix == 0U ? UINT32_C(0) :
        UINT32_MAX << (32U - parsed_prefix);
    *minimum = network;
    *maximum = network | ~mask;
    if (prefix != NULL) {
        *prefix = parsed_prefix;
    }
    return 0;
}


static int load_network_records(struct td_network_record *records,
                                size_t capacity, size_t *record_count) {
    enum { MAX_NETWORK_STATE_SIZE = 1024 * 1024 };
    char line[4096] = {0};
    struct stat status;
    int state_fd;
    FILE *file;
    size_t count = 0U;

    if (records == NULL || capacity == 0U || record_count == NULL) {
        errno = EINVAL;
        return -1;
    }
    state_fd = open(CONTAINER_NETWORKS_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (state_fd < 0) {
        if (errno == ENOENT) {
            *record_count = 0U;
            return 0;
        }
        log_error("failed to open %s: %s", CONTAINER_NETWORKS_FILE,
                  strerror(errno));
        return -1;
    }
    if (fstat(state_fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || status.st_size > MAX_NETWORK_STATE_SIZE) {
        log_error("network state is not a safe regular file");
        (void)close(state_fd);
        return -1;
    }
    file = fdopen(state_fd, "r");
    if (file == NULL) {
        (void)close(state_fd);
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char error[160] = {0};
        size_t length = strlen(line);

        if (length == 0U || (line[length - 1U] != '\n' && !feof(file))) {
            log_error("network state contains an oversized record");
            (void)fclose(file);
            return -1;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (count >= capacity ||
            td_parse_network_record(line, &records[count], error,
                                    sizeof(error)) != 0) {
            log_error("corrupt network state record: %s",
                      count >= capacity ? "record capacity exceeded" : error);
            (void)fclose(file);
            return -1;
        }
        count++;
    }
    if (ferror(file) != 0) {
        log_error("failed to read network state: %s", strerror(errno));
        (void)fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        log_error("failed to close network state: %s", strerror(errno));
        return -1;
    }
    *record_count = count;
    return 0;
}

static int lock_network_state(void) {
    char lock_path[1024] = {0};
    int lock_fd;
    int written = snprintf(lock_path, sizeof(lock_path), "%s.lock",
                           CONTAINER_NETWORKS_FILE);

    if (written < 0 || (size_t)written >= sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                   S_IRUSR | S_IWUSR);
    if (lock_fd < 0) {
        return -1;
    }
    while (flock(lock_fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            (void)close(lock_fd);
            return -1;
        }
    }
    return lock_fd;
}

static int write_network_records(const struct td_network_record *records,
                                 size_t record_count) {
    char temporary_path[1024] = {0};
    int temporary_fd = -1;
    FILE *file = NULL;
    int result = -1;

    for (unsigned int attempt = 0U; attempt < 100U; attempt++) {
        int written = snprintf(temporary_path, sizeof(temporary_path),
                               "%s.tmp.%ld.%u", CONTAINER_NETWORKS_FILE,
                               (long)getpid(), attempt);
        if (written < 0 || (size_t)written >= sizeof(temporary_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        temporary_fd = open(temporary_path,
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            S_IRUSR | S_IWUSR);
        if (temporary_fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (temporary_fd < 0) {
        return -1;
    }
    file = fdopen(temporary_fd, "w");
    if (file == NULL) {
        goto cleanup;
    }
    temporary_fd = -1;
    for (size_t index = 0U; index < record_count; index++) {
        char serialized[4096] = {0};
        char error[160] = {0};
        if (td_format_network_record(&records[index], serialized,
                                     sizeof(serialized), error,
                                     sizeof(error)) != 0 ||
            fprintf(file, "%s\n", serialized) < 0) {
            log_error("failed to serialize network state: %s", error);
            goto cleanup;
        }
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;
    if (rename(temporary_path, CONTAINER_NETWORKS_FILE) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (file != NULL) {
        (void)fclose(file);
    } else if (temporary_fd >= 0) {
        (void)close(temporary_fd);
    }
    if (result != 0) {
        (void)unlink(temporary_path);
    }
    return result;
}

static int add_network_info(const struct td_network_record *network) {
    struct td_network_record records[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;
    int lock_fd = lock_network_state();
    int result = -1;

    if (lock_fd < 0) {
        return -1;
    }
    if (load_network_records(records, TD_NETWORK_MAX_RECORDS, &count) != 0 ||
        count >= TD_NETWORK_MAX_RECORDS) {
        goto cleanup;
    }
    for (size_t index = 0U; index < count; index++) {
        if (strcmp(records[index].name, network->name) == 0) {
            errno = EEXIST;
            goto cleanup;
        }
    }
    records[count++] = *network;
    result = write_network_records(records, count);

cleanup:
    (void)close(lock_fd);
    return result;
}

static int read_network_info(const char *name, struct td_network_record *network) {
    struct td_network_record records[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;

    if (load_network_records(records, TD_NETWORK_MAX_RECORDS, &count) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < count; index++) {
        if (strcmp(records[index].name, name) == 0) {
            *network = records[index];
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

static int network_info_exists(const char *name) {
    struct td_network_record records[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;

    if (load_network_records(records, TD_NETWORK_MAX_RECORDS, &count) != 0) {
        return -1;
    }
    for (size_t index = 0U; index < count; index++) {
        if (strcmp(records[index].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int delete_network_info(const char *name) {
    struct td_network_record records[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;
    size_t output_count = 0U;
    int lock_fd = lock_network_state();
    int result = -1;

    if (lock_fd < 0) {
        return -1;
    }
    if (load_network_records(records, TD_NETWORK_MAX_RECORDS, &count) != 0) {
        goto cleanup;
    }
    for (size_t index = 0U; index < count; index++) {
        if (strcmp(records[index].name, name) != 0) {
            records[output_count++] = records[index];
        }
    }
    result = write_network_records(records, output_count);

cleanup:
    (void)close(lock_fd);
    return result;
}


static int net_has_exist(char *brname) {
    return if_nametoindex(brname) == 0U ? 0 : 1;
}

int create_network(char *name, char *cidr_network, char *driver) {
    struct td_network_record network_record;
    char serialized[4096] = {0};
    char validation_error[160] = {0};
    int metadata_exists;
    int name_length;
    int driver_length;
    int cidr_length;

    memset(&network_record, 0, sizeof(network_record));
    if (name == NULL || cidr_network == NULL || driver == NULL) {
        log_error("invalid network configuration: null field");
        return -1;
    }
    name_length = snprintf(network_record.name, sizeof(network_record.name),
                           "%s", name);
    driver_length = snprintf(network_record.driver,
                             sizeof(network_record.driver), "%s", driver);
    cidr_length = snprintf(network_record.cidr, sizeof(network_record.cidr),
                           "%s", cidr_network);
    if (name_length < 0 || (size_t)name_length >= sizeof(network_record.name) ||
        driver_length < 0 ||
        (size_t)driver_length >= sizeof(network_record.driver) ||
        cidr_length < 0 || (size_t)cidr_length >= sizeof(network_record.cidr) ||
        td_format_network_record(&network_record, serialized,
                                 sizeof(serialized), validation_error,
                                 sizeof(validation_error)) != 0) {
        log_error("invalid network configuration: %s", validation_error);
        return -1;
    }
    metadata_exists = network_info_exists(name);
    if (metadata_exists < 0) {
        log_error("refusing network creation while state is unreadable");
        return -1;
    }
    if (metadata_exists > 0) {
        log_error("network metadata already exists: %s", name);
        return -1;
    }

    //如果已经存在该网络
    if (net_has_exist(name)) {
        log_error("network %s has exists", name);
        return -1;
    }

    //创建网桥
    char *const add_bridge[] = {"brctl", "addbr", name, NULL};
    if (td_run_command(add_bridge) != 0) {
        log_error("failed create new bridge for %s", name);
        return -1;
    }

    //写入网络信息
    if (add_network_info(&network_record) == -1) {
        log_error("failed to save network info: %s", name);
        char *const delete_bridge[] = {"brctl", "delbr", name, NULL};
        (void)td_run_command(delete_bridge);
        return -1;
    }

    //为网桥设置IP地址
    char firs_cidr_ip[32] = {0};
    get_first_cidr_host_ip(cidr_network, firs_cidr_ip, 32);
    char *const set_address[] = {"ip", "addr", "add", firs_cidr_ip,
                                 "dev", name, NULL};
    char *const set_up[] = {"ip", "link", "set", name, "up", NULL};
    if (td_run_command(set_address) != 0 || td_run_command(set_up) != 0) {
        log_error("failed to configure bridge %s", name);
        char *const delete_bridge[] = {"brctl", "delbr", name, NULL};
        if (td_run_command(delete_bridge) == 0) {
            (void)delete_network_info(name);
        } else {
            log_error("bridge rollback failed; keeping network metadata for safe retry: %s",
                      name);
        }
        return -1;
    }
    return 0;
}


int create_default_bridge(void) {
    //如果网桥不存在就创建
    int ret_val = 0;
    if (net_has_exist(TINYDOCKER_DEFAULT_NETWORK_NAME) == 0) {
        log_info("init detail bridge network %s for tinydocker", TINYDOCKER_DEFAULT_NETWORK_NAME);
        ret_val = create_network(TINYDOCKER_DEFAULT_NETWORK_NAME, TINYDOCKER_DEFAULT_NETWORK_CIDR, "bridge");
    } else {
        struct td_network_record existing;
        if (read_network_info(TINYDOCKER_DEFAULT_NETWORK_NAME, &existing) != 0 ||
            strcmp(existing.driver, "bridge") != 0 ||
            strcmp(existing.cidr, TINYDOCKER_DEFAULT_NETWORK_CIDR) != 0) {
            log_error("refusing to adopt an existing default bridge without matching metadata");
            return -1;
        }
    }

    //存在无论如何都启动一把
    char *const set_up[] = {"ip", "link", "set",
                            TINYDOCKER_DEFAULT_NETWORK_NAME, "up", NULL};
    if (td_run_command(set_up) != 0) {
        return -1;
    }

    return ret_val;
}


int delte_network(char *name) {
    struct td_network_record owned_network;
    if (read_network_info(name, &owned_network) != 0) {
        log_error("refusing to delete untracked network interface: %s", name);
        return -1;
    }
    if (if_nametoindex(name) != 0U) {
        char *const set_down[] = {"ip", "link", "set", "dev", name,
                                  "down", NULL};
        char *const delete_bridge[] = {"brctl", "delbr", name, NULL};
        if (td_run_command(set_down) != 0 ||
            td_run_command(delete_bridge) != 0) {
            log_error("failed to remove bridge %s", name);
            return -1;
        }
    }
    return delete_network_info(name);
}

void get_first_cidr_host_ip(char *cidr_network, char *cidr_host_ip, int size) {
    unsigned int minIP;
    unsigned int maxIP;
    unsigned int prefix;
    if (get_cidr_range(cidr_network, &minIP, &maxIP, &prefix) != 0) {
        if (size > 0) {
            cidr_host_ip[0] = '\0';
        }
        return;
    }

    //获取第一个IP
    int_to_str_ip(minIP + 1, cidr_host_ip, size); //加1是忽略0号主机

    size_t used = strlen(cidr_host_ip);
    if (used < (size_t)size) {
        (void)snprintf(cidr_host_ip + used, (size_t)size - used,
                       "/%u", prefix);
    }
}

unsigned alloc_new_ip(char *name, char *ip, int buf_size) {
    struct td_network_record records[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;
    struct td_network_record *network = NULL;
    unsigned int minimum;
    unsigned int maximum;
    unsigned int allocated = 0U;
    int lock_fd;

    if (name == NULL || ip == NULL || buf_size <= 0) {
        return 0U;
    }
    lock_fd = lock_network_state();
    if (lock_fd < 0) {
        return 0U;
    }
    if (load_network_records(records, TD_NETWORK_MAX_RECORDS, &count) != 0) {
        goto cleanup;
    }
    for (size_t index = 0U; index < count; index++) {
        if (strcmp(records[index].name, name) == 0) {
            network = &records[index];
            break;
        }
    }
    if (network == NULL || network->used_ip_count >= TD_NETWORK_MAX_USED_IPS ||
        get_cidr_range(network->cidr, &minimum, &maximum, NULL) != 0 ||
        maximum <= minimum + 2U) {
        goto cleanup;
    }
    for (unsigned int candidate = minimum + 2U; candidate < maximum; candidate++) {
        int used = 0;
        for (size_t index = 0U; index < network->used_ip_count; index++) {
            if (candidate == network->used_ips[index]) {
                used = 1;
                break;
            }
        }
        if (used == 0) {
            allocated = candidate;
            break;
        }
    }
    if (allocated == 0U) {
        goto cleanup;
    }
    network->used_ips[network->used_ip_count++] = allocated;
    if (write_network_records(records, count) != 0) {
        allocated = 0U;
        goto cleanup;
    }
    int_to_str_ip(allocated, ip, buf_size);

cleanup:
    (void)close(lock_fd);
    return allocated;
}


int release_used_ip(char *name, char *ip) {
    struct td_network_record records[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;
    unsigned int address = str_ip_to_int(ip);
    int lock_fd;
    int result = -1;

    if (name == NULL || address == 0U) {
        return -1;
    }
    lock_fd = lock_network_state();
    if (lock_fd < 0) {
        return -1;
    }
    if (load_network_records(records, TD_NETWORK_MAX_RECORDS, &count) != 0) {
        goto cleanup;
    }
    for (size_t record_index = 0U; record_index < count; record_index++) {
        struct td_network_record *network = &records[record_index];
        if (strcmp(network->name, name) == 0) {
            size_t output_index = 0U;
            for (size_t ip_index = 0U; ip_index < network->used_ip_count;
                 ip_index++) {
                if (network->used_ips[ip_index] != address) {
                    network->used_ips[output_index++] = network->used_ips[ip_index];
                }
            }
            network->used_ip_count = output_index;
            result = write_network_records(records, count);
            goto cleanup;
        }
    }
    errno = ENOENT;

cleanup:
    (void)close(lock_fd);
    return result;
}


int list_network(void) {
    struct td_network_record networks[TD_NETWORK_MAX_RECORDS];
    size_t count = 0U;
    if (load_network_records(networks, TD_NETWORK_MAX_RECORDS, &count) != 0) {
        return -1;
    }
    printf("%-10s\t%s\t%-18s\t%s\n", "NAME", "DRIVER", "CIDR", "ALLOC_IPS");
    for (size_t index = 0U; index < count; index++) {
        printf("%-10s\t%s\t%-18s\t", networks[index].name,
               networks[index].driver, networks[index].cidr);
        char ips[4096] = {0};
        size_t used = 0U;
        for (size_t ip_index = 0U; ip_index < networks[index].used_ip_count;
             ip_index++) {
            char str_ips[64] = {0};
            int_to_str_ip(networks[index].used_ips[ip_index], str_ips, 64);
            int written = snprintf(ips + used, sizeof(ips) - used, "%s,", str_ips);
            if (written < 0 || (size_t)written >= sizeof(ips) - used) {
                return -1;
            }
            used += (size_t)written;
        }
        if (used == 0U) {
            (void)snprintf(ips, sizeof(ips), "NULL");
        }
        printf("%s\n", ips);
    }
    return 0;
}

int remove_docker_network(struct docker_network_rm *cmd) {
    int result = 0;
    for (int i = 0; i < cmd->network_argc; i++) {
        if (delte_network(cmd->network_argv[i]) != 0) {
            result = -1;
        }
    }
    return result;
}


int connect_container(char *container_name, char *network, char *ip_addr) {
    //为容器申请新的IP
    char str_ip[64] = {0}; //返回0表示没有申请到有效的IP
    if (alloc_new_ip(network, str_ip, 64) == 0) {
        log_error("failed alloc new ip for container: %s", container_name);
        return -1;
    }
    
    // 找出目标容器中的一个进程ID, 该进程用来寻找ns文件
    int pid_list[4096];
    int pid_cnt = get_container_processes_id(container_name, pid_list,
                                             sizeof(pid_list) / sizeof(pid_list[0]));
    if (pid_cnt <= 0) {
        log_error("failed to get container process list");
        (void)release_used_ip(network, str_ip);
        return -1;
    }
    //默认第一个进程为1号进程, 可能不准, 但是在我们这个简单环境下基本都是它了
    int one_pid = pid_list[0];


    // 两端在宿主 namespace 创建时都使用唯一名字；移动后才重命名为 eth0。
    char *veth_container = "eth0";
    char veth_host[16] = {0};
    char veth_peer[16] = {0};
    char pid_text[32] = {0};
    char container_address[64] = {0};
    if (td_make_veth_name(container_name, veth_host, sizeof(veth_host)) != 0 ||
        td_make_veth_peer_name(container_name, veth_peer,
                               sizeof(veth_peer)) != 0 ||
        snprintf(pid_text, sizeof(pid_text), "%d", one_pid) < 0 ||
        snprintf(container_address, sizeof(container_address), "%s/24", str_ip) < 0) {
        (void)release_used_ip(network, str_ip);
        return -1;
    }

    char *const add_veth[] = {"ip", "link", "add", veth_peer, "type",
                              "veth", "peer", "name", veth_host, NULL};
    char *const add_to_bridge[] = {"brctl", "addif", network, veth_host, NULL};
    char *const host_up[] = {"ip", "link", "set", veth_host, "up", NULL};
    char *const move_peer[] = {"ip", "link", "set", "dev", veth_peer,
                               "netns", pid_text, NULL};
    char *const rename_peer[] = {"nsenter", "-t", pid_text, "-n", "ip",
                                 "link", "set", veth_peer, "name",
                                 veth_container, NULL};
    char *const add_loopback[] = {"nsenter", "-t", pid_text, "-n", "ip",
                                  "addr", "add", "127.0.0.1/8", "dev", "lo", NULL};
    char *const loopback_up[] = {"nsenter", "-t", pid_text, "-n", "ip",
                                 "link", "set", "lo", "up", NULL};
    char *const add_address[] = {"nsenter", "-t", pid_text, "-n", "ip",
                                 "addr", "add", container_address, "dev",
                                 veth_container, NULL};
    char *const container_up[] = {"nsenter", "-t", pid_text, "-n", "ip",
                                  "link", "set", veth_container, "up", NULL};
    char *const add_route[] = {"nsenter", "-t", pid_text, "-n", "ip",
                               "route", "add", "default", "via",
                               TINYDOCKER_DEFAULT_GATEWAY, "dev", veth_container, NULL};

    if (td_run_command(add_veth) != 0 ||
        td_run_command(add_to_bridge) != 0 ||
        td_run_command(host_up) != 0 ||
        td_run_command(move_peer) != 0 ||
        td_run_command(rename_peer) != 0 ||
        td_run_command(add_loopback) != 0 ||
        td_run_command(loopback_up) != 0 ||
        td_run_command(add_address) != 0 ||
        td_run_command(container_up) != 0 ||
        td_run_command(add_route) != 0) {
        char *const delete_veth[] = {"ip", "link", "delete", veth_host, NULL};
        (void)td_run_command(delete_veth);
        (void)release_used_ip(network, str_ip);
        log_error("failed to configure network for container %s", container_name);
        return -1;
    }

    (void)snprintf(ip_addr, 20U, "%s", str_ip);
    return 0;
}

int disconnect_container(char *container_name, char *network) {
    char veth_host[16] = {0};
    if (td_make_veth_name(container_name, veth_host, sizeof(veth_host)) != 0) {
        return -1;
    }
    if (if_nametoindex(veth_host) == 0U) {
        return 0;
    }
    char *const remove_from_bridge[] = {"brctl", "delif", network,
                                        veth_host, NULL};
    char *const delete_veth[] = {"ip", "link", "delete", veth_host, NULL};
    if (td_run_command(remove_from_bridge) != 0 ||
        td_run_command(delete_veth) != 0) {
        log_error("failed to disconnect veth %s", veth_host);
        return -1;
    }
    return 0;
}


int set_container_port_map(char *container_ip, int port_cnt, struct port_map *port_maps) {
    struct in_addr parsed_address;
    if (container_ip == NULL || inet_pton(AF_INET, container_ip, &parsed_address) != 1 ||
        port_cnt < 0 || (port_cnt > 0 && port_maps == NULL)) {
        log_error("invalid port mapping inputs");
        return -1;
    }
    for (int i = 0; i < port_cnt; i++) {
        int host_port = port_maps[i].host_port;
        int container_port = port_maps[i].container_port;
        char host_port_text[8] = {0};
        char destination[64] = {0};
        if (snprintf(host_port_text, sizeof(host_port_text), "%d", host_port) < 0 ||
            snprintf(destination, sizeof(destination), "%s:%d", container_ip,
                     container_port) < 0) {
            return -1;
        }
        char *const add_output[] = {
            "iptables", "-t", "nat", "-A", "OUTPUT", "-p", "tcp",
            "--dport", host_port_text, "-j", "DNAT", "--to-destination",
            destination, NULL
        };
        char *const add_prerouting[] = {
            "iptables", "-t", "nat", "-A", "PREROUTING", "-p", "tcp",
            "-m", "tcp", "--dport", host_port_text, "-j", "DNAT",
            "--to-destination", destination, NULL
        };
        if (td_run_command(add_output) != 0) {
            log_error("failed to add OUTPUT DNAT for port %d", host_port);
            return -1;
        }
        if (td_run_command(add_prerouting) != 0) {
            char *const delete_output[] = {
                "iptables", "-t", "nat", "-D", "OUTPUT", "-p", "tcp",
                "--dport", host_port_text, "-j", "DNAT", "--to-destination",
                destination, NULL
            };
            (void)td_run_command(delete_output);
            log_error("failed to add PREROUTING DNAT for port %d", host_port);
            return -1;
        }
        log_info("set host port %d map to container port %d successful", host_port, container_port);
    }
    return 0;
}

static int delete_dnat_rules_for_chain(const char *chain, const char *container_ip) {
    char *output = NULL;
    size_t output_size = 0U;
    char destination_prefix[64] = {0};
    char *list_arguments[] = {"iptables", "-t", "nat", "-S", (char *)chain, NULL};
    int result = 0;

    if (snprintf(destination_prefix, sizeof(destination_prefix), "%s:",
                 container_ip) < 0 ||
        td_capture_command(list_arguments, &output, &output_size) != 0) {
        free(output);
        return -1;
    }
    (void)output_size;
    char *save_line = NULL;
    char *line = strtok_r(output, "\n", &save_line);
    while (line != NULL) {
        char *tokens[32] = {0};
        size_t token_count = 0U;
        char *save_token = NULL;
        char *token = strtok_r(line, " \t", &save_token);
        int matches_destination = 0;

        while (token != NULL && token_count < 32U) {
            tokens[token_count++] = token;
            token = strtok_r(NULL, " \t", &save_token);
        }
        for (size_t index = 0U; index + 1U < token_count; index++) {
            if (strcmp(tokens[index], "--to-destination") == 0 &&
                strncmp(tokens[index + 1U], destination_prefix,
                        strlen(destination_prefix)) == 0) {
                matches_destination = 1;
                break;
            }
        }
        if (matches_destination != 0 && token_count >= 2U &&
            strcmp(tokens[0], "-A") == 0 && strcmp(tokens[1], chain) == 0) {
            char *delete_arguments[36] = {
                "iptables", "-t", "nat", "-D", (char *)chain, NULL
            };
            size_t delete_count = 5U;
            for (size_t index = 2U; index < token_count && delete_count < 35U;
                 index++) {
                delete_arguments[delete_count++] = tokens[index];
            }
            delete_arguments[delete_count] = NULL;
            if (delete_count != token_count + 3U ||
                td_run_command(delete_arguments) != 0) {
                result = -1;
            }
        }
        line = strtok_r(NULL, "\n", &save_line);
    }
    free(output);
    return result;
}

int unset_container_port_map(char *container_ip) {
    struct in_addr parsed_address;
    if (container_ip == NULL || container_ip[0] == '\0') {
        return 0;
    }
    if (inet_pton(AF_INET, container_ip, &parsed_address) != 1) {
        log_error("refusing to remove port mappings for invalid IP: %s", container_ip);
        return -1;
    }
    int output_result = delete_dnat_rules_for_chain("OUTPUT", container_ip);
    int prerouting_result = delete_dnat_rules_for_chain("PREROUTING", container_ip);
    if (output_result != 0 || prerouting_result != 0) {
        log_error("failed to remove all DNAT rules for %s", container_ip);
        return -1;
    }
    log_info("removed container port mappings for %s", container_ip);
    return 0;
}
