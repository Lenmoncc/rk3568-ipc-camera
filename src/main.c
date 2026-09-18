/**
 * @file main.c
 * @brief 配置与日志阶段入口；仅完成参数处理、配置校验和摘要输出。
 *
 * 当前没有创建采集/编码/输出线程。即使配置启用 RTMP，也只检查 URL，
 * 不连接服务器。后续媒体实现必须在本阶段检查完成后再接入。
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

static void print_usage(FILE *output, const char *program)
{
    fprintf(output,
            "Usage: %s [-c FILE] [--check-config]\n"
            "  -c, --config FILE  Config path (default: configs/ipc.conf from current directory)\n"
            "      --check-config Validate and print config, then exit\n"
            "  -h, --help         Show this help without loading config\n"
            "Current stage: configuration/logging only; no devices, network or recording.\n",
            program);
}

int main(int argc, char **argv)
{
    const char *config_path = "configs/ipc.conf";
    bool check_only = false;
    IpcConfig config;
    int option;
    const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"check-config", no_argument, NULL, 'k'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    /* 统一由本程序报告参数错误，避免 getopt 和日志重复输出。 */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "c:h", options, NULL)) != -1) {
        switch (option) {
        case 'c': config_path = optarg; break;
        case 'k': check_only = true; break;
        case 'h': print_usage(stdout, argv[0]); return EXIT_SUCCESS;
        default:
            ipc_log_write(IPC_LOG_ERROR, "main", "unknown option or missing option value");
            print_usage(stderr, argv[0]);
            return 2;
        }
    }
    if (optind != argc) {
        ipc_log_write(IPC_LOG_ERROR, "main", "unexpected positional argument: %s", argv[optind]);
        return 2;
    }
    ipc_log_write(IPC_LOG_DEBUG, "main", "loading configuration from %s", config_path);
    if (ipc_config_load(config_path, &config) < 0)
        return EXIT_FAILURE;

    /* 加载前使用默认 INFO 阈值；新等级只在整个配置有效后生效。 */
    ipc_log_set_level(config.log_level);
    ipc_log_write(IPC_LOG_DEBUG, "main", "configuration source: %s", config_path);
    ipc_config_dump(&config);
    if (!config.rtmp_enabled)
        ipc_log_write(IPC_LOG_WARN, "main", "RTMP is disabled; an empty or placeholder URL is permitted");
    ipc_log_write(IPC_LOG_INFO, "main", "configuration validation passed; hardware parameters were not probed");
    if (!check_only)
        ipc_log_write(IPC_LOG_INFO, "main", "configuration stage complete; raw frame queue is available via test_frame_queue; capture, packet queues, encoding and outputs are NOT IMPLEMENTED");
    return EXIT_SUCCESS;
}
