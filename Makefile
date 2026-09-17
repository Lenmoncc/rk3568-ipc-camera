# 当前仅构建骨架，不链接或冒充验证尚未接入的 FFmpeg/ALSA/MPP。
# 默认交叉编译；make host-check 使用独立目录做本机语法及链接检查。
SDK_ROOT ?= $(HOME)/rk3568_linux_sdk
BOARD_BUILD_DIR := $(SDK_ROOT)/buildroot/output/rockchip_atk_dlrk3568
SYSROOT ?= $(BOARD_BUILD_DIR)/host/aarch64-buildroot-linux-gnu/sysroot
CC = $(BOARD_BUILD_DIR)/host/bin/aarch64-buildroot-linux-gnu-gcc
BUILD_DIR ?= build
BIN_DIR ?= bin
TARGET := $(BIN_DIR)/ipc_camera
SOURCES := $(sort $(wildcard src/*.c))
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -O0 -g -Wall -Wextra -Wpedantic
ifneq ($(strip $(SYSROOT)),)
CPPFLAGS += --sysroot=$(SYSROOT)
LDFLAGS += --sysroot=$(SYSROOT)
LDFLAGS += -L$(SYSROOT)/usr/lib
LDFLAGS += -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/lib
endif

.PHONY: all clean host-check
all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

# host-check 的程序只能运行在编译主机，不能部署到 RK3568。
host-check:
	$(MAKE) CC=cc SYSROOT= BUILD_DIR=build/host BIN_DIR=bin/host all
	./bin/host/ipc_camera

clean:
	rm -rf build bin

-include $(DEPS)
