# MPP 测试头文件来源

本目录仅为无 RK3568 硬件的主机测试提供**未经修改的官方公开头文件依赖闭包**。
正式交叉编译只包含 SDK sysroot 中的头文件并链接 SDK 的 `librockchip_mpp`，
不会包含或链接本目录；这里不提供硬编码库或硬件能力。

- 上游：https://github.com/rockchip-linux/mpp
- 固定提交：`ee0773be065ba43f5f8fee218f8c0ae7343dfa57`
- 来源：该提交的 `inc/`，以 `rk_mpi.h`、`mpp_buffer.h`、`mpp_frame.h`、
  `mpp_packet.h`、`rk_venc_cfg.h` 为入口递归收集，共 17 个头文件。
- 保留各文件原版权声明；许可文本见本目录 `Apache-2.0.txt`、`MIT.txt`，
  实际许可按每个源文件的声明执行。

`tests/mock_mpp.c` 提供故障注入和资源所有权检查。模拟包只是测试数据，不能播放。
此快照证明正式 C 模块可针对公开 API 编译，不证明用户 BSP 库版本与硬件已经通过验证。
