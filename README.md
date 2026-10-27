# rts-tools

[![build](https://github.com/rts-component/rts-tools/actions/workflows/build.yml/badge.svg)](https://github.com/rts-component/rts-tools/actions/workflows/build.yml)

串口调试与曲线查看工具集，包含两个可执行文件：

- **`rts-tools`** — 串口收发 + CSV 解析：把接收到的 CSV 行解析成数据列，支持分组、
  高亮规则和曲线勾选。
- **`rts-plot`** — 曲线查看器。每个勾选的"组"由 `rts-tools` 拉起一个独立进程，通过本地
  套接字接收采样数据；独立的进程边界保证单个图表崩溃不影响主程序。

## 目录结构

```
app/        主程序入口与主窗口（串口收发、接收日志）
csv/        CSV 解析、分组与数据面板
highlight/  接收日志的高亮规则
common/     rts-tools 与 rts-plot 共用：本地管道协议、端点清单、数据目录（静态库 rts-common）
plot/server/  主程序侧：曲线数据出口（PlotServer）与图表进程管理（PlotLauncher）
plot/viewer/  rts-plot：连接、数据表与绘图窗口
resources/  图标、Windows 资源模板、翻译
tools/      开发脚本（如 make_icons.py 生成图标）
packaging/  Linux 桌面入口
```

## 构建

依赖 CMake ≥ 3.19 和 Qt ≥ 6.5（Core、Widgets、SerialPort、Network、LinguistTools）：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux 上打包 Qt 运行时需要构建机额外安装 `patchelf`。

## 翻译

界面文字用 `tr()`，中文翻译在 `resources/rts-tools_zh_CN.ts` 与 `resources/rts-plot_zh_CN.ts`，
构建时编译进可执行文件，按系统语言自动选择。新增/修改界面文字后：

```sh
cmake --build build --target update_translations   # 提取新文字到 .ts（用 Qt Linguist 翻译）
```

## 测试

```sh
ctest --test-dir build --output-on-failure          # 协议拆帧/编解码、CSV 解析
```

## 打包

```sh
cmake --build build --target package
```

产物为 deb 包，安装到 `/opt/rts-tools`，Qt 库和插件一并部署，目标机器无需另装 Qt。

## 致谢

界面风格参考了 [jooiee.com](https://jooiee.com) 的 **JCom** 软件：整体布局、配色和控件
观感都从它那里获得启发。本项目只借鉴设计思路，代码、样式表和图标全部自行实现，未使用
该软件的源码或素材，与作者亦无隶属或背书关系。

## 许可证

本项目源代码以 **MIT License** 发布，全文见 [LICENSE](./LICENSE)。

发布的二进制包中随附的 Qt 库仍为 **LGPLv3**，详见
[THIRD-PARTY-NOTICES.md](./THIRD-PARTY-NOTICES.md)。
