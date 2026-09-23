/** @file asoundlib.h
 * @brief 本机测试聚合头：仅汇入本项目使用的 ALSA v1.2.4 官方接口。
 * 正式构建使用 SDK 完整 asoundlib.h；此文件不声明自定义 ALSA ABI。
 */
#ifndef IPC_TEST_ASOUNDLIB_H
#define IPC_TEST_ASOUNDLIB_H
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <poll.h>
#include <stdarg.h>
#include "global.h"
#include "input.h"
#include "output.h"
#include "conf.h"
#include "pcm.h"
#include "error.h"
#endif
