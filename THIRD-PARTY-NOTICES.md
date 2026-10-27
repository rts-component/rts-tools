# Third-party notices

`rts-tools` 的源代码以 MIT 许可证发布（见 [LICENSE](./LICENSE)）。发布的可执行文件与安装包
（deb / AppImage）中还包含以下第三方组件，它们仍受各自的许可证约束，不因本项目的 MIT
许可证而改变。

## Qt 6

- 组件：Qt Core、Qt Widgets、Qt SerialPort、Qt Network（含所需平台插件与图像插件）
- 许可证：**GNU Lesser General Public License v3（LGPLv3）**，或商业许可证
- 版权：Copyright (C) The Qt Company Ltd. 及 Qt 贡献者
- 获取源码：<https://download.qt.io/official_releases/qt/>（选择对应的 6.x 源码包），
  或使用发行版提供的源码包：`apt-get source qt6-base qt6-serialport`。

许可证全文见 [LICENSES/LGPL-3.0.txt](./LICENSES/LGPL-3.0.txt)；
LGPLv3 引用的 GPLv3 全文见 [LICENSES/GPL-3.0.txt](./LICENSES/GPL-3.0.txt)。

### 关于可替换性

本项目**动态链接**未修改的 Qt 共享库，Qt 以独立的 `.so` 文件随包分发，用户可以用自己
编译的兼容版本替换它们（Linux 安装目录为 `/opt/rts-tools/lib`，AppImage 位于
`AppDir/usr/lib`），替换后无需修改本项目的程序即可运行。本项目未对 Qt 源码做任何修改，
因此不适用 LGPLv3 第 4 条 d) 项关于修改版本的要求。

## 仅用于构建的工具（不随发布物分发）

| 工具 | 许可证 | 用途 |
|---|---|---|
| `patchelf` | GPL-3.0 | Linux 部署时修正 Qt 插件的 RPATH |
| `lrelease`（Qt Linguist） | GPL-3.0 | 把 `rts-tools_zh_CN.ts` 编译成 `.qm` |
| `Pillow` | HPND | `make_icons.py` 生成应用图标 |

这些工具只参与构建过程，其许可证不延伸到本项目的源代码或生成的文件。
