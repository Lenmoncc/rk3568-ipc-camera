/**
 * @file main.c
 * @brief 主程序：默认检查配置，显式选择视频采集、视频编码或音频采集模式。
 *
 * 视频输出 H.264，音频独立输出 PCM 或 AAC；可并行录制 MP4；尚不连接服务器。
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"
#include "video_pipeline.h"
#include "video_encoder.h"
#include "audio_pipeline.h"
#include "audio_encoder.h"
#include "record_pipeline.h"
#include "alsa_capture.h"
#include <errno.h>
#include <limits.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

/** @brief 输出命令行说明，帮助模式不读取配置或打开设备。 */
static void print_usage(FILE *output, const char *program)
{
    fprintf(output,
            "Usage: %s [-c FILE] [--check-config | --capture | --encode | --audio-capture | --audio-encode | --record] [options]\n"
            "  -c, --config FILE  Config path (default: configs/ipc.conf from current directory)\n"
            "      --check-config Validate and print config, then exit\n"
            "  -h, --help         Show this help without loading config\n"
            "      --capture     Capture real video through the raw frame queue\n"
            "      --encode      Capture and encode with Rockchip MPP H.264\n"
            "      --output PATH Create a NEW Annex B H.264 file (required for --encode)\n"
            "      --encode-fps N  Encoder nominal rate 1..30 (default config video.fps)\n"
            "      --frames N    Valid capture limit (default 300; 0 until Ctrl+C)\n"
            "      --dump PATH   Create a NEW packed NV12 file (never overwrite)\n"
            "      --dump-frames N  Save at most N consumed frames (default 60)\n"
            "      --consumer-delay-ms N  Simulate slow consumption (0..1000)\n"
            "      --audio-capture Capture PCM through the audio raw queue\n"
            "      --pcm PATH     Create a NEW S16_LE PCM file (required for --audio-capture)\n"
            "      --seconds N    Audio/record length 0..86400 (default audio 10, record 30; 0 until Ctrl+C)\n"
            "      --audio-encode Capture and encode AAC-LC (requires --aac PATH)\n"
            "      --aac PATH     Create a NEW ADTS AAC file\n"
            "      --record      Record H.264 + AAC to MP4 (default 30 seconds)\n"
            "      --mp4 PATH    NEW MP4 path (default config output.record_path)\n"
            "No RTMP yet; board-local playback uses the scripts.\n",
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

/** @brief 解析参数、加载配置，然后选择配置检查、视频或音频模式；各模式参数严格隔离。 */
int main(int argc, char **argv)
{
    const char *config_path = "configs/ipc.conf";
    bool check_only = false, capture = false, encode = false, audio = false;
    bool capture_option = false, dump_count_set = false, encode_option = false;
    bool audio_option = false, delay_set = false, audio_encode = false, record = false;
    bool seconds_set = false, fps_set = false;
    IpcRecordOptions record_run = {.seconds = 30};
    IpcAudioRunOptions audio_run = {.seconds = 10};
    IpcVideoRunOptions run = {.frames = 300, .dump_path = NULL, .dump_frames = 60, .consumer_delay_ms = 0};
    IpcConfig config;
    int option;
    const struct option options[] = {
        {"config", required_argument, NULL, 'c'},
        {"check-config", no_argument, NULL, 'k'},
        {"help", no_argument, NULL, 'h'},
        {"capture", no_argument, NULL, 'v'},
        {"encode", no_argument, NULL, 'e'},
        {"audio-capture", no_argument, NULL, 'a'},
        {"audio-encode", no_argument, NULL, 'A'},
        {"record", no_argument, NULL, 'R'},
        {"mp4", required_argument, NULL, 'M'},
        {"aac", required_argument, NULL, 'O'},
        {"pcm", required_argument, NULL, 'p'},
        {"seconds", required_argument, NULL, 't'},
        {"output", required_argument, NULL, 'o'},
        {"encode-fps", required_argument, NULL, 'f'},
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
        case 'e': encode = true; break;
        case 'a': audio = true; break;
        case 'A': audio_encode = true; break;
        case 'R': record = true; break;
        case 'M':
            record_run.mp4_path = optarg;
            if (!*optarg) goto bad_value;
            break;
        case 'O':
            audio_option = true;
            audio_run.aac_path = optarg;
            if (!*optarg) goto bad_value;
            break;
        case 'p':
            audio_option = true;
            audio_run.pcm_path = optarg;
            if (!*optarg) goto bad_value;
            break;
        case 't':
            seconds_set = true;
            if (parse_unsigned(optarg, &audio_run.seconds) < 0 || audio_run.seconds > 86400) goto bad_value;
            break;
        case 'o':
            encode_option = true;
            run.h264_path = optarg;
            if (*optarg == '\0') goto bad_value;
            break;
        case 'f':
            fps_set = true;
            if (parse_unsigned(optarg, &run.encode_fps) < 0 || run.encode_fps < 1 || run.encode_fps > 30) goto bad_value;
            break;
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
            delay_set = true;
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
    if ((unsigned int)capture + encode + audio + audio_encode + record + check_only > 1 ||
        (audio_option && !audio && !audio_encode) || (audio && !audio_run.pcm_path) ||
        (audio_run.pcm_path && !audio) || (audio_run.aac_path && !audio_encode) ||
        (audio_encode && !audio_run.aac_path) ||
        (delay_set && !capture && !encode && !audio && !audio_encode && !record) ||
        (capture_option && !capture && !encode) || (dump_count_set && run.dump_path == NULL) ||
        (encode_option && !encode) || (fps_set && !encode && !record) ||
        (seconds_set && !audio && !audio_encode && !record) || (record_run.mp4_path && !record) || (encode && run.h264_path == NULL)) {
        ipc_log_write(IPC_LOG_ERROR, "main", "invalid mode/options; --encode requires --output; --audio-capture requires --pcm; --audio-encode requires --aac");
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
    if (encode && !ipc_video_encoder_available()) {
        ipc_log_write(IPC_LOG_ERROR, "main", "MPP support disabled; rebuild with WITH_MPP=1 using the board SDK");
        return EXIT_FAILURE;
    }
    if (record) {
        if (seconds_set) record_run.seconds = audio_run.seconds;
        record_run.encode_fps = run.encode_fps;
        record_run.output_delay_ms = run.consumer_delay_ms;
        return ipc_record_pipeline_run(&config, &record_run);
    }
    if (capture || encode) return ipc_video_pipeline_run(&config, &run);
    if (audio || audio_encode) {
        if (audio_encode && !ipc_audio_encoder_available()) {
            ipc_log_write(IPC_LOG_ERROR, "main", "FFmpeg support disabled; rebuild with WITH_FFMPEG=1 using the board SDK");
            return EXIT_FAILURE;
        }
        if (!ipc_audio_capture_available()) {
            ipc_log_write(IPC_LOG_ERROR, "main", "ALSA support disabled; rebuild with WITH_ALSA=1 using the board SDK");
            return EXIT_FAILURE;
        }
        audio_run.consumer_delay_ms = run.consumer_delay_ms;
        return ipc_audio_pipeline_run(&config, &audio_run);
    }
    if (!check_only)
        ipc_log_write(IPC_LOG_INFO, "main", "configuration stage complete; use --capture, --encode or --audio-capture/--audio-encode; use --record for MP4; RTMP is NOT IMPLEMENTED");
    return EXIT_SUCCESS;
bad_value:
    ipc_log_write(IPC_LOG_ERROR, "main", "invalid option value: %s", optarg ? optarg : "");
    return 2;
}
