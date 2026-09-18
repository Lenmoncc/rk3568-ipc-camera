/** @file test_config_api.c @brief 验证加载失败不会部分覆盖调用方配置对象。 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    IpcConfig config, before;
    char path[] = "/tmp/ipc-config-api-XXXXXX";
    const char invalid[] = "unknown=1\n";
    int fd, failed = 0;
    if (argc != 2 || ipc_config_load(argv[1], &config) != 0 || ipc_config_validate(&config) != 0)
        return 1;
    fd = mkstemp(path);
    if (fd < 0)
        return 1;
    if (write(fd, invalid, sizeof(invalid) - 1) != (ssize_t)(sizeof(invalid) - 1))
        failed = 1;
    if (close(fd) != 0)
        failed = 1;
    memset(&config, 0xa5, sizeof(config));
    memcpy(&before, &config, sizeof(config));
    if (!failed && (ipc_config_load(path, &config) >= 0 || memcmp(&before, &config, sizeof(config)) != 0))
        failed = 1;
    unlink(path);
    if (failed)
        return 1;
    puts("PASS: failed load preserves the caller's configuration.");
    return 0;
}
