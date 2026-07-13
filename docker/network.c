#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdint.h>
#include "network.h"
#include "cgroup.h"
#include "status_info.h"
#include "../cmdparser/cmdparser.h"
#include "../logger/log.h"
#include "../core/process.h"
#include "../core/safety.h"


//IP地址转主机序整数
static unsigned int str_ip_to_int(char *ip) {
    struct in_addr addr;
    addr.s_addr = inet_addr(ip);
    inet_aton(ip, &addr);
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


static int write_network_info(struct network nw) {
    FILE* file = fopen(CONTAINER_NETWORKS_FILE, "a");
    if (file == NULL) {
        log_error("failed to open %s, error: %s", CONTAINER_NETWORKS_FILE, strerror(errno));
        return -1;
    }

    //name:driver:cidr:ip1;ip2;ip3
    char buf[4096] = {0};
    sprintf(buf, "%s:%s:%s:", nw.name, nw.driver, nw.cidr);
    for (int i = 0; i < nw.used_ip_cnt; i++) {
        char int_str[32] = {0};
        sprintf(int_str, "%u;", nw.used_ips[i]);
        strcat(buf, int_str);
    }
    //log_info("write network info: %s", buf);
    int ret = fprintf(file, "%s\n", buf);
    fclose(file);
    return ret > 0 ? 0 : -1;
}  


static int get_network_list(struct network *nw_buffer, int bufsize) {
    FILE* file = fopen(CONTAINER_NETWORKS_FILE, "r");
    if (file == NULL) {
        log_error("failed to open %s, error: %s", CONTAINER_NETWORKS_FILE, strerror(errno));
        return -1;
    }

    //name:driver:cidr:ip1;ip2;ip3
    int line_cnt = 0;
    char line[4096] = {0};
    while (fgets(line, sizeof(line), file) != NULL) {
        struct network nw;
        nw.name = (char *) malloc(64);
        nw.driver = (char *) malloc(64);
        nw.cidr = (char *) malloc(64);
        nw.used_ips = (unsigned *) malloc(sizeof(unsigned) * 128);

        line[strcspn(line, "\n")] = '\0';
        char* token = strtok(line, ":");
        strcpy(nw.name, token);
        token = strtok(NULL, ":");
        strcpy(nw.driver, token);
        token = strtok(NULL, ":");
        strcpy(nw.cidr, token);


        char *ip_list = strtok(NULL, ":");

        //printf("read %s |: %s %s %s %s\n", line, nw.name, nw.driver, nw.cidr, ip_list);
        char *ip_token = strtok(ip_list, ";");
        int ip_cnt = 0;
        while (ip_token != NULL) {
            nw.used_ips[ip_cnt++] = (unsigned) atol(ip_token);
            ip_token = strtok(NULL, ";");
        }
        nw.used_ip_cnt = ip_cnt;
        nw_buffer[line_cnt++] = nw;
        if (line_cnt == bufsize) {
            break;
        }
        memset(line, 0, 4096);
    }
    fclose(file);

    //printf("list end, get %d lines\n", line_cnt);
    return line_cnt;
}


static int read_network_info(char *name, struct network *nw) {
    int bufsize = 128;
    struct network *nw_buffer = malloc(sizeof(struct network) * (size_t)bufsize);
    int size = get_network_list(nw_buffer, bufsize);
    int ok = -1;
    for (int i = 0; i < size; i++) {
        if (strcmp(nw_buffer[i].name, name) == 0) {
            memcpy(nw, &nw_buffer[i], sizeof(struct network));
            ok = 0;
            break;
        }
    }
    return ok;
}


static int delete_network_info(char *name) {
    int bufsize = 128;
    struct network *nw_buffer = malloc(sizeof(struct network) * (size_t)bufsize);
    if (nw_buffer == NULL) {
        log_error("failed alloc network buffer");
        return -1;
    }
    int size = get_network_list(nw_buffer, bufsize);

    // 打开文件以清空内容
    FILE *file = fopen(CONTAINER_NETWORKS_FILE, "w");
    if (file == NULL) {
        log_error("failed update network info, can not open %s", CONTAINER_NETWORKS_FILE);
        free(nw_buffer);
        return -1;
    }
    fclose(file);

    // 重新写入数据到文件
    for (int i = 0; i < size; i++) {
        if (strcmp(nw_buffer[i].name, name) == 0) {
            continue;
        }
        if (write_network_info(nw_buffer[i]) == -1) {
            log_error("failed update network info");
            free(nw_buffer);
            return -1;
        }
    }
    free(nw_buffer);
    return 0;
}


static int net_has_exist(char *brname) {
    return if_nametoindex(brname) == 0U ? 0 : 1;
}

int create_network(char *name, char *cidr_network, char *driver) {
    // 暂时只支持网桥
    if (strcmp(driver, "bridge") != 0) {
        return -1;
    }

    //如果已经存在该网络
    if (net_has_exist(name)) {
        log_error("network %s has exists", name);
        return -1;
    }

    struct network nw = {
        .name = name,
        .driver = driver,
        .cidr = cidr_network,
        .used_ips = NULL,
        .used_ip_cnt = 0
    };

    //创建网桥
    char *const add_bridge[] = {"brctl", "addbr", name, NULL};
    if (td_run_command(add_bridge) != 0) {
        log_error("failed create new bridge for %s", name);
        return -1;
    }

    //写入网络信息
    if (write_network_info(nw) == -1) {
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
        (void)td_run_command(delete_bridge);
        (void)delete_network_info(name);
        return -1;
    }
    return 0;
}


static int update_network_info(struct network *nw) {
    int bufsize = 128;
    struct network *nw_buffer = malloc(sizeof(struct network) * (size_t)bufsize);
    if (nw_buffer == NULL) {
        log_error("failed alloc network buffer");
        return -1;
    }
    int size = get_network_list(nw_buffer, bufsize);

     // 打开文件以清空内容
    FILE *file = fopen(CONTAINER_NETWORKS_FILE, "w");
    if (file == NULL) {
        log_error("failed update network info, can not open %s", CONTAINER_NETWORKS_FILE);
        free(nw_buffer);
        return -1;
    }
    fclose(file);

    // 重新写入数据到文件
    for (int i = 0; i < size; i++) {
        if (strcmp(nw_buffer[i].name, nw->name) == 0) {
            nw_buffer[i] = *nw; //用新的覆盖旧的
        }
        if (write_network_info(nw_buffer[i]) == -1) {
            log_error("failed update network info");
            free(nw_buffer);
            return -1;
        }
    }
    free(nw_buffer);
    return 0;
}

int create_default_bridge(void) {
    //如果网桥不存在就创建
    int ret_val = 0;
    if (net_has_exist(TINYDOCKER_DEFAULT_NETWORK_NAME) == 0) {
        log_info("init detail bridge network %s for tinydocker", TINYDOCKER_DEFAULT_NETWORK_NAME);
        ret_val = create_network(TINYDOCKER_DEFAULT_NETWORK_NAME, TINYDOCKER_DEFAULT_NETWORK_CIDR, "bridge");
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
    struct network nw;
    if (read_network_info(name, &nw) == -1) {
        log_error("failed to read newwork info of %s", name);
        return 0;
    }

    unsigned int minIP;
    unsigned int maxIP;
    if (get_cidr_range(nw.cidr, &minIP, &maxIP, NULL) != 0) {
        return 0;
    }
    //分配IP地址ID时候排除最小的IP和最大的IP和已经被分配的IP
    unsigned int_ip = 0;
    for (unsigned i = minIP + 2; i < maxIP - 1; i++) { //加2开始时为了避免分配主机号0,1和255, 1是这里被设置为网桥地址
        int used = 0;
        for (int j = 0; j < nw.used_ip_cnt; j++) {
            if (i == nw.used_ips[j]) {
                used = 1;
            }
        }
        if (used == 0) {
            int_ip = i;
            break;
        }
    }

    nw.used_ips[nw.used_ip_cnt++] = int_ip;
    if (update_network_info(&nw) == -1) {
        log_info("failed to update network info: %s", nw.name);
    }
    
    if (int_ip > 0) { //如果拿到了有效IP, 那么就做地址转换
        int_to_str_ip(int_ip, ip, buf_size);
    }

    return int_ip;
}


int release_used_ip(char *name, char *ip) {
    struct network nw;
    if (read_network_info(name, &nw) == -1) {
        log_error("failed to read network info of %s", name);
        return -1;
    }

    if (nw.used_ip_cnt < 0) {
        return -1;
    }
    unsigned *old_ips = malloc(sizeof(unsigned) * (size_t)nw.used_ip_cnt);
    for (int i = 0; i < nw.used_ip_cnt; i++) {
        old_ips[i] = nw.used_ips[i];
    }

    unsigned int_ip = str_ip_to_int(ip);
    int new_size = 0;
    for (int i = 0; i < nw.used_ip_cnt; i++) {
        if (old_ips[i] != int_ip) {  //回写是排除当前释放的IP地址
            nw.used_ips[new_size++] = old_ips[i];
        }
    }
    nw.used_ip_cnt = new_size;

    free(old_ips);

    int ret = update_network_info(&nw);
    if (ret == -1) {
        log_info("failed to update network info: %s", nw.name);
    }
    
    return ret;
}


int list_network(void) {
    struct network nw_buffer[100];
    int cnt = get_network_list(nw_buffer, 100);
    if (cnt < 0) {
        return -1;
    }
    printf("%-10s\t%s\t%-18s\t%s\n", "NAME", "DRIVER", "CIDR", "ALLOC_IPS");
    for (int i = 0; i < cnt; i++) {
        printf("%-10s\t%s\t%-18s\t", nw_buffer[i].name, nw_buffer[i].driver, nw_buffer[i].cidr);
        char ips[4096] = {0};
        for (int j = 0; j < nw_buffer[i].used_ip_cnt; j++) {
            char str_ips[64] = {0};
            int_to_str_ip(nw_buffer[i].used_ips[j], str_ips, 64);
            strcat(ips, str_ips);
            strcat(ips, ",");
        }
        if (strlen(ips) == 0) {
            strcpy(ips, "NULL");
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
        return -1;
    }
    //默认第一个进程为1号进程, 可能不准, 但是在我们这个简单环境下基本都是它了
    int one_pid = pid_list[0];


    //生成veth的名字
    char *veth_container = "eth0";
    char veth_host[16] = {0};
    char pid_text[32] = {0};
    char container_address[64] = {0};
    if (td_make_veth_name(container_name, veth_host, sizeof(veth_host)) != 0 ||
        snprintf(pid_text, sizeof(pid_text), "%d", one_pid) < 0 ||
        snprintf(container_address, sizeof(container_address), "%s/24", str_ip) < 0) {
        (void)release_used_ip(network, str_ip);
        return -1;
    }

    char *const add_veth[] = {"ip", "link", "add", veth_container, "type",
                              "veth", "peer", "name", veth_host, NULL};
    char *const add_to_bridge[] = {"brctl", "addif", network, veth_host, NULL};
    char *const host_up[] = {"ip", "link", "set", veth_host, "up", NULL};
    char *const move_peer[] = {"ip", "link", "set", "dev", veth_container,
                               "netns", pid_text, NULL};
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

    strcpy(ip_addr, str_ip);
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
