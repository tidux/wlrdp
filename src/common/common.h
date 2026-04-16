#ifndef WLRDP_COMMON_H
#define WLRDP_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define WLRDP_LOG_INFO(fmt, ...) \
    fprintf(stderr, "[wlrdp] INFO: " fmt "\n", ##__VA_ARGS__)
#define WLRDP_LOG_WARN(fmt, ...) \
    fprintf(stderr, "[wlrdp] WARN: " fmt "\n", ##__VA_ARGS__)
#define WLRDP_LOG_ERROR(fmt, ...) \
    fprintf(stderr, "[wlrdp] ERROR: " fmt "\n", ##__VA_ARGS__)

#define WLRDP_DEFAULT_PORT 3389
#define WLRDP_DEFAULT_WIDTH 1920
#define WLRDP_DEFAULT_HEIGHT 1080

#endif /* WLRDP_COMMON_H */
