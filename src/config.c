/**
 * @file config.c
 * @brief 用统一字段表完成键值解析、范围检查和跨字段校验。
 *
 * 解析先写入局部候选对象，全部通过后才交付调用者，避免失败配置部分生效。
 * 本模块不创建录像文件、不打开摄像头/声卡、不连接 RTMP 服务器。
 */
#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPC_CONFIG_LINE_MAX 1023U

typedef enum { FIELD_STRING, FIELD_UINT, FIELD_SIZE, FIELD_BOOL, FIELD_LEVEL } FieldType;
typedef struct {
    const char *key;
    FieldType type;
    size_t offset;
    size_t width;
    uintmax_t minimum;
    uintmax_t maximum;
    bool required;
} ConfigField;

#define STR_FIELD(key, member) \
    {key, FIELD_STRING, offsetof(IpcConfig, member), sizeof(((IpcConfig *)0)->member), 0, 0, true}
#define NUM_FIELD(key, member, kind, lo, hi) \
    {key, kind, offsetof(IpcConfig, member), sizeof(((IpcConfig *)0)->member), lo, hi, true}

/* 上下界是初版应用策略，不能据此宣称声卡或 MPP 一定支持所有值。
 * 原始帧容量限制较小，避免 720P 图像队列占用过多内存。 */
static const ConfigField g_fields[] = {
    STR_FIELD("video.device", video_device),
    NUM_FIELD("video.width", video_width, FIELD_UINT, 1280, 1280),
    NUM_FIELD("video.height", video_height, FIELD_UINT, 720, 720),
    STR_FIELD("video.pixel_format", video_pixel_format),
    STR_FIELD("video.codec", video_codec),
    NUM_FIELD("video.fps", video_fps, FIELD_UINT, 1, 30),
    NUM_FIELD("video.bitrate", video_bitrate, FIELD_UINT, 64000, 20000000),
    NUM_FIELD("video.gop", video_gop, FIELD_UINT, 1, 300),
    STR_FIELD("audio.device", audio_device),
    STR_FIELD("audio.sample_format", audio_sample_format),
    STR_FIELD("audio.codec", audio_codec),
    NUM_FIELD("audio.sample_rate", audio_sample_rate, FIELD_UINT, 7350, 96000),
    NUM_FIELD("audio.channels", audio_channels, FIELD_UINT, 1, 2),
    NUM_FIELD("audio.bitrate", audio_bitrate, FIELD_UINT, 16000, 320000),
    {"output.rtmp_enabled", FIELD_BOOL, offsetof(IpcConfig, rtmp_enabled), sizeof(bool), 0, 1, true},
    STR_FIELD("output.rtmp_url", rtmp_url),
    STR_FIELD("output.record_path", record_path),
    NUM_FIELD("queue.video_raw_capacity", video_raw_capacity, FIELD_SIZE, 1, 16),
    NUM_FIELD("queue.audio_raw_capacity", audio_raw_capacity, FIELD_SIZE, 1, 256),
    NUM_FIELD("queue.video_packet_capacity", video_packet_capacity, FIELD_SIZE, 1, 256),
    NUM_FIELD("queue.audio_packet_capacity", audio_packet_capacity, FIELD_SIZE, 1, 512),
    {"log.level", FIELD_LEVEL, offsetof(IpcConfig, log_level), sizeof(IpcLogLevel), 0, 0, false}
};
#define FIELD_COUNT (sizeof(g_fields) / sizeof(g_fields[0]))

/** @brief 记录带位置的配置错误；不使用用户输入作为格式字符串。 */
static int config_error(const char *path, size_t line, const char *key,
                        int code, const char *format, ...)
{
    char detail[512];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    if (line)
        ipc_log_write(IPC_LOG_ERROR, "config", "%s:%zu: %s: %s", path, line, key, detail);
    else
        ipc_log_write(IPC_LOG_ERROR, "config", "%s: %s: %s", path, key, detail);
    return code;
}

static size_t field_index(const char *key)
{
    size_t i;
    for (i = 0; i < FIELD_COUNT; ++i)
        if (strcmp(key, g_fields[i].key) == 0)
            return i;
    return FIELD_COUNT;
}

static size_t field_line(const size_t *lines, const char *key)
{
    size_t index = field_index(key);
    return lines && index < FIELD_COUNT ? lines[index] : 0;
}

/** @brief 就地去掉两端空白；不修改键值中间的字符，不支持行内注释。 */
static char *trim(char *text)
{
    char *end;
    while (*text && isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

/**
 * @brief 读取一行，显式拒绝 NUL/异常控制字符，避免 C 字符串截断绕过校验。
 * @return 1 有一行，0 正常 EOF，负值表示行过长、格式错误或 I/O 错误。
 * @note 即使最后一行没有换行，也会正常交付；fclose 由 load 统一负责。
 */
static int read_config_line(FILE *file, char *buffer)
{
    size_t used = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n')
            break;
        if (ch == 0 || (ch < 32 && ch != '\t' && ch != '\r') || ch == 127)
            return -EINVAL;
        if (used >= IPC_CONFIG_LINE_MAX)
            return -EOVERFLOW;
        buffer[used++] = (char)ch;
    }
    if (ferror(file))
        return -EIO;
    buffer[used] = '\0';
    return ch == EOF && used == 0 ? 0 : 1;
}

/** @brief 只接受非空十进制数字；拒绝符号、尾随字符和转换溢出。 */
static int parse_number(const char *text, uintmax_t *number)
{
    const unsigned char *p = (const unsigned char *)text;
    char *end;
    if (!*p)
        return -EINVAL;
    for (; *p; ++p)
        if (*p < '0' || *p > '9')
            return -EINVAL;
    errno = 0;
    *number = strtoumax(text, &end, 10);
    if (errno == ERANGE)
        return -ERANGE;
    return *end ? -EINVAL : 0;
}

static int assign_field(IpcConfig *config, size_t index, const char *value,
                        const char *path, size_t line)
{
    const ConfigField *field = &g_fields[index];
    unsigned char *dest = (unsigned char *)config + field->offset;
    uintmax_t number;
    int result;
    static const char *const levels[] = {"debug", "info", "warn", "error"};
    size_t i;

    switch (field->type) {
    case FIELD_STRING:
        if (strlen(value) >= field->width)
            return config_error(path, line, field->key, -EOVERFLOW,
                                "string too long (maximum %zu bytes)", field->width - 1);
        memcpy(dest, value, strlen(value) + 1);
        return 0;
    case FIELD_UINT:
    case FIELD_SIZE:
        result = parse_number(value, &number);
        if (result < 0)
            return config_error(path, line, field->key, result,
                                "expected a decimal integer without sign or trailing text");
        if (number < field->minimum || number > field->maximum)
            return config_error(path, line, field->key, -ERANGE,
                                "expected range [%ju, %ju]", field->minimum, field->maximum);
        if (field->type == FIELD_UINT)
            *(unsigned int *)dest = (unsigned int)number;
        else
            *(size_t *)dest = (size_t)number;
        return 0;
    case FIELD_BOOL:
        if (strcmp(value, "true") != 0 && strcmp(value, "false") != 0)
            return config_error(path, line, field->key, -EINVAL, "expected true or false");
        *(bool *)dest = strcmp(value, "true") == 0;
        return 0;
    case FIELD_LEVEL:
        for (i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
            if (strcmp(value, levels[i]) == 0) {
                *(IpcLogLevel *)dest = (IpcLogLevel)i;
                return 0;
            }
        }
        return config_error(path, line, field->key, -EINVAL,
                            "expected debug, info, warn or error");
    }
    return -EINVAL;
}

/** @brief 验证初版所支持的 RTMP URL 子集，不尝试 DNS 或连接服务器。
 * 支持 hostname/IPv4[:port]/app/stream；初版不接受认证字段、IPv6、查询和片段。
 * 占位主机 SRS_SERVER_IP 只在推流禁用时允许，避免误当实际服务器。
 */
static int valid_rtmp_url(const char *url, bool enabled)
{
    const char *authority, *slash, *colon, *p, *stream;
    char host[254];
    size_t host_len, label_len = 0;
    uintmax_t port;
    bool numeric = true;
    struct in_addr address;
    if (!*url)
        return enabled ? -EINVAL : 0;
    if (strncmp(url, "rtmp://", 7) != 0)
        return -EINVAL;
    for (p = url; *p; ++p)
        if (isspace((unsigned char)*p) || strchr("@?#[]", *p))
            return -EINVAL;
    authority = url + 7;
    slash = strchr(authority, '/');
    if (!slash || slash == authority)
        return -EINVAL;
    stream = strchr(slash + 1, '/');
    if (!stream || stream == slash + 1 || !stream[1] ||
        url[strlen(url) - 1] == '/' || strstr(slash, "//"))
        return -EINVAL;
    colon = memchr(authority, ':', (size_t)(slash - authority));
    host_len = (size_t)((colon ? colon : slash) - authority);
    if (host_len == 0 || host_len >= sizeof(host))
        return -EINVAL;
    memcpy(host, authority, host_len);
    host[host_len] = '\0';
    if (colon) {
        char port_text[8];
        size_t port_len = (size_t)(slash - colon - 1);
        if (port_len == 0 || port_len >= sizeof(port_text))
            return -EINVAL;
        memcpy(port_text, colon + 1, port_len);
        port_text[port_len] = '\0';
        if (parse_number(port_text, &port) < 0 || port == 0 || port > 65535)
            return -EINVAL;
    }
    if (strcmp(host, "SRS_SERVER_IP") == 0)
        return enabled ? -EINVAL : 0;
    for (p = host; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (!(ch >= '0' && ch <= '9') && ch != '.')
            numeric = false;
        if (ch == '.') {
            if (label_len == 0 || p[-1] == '-')
                return -EINVAL;
            label_len = 0;
        } else {
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '-') ||
                (label_len == 0 && ch == '-') || ++label_len > 63)
                return -EINVAL;
        }
    }
    if (label_len == 0 || host[host_len - 1] == '-')
        return -EINVAL;
    return numeric && inet_pton(AF_INET, host, &address) != 1 ? -EINVAL : 0;
}

static int validate_config(const IpcConfig *config, const char *path, const size_t *lines)
{
    size_t i;
    static const unsigned int rates[] = {
        7350, 8000, 11025, 12000, 16000, 22050, 24000,
        32000, 44100, 48000, 64000, 88200, 96000
    };
    bool rate_found = false;
    if (!config)
        return config_error(path, 0, "<config>", -EINVAL, "NULL configuration");

    /* 该遍历也服务于公开 validate 接口，防止调用者绕过 load 后传入坏字段。 */
    for (i = 0; i < FIELD_COUNT; ++i) {
        const ConfigField *field = &g_fields[i];
        const unsigned char *value = (const unsigned char *)config + field->offset;
        uintmax_t number;
        if (field->type == FIELD_STRING) {
            const unsigned char *text;
            if (!memchr(value, '\0', field->width))
                return config_error(path, lines ? lines[i] : 0, field->key, -EINVAL,
                                    "string is not NUL terminated");
            if (!*value && strcmp(field->key, "output.rtmp_url") != 0)
                return config_error(path, lines ? lines[i] : 0, field->key, -EINVAL,
                                    "required value is empty");
            for (text = value; *text; ++text)
                if (iscntrl(*text))
                    return config_error(path, lines ? lines[i] : 0, field->key, -EINVAL,
                                        "control characters are not permitted inside a value");
        } else if (field->type == FIELD_UINT || field->type == FIELD_SIZE) {
            number = field->type == FIELD_UINT ? *(const unsigned int *)value : *(const size_t *)value;
            if (number < field->minimum || number > field->maximum)
                return config_error(path, lines ? lines[i] : 0, field->key, -ERANGE,
                                    "expected range [%ju, %ju]", field->minimum, field->maximum);
        }
    }
#define REQUIRE(condition, key, message) do { \
    if (!(condition)) return config_error(path, field_line(lines, key), key, -EINVAL, "%s", message); \
} while (0)
    REQUIRE(strncmp(config->video_device, "/dev/", 5) == 0 && config->video_device[5],
            "video.device", "expected an absolute /dev/... path");
    REQUIRE(strcmp(config->video_pixel_format, "NV12") == 0, "video.pixel_format", "initial version requires NV12");
    REQUIRE(strcmp(config->video_codec, "h264") == 0, "video.codec", "initial version requires h264");
    REQUIRE(strcmp(config->audio_sample_format, "S16_LE") == 0, "audio.sample_format", "initial version requires S16_LE");
    REQUIRE(strcmp(config->audio_codec, "aac") == 0, "audio.codec", "initial version requires aac");
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
        if (config->audio_sample_rate == rates[i])
            rate_found = true;
    REQUIRE(rate_found, "audio.sample_rate", "unsupported AAC sample rate (hardware support still requires validation)");
    REQUIRE(config->record_path[0] == '/' && strlen(config->record_path) > 4 &&
            strcmp(config->record_path + strlen(config->record_path) - 4, ".mp4") == 0,
            "output.record_path", "expected an absolute path ending in .mp4");
    REQUIRE(config->log_level >= IPC_LOG_DEBUG && config->log_level <= IPC_LOG_ERROR,
            "log.level", "invalid log level");
    REQUIRE(valid_rtmp_url(config->rtmp_url, config->rtmp_enabled) == 0, "output.rtmp_url",
            "expected rtmp://host[:port]/app/stream; enabled RTMP rejects empty URL and SRS_SERVER_IP placeholder");
#undef REQUIRE
    return 0;
}

int ipc_config_validate(const IpcConfig *config)
{
    return validate_config(config, "<config>", NULL);
}

int ipc_config_load(const char *path, IpcConfig *config)
{
    IpcConfig candidate = {0};
    size_t lines[FIELD_COUNT] = {0};
    char buffer[IPC_CONFIG_LINE_MAX + 1];
    size_t line_number = 0, i;
    FILE *file;
    int result;
    if (!path || !*path || !config)
        return config_error(path ? path : "<null>", 0, "<file>", -EINVAL, "invalid load arguments");
    candidate.log_level = IPC_LOG_INFO;
    file = fopen(path, "r");
    if (!file) {
        int saved_errno = errno;
        return config_error(path, 0, "<file>", -saved_errno, "cannot open: %s", strerror(saved_errno));
    }
    while ((result = read_config_line(file, buffer)) > 0) {
        char *key, *value, *equal;
        ++line_number;
        /* 允许 Windows 编辑器保存的 UTF-8 BOM，但仅在文件开头识别。 */
        if (line_number == 1 && strncmp(buffer, "\xef\xbb\xbf", 3) == 0)
            memmove(buffer, buffer + 3, strlen(buffer + 3) + 1);
        key = trim(buffer);
        if (!*key || *key == '#')
            continue;
        equal = strchr(key, '=');
        if (!equal) {
            result = config_error(path, line_number, "<syntax>", -EINVAL, "expected key=value");
            goto done;
        }
        *equal = '\0';
        value = trim(equal + 1);
        key = trim(key);
        i = field_index(key);
        if (i == FIELD_COUNT) {
            result = config_error(path, line_number, *key ? key : "<empty-key>", -EINVAL, "unknown key");
            goto done;
        }
        if (lines[i]) {
            result = config_error(path, line_number, key, -EINVAL, "duplicate key (first defined at line %zu)", lines[i]);
            goto done;
        }
        result = assign_field(&candidate, i, value, path, line_number);
        if (result < 0)
            goto done;
        lines[i] = line_number;
    }
    if (result < 0) {
        result = config_error(path, line_number + 1, "<line>", result,
                              "invalid control character, I/O error or line longer than %u bytes", IPC_CONFIG_LINE_MAX);
        goto done;
    }
    for (i = 0; i < FIELD_COUNT; ++i) {
        if (g_fields[i].required && !lines[i]) {
            result = config_error(path, 0, g_fields[i].key, -EINVAL, "missing required key");
            goto done;
        }
    }
    result = validate_config(&candidate, path, lines);

done:
    if (fclose(file) != 0 && result == 0)
        result = config_error(path, 0, "<file>", -EIO, "failed to close input file");
    if (result == 0)
        *config = candidate;
    return result;
}

void ipc_config_dump(const IpcConfig *config)
{
    if (!config)
        return;
    ipc_log_write(IPC_LOG_INFO, "config", "video: device=%s size=%ux%u format=%s codec=%s target_fps=%u bitrate=%u gop=%u",
                  config->video_device, config->video_width, config->video_height,
                  config->video_pixel_format, config->video_codec, config->video_fps,
                  config->video_bitrate, config->video_gop);
    ipc_log_write(IPC_LOG_INFO, "config", "audio: device=%s rate=%u channels=%u format=%s codec=%s bitrate=%u",
                  config->audio_device, config->audio_sample_rate, config->audio_channels,
                  config->audio_sample_format, config->audio_codec, config->audio_bitrate);
    ipc_log_write(IPC_LOG_INFO, "config", "rtmp: enabled=%s url=%s",
                  config->rtmp_enabled ? "true" : "false",
                  config->rtmp_url[0] ? config->rtmp_url : "<empty>");
    ipc_log_write(IPC_LOG_INFO, "config", "record: %s", config->record_path);
    ipc_log_write(IPC_LOG_INFO, "config", "queues: video_raw=%zu audio_raw=%zu video_packet=%zu audio_packet=%zu (per output)",
                  config->video_raw_capacity, config->audio_raw_capacity,
                  config->video_packet_capacity, config->audio_packet_capacity);
}
