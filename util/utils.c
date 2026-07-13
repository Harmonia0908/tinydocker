#define _XOPEN_SOURCE 500
#include <sys/stat.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <limits.h>
#include <sys/stat.h>
#include "utils.h"
#include "../logger/log.h"
#include "../core/fs.h"
#include "../core/process.h"
#include "../core/safety.h"



off_t filesize(const char *filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

#define BUFFER_SIZE 1024

char* calculate_sha256(const char* file_path) {
    FILE* file = fopen(file_path, "rb");
    if (!file) {
        log_error("无法打开文件: %s", file_path);
        return NULL;
    }

    EVP_MD_CTX* md_context = EVP_MD_CTX_new();
    if (!md_context) {
        log_error("无法创建哈希上下文");
        fclose(file);
        return NULL;
    }

    if (EVP_DigestInit_ex(md_context, EVP_sha256(), NULL) != 1) {
        log_error("无法初始化哈希算法");
        EVP_MD_CTX_free(md_context);
        fclose(file);
        return NULL;
    }

    unsigned char buffer[BUFFER_SIZE];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) != 0) {
        if (EVP_DigestUpdate(md_context, buffer, bytes_read) != 1) {
            log_error("无法更新哈希值");
            EVP_MD_CTX_free(md_context);
            fclose(file);
            return NULL;
        }
    }

    unsigned char sha256_hash[EVP_MAX_MD_SIZE];
    unsigned int sha256_hash_len;

    if (EVP_DigestFinal_ex(md_context, sha256_hash, &sha256_hash_len) != 1) {
        log_error("无法计算哈希值");
        EVP_MD_CTX_free(md_context);
        fclose(file);
        return NULL;
    }

    EVP_MD_CTX_free(md_context);
    fclose(file);

    char* sha256_string = (char*)malloc(sha256_hash_len * 2 + 1);
    if (!sha256_string) {
        log_error("内存分配失败");
        return NULL;
    }

    for (int i = 0; i < sha256_hash_len; i++) {
        sprintf(&sha256_string[i * 2], "%02x", (unsigned int)sha256_hash[i]);
    }

    return sha256_string;
}


int path_exist(const char *path) {
    if (access(path, F_OK) == 0)
        return 1;
    return 0;
}


int remove_dir(char *path) {
    return td_remove_tree(path);
}


int make_path(const char *dir) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp),"%s",dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (!path_exist(tmp) && mkdir(tmp, S_IRWXU) == -1) {
                return -1;
            }
            *p = '/';
        }
    }
    return path_exist(tmp) ? 0 : mkdir(tmp, S_IRWXU);
}

int is_folder(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;
    return 0;
}


char** split_string(char* input) {
    char** result = 0;
    size_t count = 0;
    char* tmp = input;
    char* last_comma = 0;
    char delim[2] = " ";

    /* Count how many elements will be in the array */
    while (*tmp) {
        if (delim[0] == *tmp) {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    /* Add space for trailing token */
    count += last_comma < (input + strlen(input) - 1);

    /* Add space for terminating null string so caller
       knows where the list of returned strings ends. */
    count++;
    result = malloc(sizeof(char*) * count);

    if (result) {
        size_t idx  = 0;
        char* token = strtok(input, delim);

        while (token) {
            *(result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
        *(result + idx) = 0;
    }
    return result;
}

int create_tar(char *dir, char *tar_path) {
    char *const arguments[] = {"tar", "-czf", tar_path, "-C", dir, ".", NULL};
    log_info("create archive %s from %s without a shell", tar_path, dir);
    return td_run_command(arguments);
}


int extract_tar(const char* tar_file, const char* extract_dir) {
    char *listing = NULL;
    size_t listing_size = 0U;
    char *const list_arguments[] = {"tar", "-tf", (char *)tar_file, NULL};
    if (td_capture_command(list_arguments, &listing, &listing_size) != 0) {
        free(listing);
        log_error("failed to list archive before extraction: %s", tar_file);
        return -1;
    }
    (void)listing_size;
    char *save = NULL;
    char *entry = strtok_r(listing, "\n", &save);
    while (entry != NULL) {
        if (td_archive_entry_is_safe(entry) == 0) {
            log_error("refusing unsafe archive entry: %s", entry);
            free(listing);
            return -1;
        }
        entry = strtok_r(NULL, "\n", &save);
    }
    free(listing);

    char *const arguments[] = {"tar", "-xf", (char *)tar_file, "-C",
                               (char *)extract_dir, "--no-same-owner",
                               "--no-same-permissions", NULL};
    log_info("extract archive %s into %s without a shell", tar_file, extract_dir);
    return td_run_command(arguments);
}


void timestamp_to_string(time_t timestamp, char *buffer, size_t buffer_size) {
    struct tm *timeinfo;
    timeinfo = localtime(&timestamp);

    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", timeinfo);
}
