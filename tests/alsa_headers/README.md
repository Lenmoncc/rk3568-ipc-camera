# ALSA 公共头文件测试快照

来源：[alsa-project/alsa-lib](https://github.com/alsa-project/alsa-lib)，标签 `v1.2.4`，
提交 `49bd4b198e867eedb76ce8d1a504309934343532`。

`alsa/` 中 `global.h`、`input.h`、`output.h`、`conf.h`、`pcm.h`、`error.h` 原样来自该提交的 `include/`。
许可证为 LGPL-2.1-or-later，保留原文件版权声明及本目录 `COPYING`。
`alsa/asoundlib.h` 是本项目的最小聚合头，仅包含系统依赖和上述公开头文件，不重写 ABI。

`host-audio-test` / `host-audio-sanitize` 使用这些声明检查模拟实现及正式模块调用是否匹配，
不需要主机声卡或开发包；`tests/mock_alsa.c` 才提供模拟函数实现。
正式交叉编译始终使用 SDK 的 ALSA 头文件和 `-lasound`，不会包含此目录或模拟实现。
这不是对板端 ALSA 版本的断言，最终以 SDK 和烧录镜像配套版本为准。
