# DR Dict

基于 C++20 与 WebView2 的 Windows 桌面词典。结构与 DR Reader 一致：原生引擎负责解析与索引，界面由 WebView2 承载，词典正文（本质是 HTML）直接交给浏览器内核渲染。功能与格式处理参考 GoldenDict-ng（独立实现）。

## 功能（v0.1）

- 导入 MDict（`.mdx`，自动带上 `.mdd`、`.1.mdd`、`.2.mdd`… 资源卷）与 StarDict（`.ifo/.idx[.gz]/.dict[.dz]/.syn`）；可选文件或整个文件夹（递归查找）。词典留在原位，只建立索引。
- 词头联想（前缀，大小写/变音符/标点不敏感）、多词典并列查询、英文屈折形式回退（studies → study）。
- 每部词典的正文在独立 iframe 中渲染：词典自带 CSS/JS/图片/字体按相对路径从 `.mdd` 或词典目录读取，样式互不干扰；`entry://`、`bword://` 跳转查询，`sound://` 播放发音，外部链接用系统浏览器打开；双击正文中的词继续查询。
- 词典库管理：启用/停用、排序、简介、重建索引、移除；文件丢失或变化时提示。
- 浅色/深色/跟随系统主题（深色下可反色显示词条），Mica 或简洁标题栏，正文宽度，查询历史，前进/后退。

暂不支持：MDict 3.0、需要注册码的加密 MDict（Encrypted=1）、DSL/BGL/XDXF 文件等其他格式、全文搜索、`.spx` 发音。

## 构建

需要 Visual Studio 2026（C++ 桌面开发）、CMake ≥ 3.25、Node.js ≥ 22.18、pnpm，以及 WebView2 Runtime。

```powershell
./scripts/build.ps1        # 安装前端依赖、配置、编译、运行测试
./scripts/build.ps1 -Run   # 同上并启动
```

产物位于 `build/bin/Release/`：`dr-dict.exe`（需与同目录的 `web/` 一起分发）、`dr-engine-tests.exe`、`dr-dict-cli.exe`。

命令行参数：`dr-dict.exe [--data-dir <目录>] [--shell mica|direct2d] [词典文件或文件夹...]`。默认数据目录为 `%LOCALAPPDATA%\DRDict`。

## 目录

- `engine/` 词典格式（`mdict.cpp`、`stardict.cpp`）、解压（zlib/gzip/dictzip/LZO1X）、RIPEMD-128、文本折叠、SQLite 索引与词典库（`library.cpp`）。
- `ui/` 原生窗口外壳（沿用 DR Reader）。
- `app/` WebView2 宿主、消息桥、`https://dict.dr-dict.local/` 资源服务。
- `web/` Vite + TypeScript 界面；`web/public/frame.js|css` 注入每个词条 iframe。
- `tests/` 引擎测试（生成合成的 MDX/MDD/StarDict 文件）；`tools/dict_cli.cpp` 用真实词典做验证。

详见 [docs/architecture.md](docs/architecture.md)。
