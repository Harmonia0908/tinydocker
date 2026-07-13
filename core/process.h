#ifndef TINYDOCKER_CORE_PROCESS_H
#define TINYDOCKER_CORE_PROCESS_H

#include <stddef.h>

int td_run_command(char *const arguments[]);
int td_capture_command(char *const arguments[], char **output,
                       size_t *output_size);

#endif
