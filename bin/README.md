# 预编译板端程序


| 文件 | 用途 | 静态检查结果 |
| --- | --- | --- |
| `ipc_camera` | NV12/H.264/PCM/AAC 调试、MP4 录像、RTMP 推流 | ELF64、小端、AArch64、动态链接，含调试信息 |
| `test_frame_queue` | 原始队列的 9 项板端测试 | ELF64、小端、AArch64、动态链接，含调试信息 |
| `SHA256SUMS` | 两个可执行文件的内容校验值 | 在本目录运行 `sha256sum -c SHA256SUMS` |

## 直接试用

在开发板的工程根目录执行：

```bash
chmod +x bin/ipc_camera bin/test_frame_queue
(cd bin && sha256sum -c SHA256SUMS)
./bin/ipc_camera --help
sh scripts/run.sh --check-config
./bin/test_frame_queue
```



## 更新约定

在 Ubuntu 用配套 SDK 构建，而后更新校验值：

```bash
sh scripts/build.sh -B all queue-test
file bin/ipc_camera bin/test_frame_queue
(cd bin && sha256sum ipc_camera test_frame_queue > SHA256SUMS)
git add bin/ipc_camera bin/test_frame_queue bin/SHA256SUMS
```

`file` 应显示 AArch64。主机测试应使用 `make host-*` 提供的独立目录，禁止将 x86_64 模拟程序替换成发布程序。
`make clean` 保留本目录；修改二进制后应一并提交对应源代码，并在开发板复验。
