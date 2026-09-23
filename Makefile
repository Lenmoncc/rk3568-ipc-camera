# 正式构建默认启用 MPP/ALSA；基础主机测试明确关闭，专用模拟测试分别启用。
# 默认交叉编译；make host-check 使用独立目录做本机语法及链接检查。
SDK_ROOT ?= $(HOME)/rk3568_linux_sdk
BOARD_BUILD_DIR := $(SDK_ROOT)/buildroot/output/rockchip_atk_dlrk3568
SYSROOT ?= $(BOARD_BUILD_DIR)/host/aarch64-buildroot-linux-gnu/sysroot
CC = $(BOARD_BUILD_DIR)/host/bin/aarch64-buildroot-linux-gnu-gcc
WITH_MPP ?= 1
WITH_ALSA ?= 1
WITH_FFMPEG ?= 1
BUILD_DIR ?= build/mpp$(WITH_MPP)-alsa$(WITH_ALSA)-ffmpeg$(WITH_FFMPEG)
BIN_DIR ?= bin
TARGET := $(BIN_DIR)/ipc_camera
QUEUE_TEST := $(BIN_DIR)/test_frame_queue
QUEUE_TEST_OBJECT := $(BUILD_DIR)/test_frame_queue.o
SOURCES := $(sort $(wildcard src/*.c))
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
MOCK_OBJECT := $(BUILD_DIR)/mock_v4l2.o
MOCK_TARGET := $(BIN_DIR)/ipc_camera_mock
MOCK_WRAPS := -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=mmap,--wrap=munmap,--wrap=poll,--wrap=__poll_chk,--wrap=fwrite,--wrap=fclose,--wrap=pthread_create
DEPS := $(OBJECTS:.o=.d) $(QUEUE_TEST_OBJECT:.o=.d) $(MOCK_OBJECT:.o=.d)
CPPFLAGS += -Iinclude -DIPC_WITH_MPP=$(WITH_MPP) -DIPC_WITH_ALSA=$(WITH_ALSA) -DIPC_WITH_FFMPEG=$(WITH_FFMPEG)
# 正式编译只使用 SDK 的头文件和 ARM64 库；测试快照不进入此路径。
MPP_INCLUDE ?= $(SYSROOT)/usr/include/rockchip
MPP_LIBS ?= -lrockchip_mpp
ifeq ($(WITH_MPP),1)
CPPFLAGS += -isystem $(MPP_INCLUDE)
LDLIBS += $(MPP_LIBS)
endif
# 正式 ALSA 使用 SDK 头文件；模拟测试显式覆盖为固定公开头文件和模拟库。
ALSA_INCLUDE ?= $(SYSROOT)/usr/include
ALSA_LIBS ?= -lasound
ifeq ($(WITH_ALSA),1)
CPPFLAGS += -isystem $(ALSA_INCLUDE)
LDLIBS += $(ALSA_LIBS)
endif
# AAC 编码只使用 SDK 中 avcodec/swresample/avutil，无外部进程编码。
FFMPEG_INCLUDE ?= $(SYSROOT)/usr/include
FFMPEG_LIBS ?= -lavcodec -lswresample -lavutil
ifeq ($(WITH_FFMPEG),1)
CPPFLAGS += -isystem $(FFMPEG_INCLUDE)
LDLIBS += $(FFMPEG_LIBS)
endif
CFLAGS ?= -std=c11 -O0 -g -Wall -Wextra -Wpedantic
CFLAGS += -pthread
LDLIBS += -pthread
# 某些受监控容器不支持 LeakSanitizer；可显式覆盖为 detect_leaks=0，
# 此时仅验证地址与未定义行为，不代表已经验证无内存泄漏。
ASAN_OPTIONS ?= detect_leaks=1:halt_on_error=1
ifneq ($(strip $(SYSROOT)),)
CPPFLAGS += --sysroot=$(SYSROOT)
LDFLAGS += --sysroot=$(SYSROOT)
LDFLAGS += -L$(SYSROOT)/usr/lib
LDFLAGS += -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/lib
endif

.PHONY: all clean host-check host-test queue-test host-queue-test host-queue-sanitize
.PHONY: capture-mock host-capture-test host-capture-sanitize
all: $(TARGET)

# 模拟系统调用仅进入独立本机测试二进制，正式 TARGET 不链接 MOCK_OBJECT。
capture-mock: $(MOCK_TARGET)

$(MOCK_OBJECT): tests/mock_v4l2.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(MOCK_TARGET): $(OBJECTS) $(MOCK_OBJECT) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(MOCK_WRAPS) $(LDLIBS) -o $@

host-capture-test:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=0 BUILD_DIR=build/host BIN_DIR=bin/host capture-mock
	python3 tests/test_capture.py ./bin/host/ipc_camera_mock

host-capture-sanitize:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=0 BUILD_DIR=build/capture-asan BIN_DIR=bin/capture-asan CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' capture-mock
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_capture.py ./bin/capture-asan/ipc_camera_mock

# 切换 WITH_MPP/WITH_ALSA 时目标文件目录不同，但部署路径相同，必须重新链接。
# 防止旧对象比另一模式的 bin/ipc_camera 更早而错误复用上一次二进制。
.PHONY: FORCE
FORCE:

$(TARGET): $(OBJECTS) FORCE | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# queue-test 只构建，不能在 Ubuntu 上直接运行生成的 ARM64 测试程序。
# 沿用同一套 CC/sysroot；部署 bin/test_frame_queue 后在板端运行。
queue-test: $(QUEUE_TEST)

$(QUEUE_TEST): $(QUEUE_TEST_OBJECT) $(BUILD_DIR)/frame_queue.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(QUEUE_TEST_OBJECT): tests/test_frame_queue.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

# host-check 的程序只能运行在编译主机，不能部署到 RK3568。
host-check:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=0 BUILD_DIR=build/host BIN_DIR=bin/host all
	./bin/host/ipc_camera --config configs/ipc.conf --check-config

# 仅在 Ubuntu/本机运行，测试使用独立临时配置，不访问设备或网络。
host-test: host-check
	python3 tests/test_config.py ./bin/host/ipc_camera
	cc -std=c11 -Wall -Wextra -Werror -Iinclude -pthread tests/test_log.c src/log.c -o build/host/test_log
	python3 tests/check_log.py ./build/host/test_log
	cc -std=c11 -Wall -Wextra -Werror -Iinclude -pthread tests/test_config_api.c src/config.c src/log.c -o build/host/test_config_api
	./build/host/test_config_api configs/ipc.conf
	$(MAKE) host-queue-test
	$(MAKE) host-capture-test
	$(MAKE) host-encoder-test
	$(MAKE) host-audio-test

# 两个本机测试入口都不访问配置、设备或网络，输出目录与 ARM64 隔离。
host-queue-test:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=0 BUILD_DIR=build/host BIN_DIR=bin/host queue-test
	./bin/host/test_frame_queue

# 可选内存检查，仅用于支持 sanitizer 的主机编译器，不部署到 RK3568。
host-queue-sanitize:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=0 BUILD_DIR=build/queue-asan BIN_DIR=bin/queue-asan CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' queue-test
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 ./bin/queue-asan/test_frame_queue

# 测试用的是官方公开头文件快照 + 故障注入实现；不需要主机 MPP 库。
.PHONY: encoder-mock host-encoder-test host-encoder-sanitize
encoder-mock: $(BIN_DIR)/ipc_camera_encoder_mock $(BIN_DIR)/test_encoder_api

$(BUILD_DIR)/mock_mpp.o: tests/mock_mpp.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR)/ipc_camera_encoder_mock: $(OBJECTS) $(MOCK_OBJECT) $(BUILD_DIR)/mock_mpp.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(MOCK_WRAPS) $(LDLIBS) -o $@

$(BUILD_DIR)/test_encoder_api.o: tests/test_encoder_api.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR)/test_encoder_api: $(BUILD_DIR)/test_encoder_api.o $(BUILD_DIR)/video_encoder.o $(BUILD_DIR)/timestamp.o $(BUILD_DIR)/log.o $(BUILD_DIR)/config.o $(BUILD_DIR)/mock_mpp.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

host-encoder-test:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=1 MPP_INCLUDE=tests/mpp_headers MPP_LIBS= BUILD_DIR=build/encoder-mock BIN_DIR=bin/encoder-mock CFLAGS='-std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread' encoder-mock
	python3 tests/test_encoder.py ./bin/encoder-mock/ipc_camera_encoder_mock
	./bin/encoder-mock/test_encoder_api configs/ipc.conf

host-encoder-sanitize:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_ALSA=0 WITH_MPP=1 MPP_INCLUDE=tests/mpp_headers MPP_LIBS= BUILD_DIR=build/encoder-asan BIN_DIR=bin/encoder-asan CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' encoder-mock
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_encoder.py ./bin/encoder-asan/ipc_camera_encoder_mock
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 ./bin/encoder-asan/test_encoder_api configs/ipc.conf

clean:
	rm -rf build bin

-include $(DEPS) $(BUILD_DIR)/mock_mpp.d $(BUILD_DIR)/test_encoder_api.d

# 使用真实 ALSA 公共头文件及模拟实现，独立目录避免与正式 ARM64 对象混用。
.PHONY: audio-mock host-audio-test host-audio-sanitize
audio-mock: $(BIN_DIR)/ipc_camera_audio_mock $(BIN_DIR)/test_audio_api

$(BUILD_DIR)/mock_alsa.o: tests/mock_alsa.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_audio_api.o: tests/test_audio_api.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR)/ipc_camera_audio_mock: $(OBJECTS) $(BUILD_DIR)/mock_alsa.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -Wl,--wrap=fwrite,--wrap=fclose,--wrap=pthread_create $(LDLIBS) -o $@

$(BIN_DIR)/test_audio_api: $(BUILD_DIR)/test_audio_api.o $(BUILD_DIR)/alsa_capture.o $(BUILD_DIR)/frame_queue.o $(BUILD_DIR)/timestamp.o $(BUILD_DIR)/config.o $(BUILD_DIR)/log.o $(BUILD_DIR)/mock_alsa.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -Wl,--wrap=fwrite,--wrap=fclose,--wrap=pthread_create $(LDLIBS) -o $@

host-audio-test:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_MPP=0 WITH_ALSA=1 ALSA_INCLUDE=tests/alsa_headers ALSA_LIBS= BUILD_DIR=build/audio-mock BIN_DIR=bin/audio-mock CFLAGS='-std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread' audio-mock
	python3 tests/test_audio.py ./bin/audio-mock/ipc_camera_audio_mock
	./bin/audio-mock/test_audio_api configs/ipc.conf

host-audio-sanitize:
	$(MAKE) CC=cc SYSROOT= WITH_FFMPEG=0 WITH_MPP=0 WITH_ALSA=1 ALSA_INCLUDE=tests/alsa_headers ALSA_LIBS= BUILD_DIR=build/audio-asan BIN_DIR=bin/audio-asan CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' audio-mock
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_audio.py ./bin/audio-asan/ipc_camera_audio_mock
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 ./bin/audio-asan/test_audio_api configs/ipc.conf

-include $(BUILD_DIR)/mock_alsa.d $(BUILD_DIR)/test_audio_api.d

# 使用主机 FFmpeg 开发库运行真实 AAC 编码；仅 ALSA 输入使用模拟，不需要麦克风。
# 可通过 FFMPEG_INCLUDE/FFMPEG_LIBS 指向主机编译的 4.4.1，禁止指向 ARM64 库运行。
.PHONY: aac-test-binaries host-aac-test host-aac-sanitize
aac-test-binaries: $(BIN_DIR)/ipc_camera_audio_mock $(BIN_DIR)/test_aac_api
$(BUILD_DIR)/test_aac_api.o: tests/test_aac_api.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BIN_DIR)/test_aac_api: $(BUILD_DIR)/test_aac_api.o $(BUILD_DIR)/audio_encoder.o $(BUILD_DIR)/audio_adts.o $(BUILD_DIR)/timestamp.o $(BUILD_DIR)/config.o $(BUILD_DIR)/log.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -lm -o $@
host-aac-test:
	$(MAKE) CC=cc SYSROOT= WITH_MPP=0 WITH_ALSA=1 WITH_FFMPEG=1 ALSA_INCLUDE=tests/alsa_headers ALSA_LIBS= FFMPEG_INCLUDE='$(FFMPEG_INCLUDE)' FFMPEG_LIBS='$(FFMPEG_LIBS)' BUILD_DIR=build/aac-real BIN_DIR=bin/aac-real CFLAGS='-std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror -pthread' aac-test-binaries
	python3 tests/test_aac.py ./bin/aac-real/ipc_camera_audio_mock ./bin/aac-real/test_aac_api
host-aac-sanitize:
	$(MAKE) CC=cc SYSROOT= WITH_MPP=0 WITH_ALSA=1 WITH_FFMPEG=1 ALSA_INCLUDE=tests/alsa_headers ALSA_LIBS= FFMPEG_INCLUDE='$(FFMPEG_INCLUDE)' FFMPEG_LIBS='$(FFMPEG_LIBS)' BUILD_DIR=build/aac-asan BIN_DIR=bin/aac-asan CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' aac-test-binaries
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_aac.py ./bin/aac-asan/ipc_camera_audio_mock ./bin/aac-asan/test_aac_api
-include $(BUILD_DIR)/test_aac_api.d
