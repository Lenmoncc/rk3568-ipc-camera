# 当前实现配置、日志和原始帧队列，尚未接入 FFmpeg/ALSA/MPP。
# 默认交叉编译；make host-check 使用独立目录做本机语法及链接检查。
SDK_ROOT ?= $(HOME)/rk3568_linux_sdk
BOARD_BUILD_DIR := $(SDK_ROOT)/buildroot/output/rockchip_atk_dlrk3568
SYSROOT ?= $(BOARD_BUILD_DIR)/host/aarch64-buildroot-linux-gnu/sysroot
CC = $(BOARD_BUILD_DIR)/host/bin/aarch64-buildroot-linux-gnu-gcc
BUILD_DIR ?= build
BIN_DIR ?= bin
TARGET := $(BIN_DIR)/ipc_camera
QUEUE_TEST := $(BIN_DIR)/test_frame_queue
QUEUE_TEST_OBJECT := $(BUILD_DIR)/test_frame_queue.o
SOURCES := $(sort $(wildcard src/*.c))
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d) $(QUEUE_TEST_OBJECT:.o=.d)
CPPFLAGS += -Iinclude
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
all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BIN_DIR)
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
	$(MAKE) CC=cc SYSROOT= BUILD_DIR=build/host BIN_DIR=bin/host all
	./bin/host/ipc_camera --config configs/ipc.conf --check-config

# 仅在 Ubuntu/本机运行，测试使用独立临时配置，不访问设备或网络。
host-test: host-check
	python3 tests/test_config.py ./bin/host/ipc_camera
	cc -std=c11 -Wall -Wextra -Werror -Iinclude -pthread tests/test_log.c src/log.c -o build/host/test_log
	python3 tests/check_log.py ./build/host/test_log
	cc -std=c11 -Wall -Wextra -Werror -Iinclude -pthread tests/test_config_api.c src/config.c src/log.c -o build/host/test_config_api
	./build/host/test_config_api configs/ipc.conf
	$(MAKE) host-queue-test

# 两个本机测试入口都不访问配置、设备或网络，输出目录与 ARM64 隔离。
host-queue-test:
	$(MAKE) CC=cc SYSROOT= BUILD_DIR=build/host BIN_DIR=bin/host queue-test
	./bin/host/test_frame_queue

# 可选内存检查，仅用于支持 sanitizer 的主机编译器，不部署到 RK3568。
host-queue-sanitize:
	$(MAKE) CC=cc SYSROOT= BUILD_DIR=build/queue-asan BIN_DIR=bin/queue-asan CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' queue-test
	ASAN_OPTIONS='$(ASAN_OPTIONS)' UBSAN_OPTIONS=halt_on_error=1 ./bin/queue-asan/test_frame_queue

clean:
	rm -rf build bin

-include $(DEPS)
