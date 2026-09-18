/**
 * @file main.c
 * @brief 主程序：默认检查配置，显式 --capture 才运行真实视频采集。
 *
 * 采集模式仅包含视频采集与消费线程；尚不编码、连接服务器或创建 MP4。
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"
#include "video_pipeline.h"
#include <errno.h>
#include <limits.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief 输出命令行说明，帮助模式不读取配置或打开设备。 */
static void print_usage(FILE *output, const char *program)
{
    fprintf(output,
            "Usage: %s [-c FILE] [--check-config | --capture [options]]\n"
            "  -c, --config FILE  Config path (default: configs/ipc.conf from current directory)\n"
            "      --check-config Validate and print config, then exit\n"
            "  -h, --help         Show this help without loading config\n"
            "      --capture     Capture real video through the raw frame queue\n"
            "      --frames N    Valid capture limit (default 300; 0 until Ctrl+C)\n"
            "      --dump PATH   Create a NEW packed NV12 file (never overwrite)\n"
            "      --dump-frames N  Save at most N consumed frames (default 60)\n"
            "      --consumer-delay-ms N  Simulate slow consumption (0..1000)\n"
            "No audio, encoding, RTMP or MP4 in this stage.\n",
            program);
}

/** @brief 严格解析十进制非负整数，拒绝符号、尾随字符及溢出。 */
static int parse_unsigned(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;
    if (text == NULL || *text == '\0') return -1;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > UINT_MAX) return -1;
    *value = (unsigned int)parsed;
    return 0;
}

/** @brief 解析参数、加载配置，然后选择配置检查或显式视频采集模式。 */
int main(int argc, char **argv)
{
    const char *config_path = "configs/ipc.conf";
    bool check_only = false, capture = false, capture_option = false, dump_count_set = false;
    IpcVideoRunOptions run = {.frames = 300, .dump_path = NULL, .dump_frames = 60, .consumer_delay_ms = 0};
    IpcConfig config;
    int option;
    const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"check-config", no_argument, NULL, 'k'},
        {"help", no_argument, NULL, 'h'},
        {"capture", no_argument, NULL, 'v'},
        {"frames", required_argument, NULL, 'n'},
        {"dump", required_argument, NULL, 'd'},
        {"dump-frames", required_argument, NULL, 's'},
        {"consumer-delay-ms", required_argument, NULL, 'w'},
        {NULL, 0, NULL, 0}
    };

    /* 统一由本程序报告参数错误，避免 getopt 和日志重复输出。 */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "c:h", options, NULL)) != -1) {
        switch (option) {
        case 'c': config_path = optarg; break;
        case 'k': check_only = true; break;
        case 'v': capture = true; break;
        case 'n':
            capture_option = true;
            if (parse_unsigned(optarg, &run.frames) < 0) goto bad_value;
            break;
        case 'd':
            capture_option = true;
            run.dump_path = optarg;
            if (*optarg == '\0') goto bad_value;
            break;
        case 's':
            capture_option = dump_count_set = true;
            if (parse_unsigned(optarg, &run.dump_frames) < 0 || run.dump_frames == 0) goto bad_value;
            break;
        case 'w':
            capture_option = true;
            if (parse_unsigned(optarg, &run.consumer_delay_ms) < 0 || run.consumer_delay_ms > 1000) goto bad_value;
            break;
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
    if ((capture && check_only) || (capture_option && !capture) || (dump_count_set && run.dump_path == NULL)) {
        ipc_log_write(IPC_LOG_ERROR, "main", "invalid combination of capture/check/dump options");
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
    if (capture) return ipc_video_pipeline_run(&config, &run);
    if (!check_only)
        ipc_log_write(IPC_LOG_INFO, "main", "configuration stage complete; use --capture for video/queue verification; audio, packet queues, encoding and outputs are NOT IMPLEMENTED");
    return EXIT_SUCCESS;
bad_value:
    ipc_log_write(IPC_LOG_ERROR, "main", "invalid capture option value: %s", optarg ? optarg : "");
    return 2;
}
