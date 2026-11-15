# DR Dict

基于 C++20 与 WebView2 的 Windows 桌面词典。结构与 DR Reader 一致：原生引擎负责解析与索引，界面由 WebView2 承载，词典正文（本质是 HTML）直接交给浏览器内核渲染。功能与格式处理参考 GoldenDict-ng（独立实现）。

## 功能（v0.1）

- 导入 MDict（`.mdx`，自动带上 `.mdd`、`.1.mdd`、`.2.mdd`… 资源卷）与 StarDict（`.ifo/.idx[.gz]/.dict[.dz]/.syn`）；可选文件或整个文件夹（递归查找）。词典留在原位，只建立索引。
- 词头联想（前缀，大小写/变音符/标点不敏感）、多词典并列查询、英文屈折形式回退（studies → study）。
- 中文简繁同查（OpenCC，可选）：开启后查中文时同时查简体与繁体，繁体可选 OpenCC 标准、台湾正体、台湾正体及台湾用词、香港繁体或全部。每部词典标题右侧的“原 / 简 / 繁”切换该词典释义的写法（只转换正文文字，选择按词典记住）；“繁”旁的箭头可为该词典选择繁体标准，默认标准在设置中指定。
- 每部词典的正文在独立 iframe 中渲染：词典自带 CSS/JS/图片/字体按相对路径从 `.mdd` 或词典目录读取，样式互不干扰；`entry://`、`bword://` 跳转查询，`sound://` 播放发音，外部链接用系统浏览器打开；双击正文中的词继续查询；选中文字后右键可查询、复制或朗读。
- 词典库管理：
  - 启用/停用、拖放排序、批量启用/停用/移除、简介、重建索引；
  - 移除的词典进入可恢复的“已移除”分组；
  - 刷新会重扫已添加的文件夹；
  - 文件丢失或变化时提示。
- 搜索框右侧的词典选择器：可以只查某本或某几本词典；最近查询可以逐条删除。
- 收藏：顶栏星标（Ctrl D），侧栏可切换“最近查询 / 收藏”。
- AI 助查（默认关闭）：在“设置 → AI 助查”开启，复用本机已安装并登录的 Codex、Claude Code 或 opencode，可选择模型和思考强度。点击收藏右边的 AI 按钮或按 Ctrl J 打开右侧面板，也可选中词条文字后右键“问 AI”；回答流式显示，支持停止、复制和新对话。面板未固定时点击别处或按 Esc 自动隐藏，图钉可固定在右侧；顶栏按钮可单独在设置中隐藏。提问会将当前词语、选中文字和对话发送给 CLI 所连接的模型服务；对话仅保留在当前窗口内存中。
- 多部词典的结果右侧有可展开的目录。
- Ctrl+C+C 弹窗查词（参考 GoldenDict-ng 的做法）：在任意程序中选中文字，按住 Ctrl 快速按两次 C，鼠标旁会弹出置顶的小窗口显示释义。
  - 第一次 Ctrl+C 照常复制；
  - 窗口可拖动、可调整大小（大小会被记住）；
  - 可固定、前进/后退，也可转到主窗口继续查；
  - 未固定时，Esc、点击别处或鼠标移开都会关闭窗口，焦点回到原来的程序。
  - 可在“设置 → 词条”中关闭。
  - 需要 DR Dict 保持运行。
- 发音音量默认 22%，可在设置中调节，录音和朗读都适用。
- 页面缩放：Ctrl+滚轮、Ctrl +/−，Ctrl 0 还原；在词条正文上同样有效，缩放比例会保存。
- 配色：跟随系统、浅色、深色，以及纸张、豆绿、雾灰、夜蓝、暖褐五套内置配色（深色下可反色显示词条）。
- 外观自定义：可自定义背景颜色和主题色（背景较深时文字自动改用浅色）；可设置背景图片（复制到数据目录），并调节浓度和模糊；标题栏颜色随之变化。
- Mica 或 Direct2D 标题栏，正文宽度，查询历史，前进/后退。

暂不支持：MDict 3.0、需要注册码的加密 MDict（Encrypted=1）、DSL/BGL/XDXF 文件等其他格式、全文搜索、`.spx` 发音。

## 构建

需要 Visual Studio 2026（C++ 桌面开发）、CMake ≥ 3.25、Node.js ≥ 22.18、pnpm、Python 3（构建 OpenCC 词典数据时使用），以及 WebView2 Runtime。

```powershell
./scripts/build.ps1        # 安装前端依赖、配置、编译、运行测试
./scripts/build.ps1 -Run   # 同上并启动
```

产物位于 `build/bin/Release/`：`dr-dict.exe`（需与同目录的 `web/`、`opencc/` 一起分发）、`dr-engine-tests.exe`、`dr-dict-cli.exe`。

### uiAccess 版本（真正置顶）

普通构建中，弹窗使用常规置顶，能够盖住普通窗口。若要让它也盖住以管理员身份运行的程序、开始菜单和任务栏，需要 `uiAccess=true`。

Windows 只认可满足以下两个条件的 uiAccess 程序：使用受信任的证书签名，并且安装在 Program Files 中。未签名或放在其他目录的 uiAccess 程序无法启动，所以默认构建不开启这一选项（CMake 选项 `DR_DICT_UIACCESS`）。

在管理员 PowerShell 中运行下面的命令，脚本会依次完成：

1. 构建 uiAccess 版本；
2. 生成本机测试证书并加入信任；
3. 安装到 `C:\Program Files\DR Dict`；
4. 签名，并创建开始菜单快捷方式。

```powershell
pwsh -File .\scripts\install-uiaccess.ps1
```

测试证书只能用于本机，不要对外分发用它签名的程序。

命令行参数：`dr-dict.exe [--data-dir <目录>] [--shell mica|direct2d] [--lookup <词> [--caret l,t,r,b] [--keyboard]] [词典文件或文件夹...]`。`--lookup` 在鼠标旁的弹窗中查词；若 DR Dict 已在运行，就交给已运行的实例处理。DR Reader 选词菜单中的“查词典”就是这样调用的（见 [docs/architecture.md](docs/architecture.md)）。`--caret` 给出文字光标所在行的屏幕矩形，弹窗开在它下方（放不下时在上方）；`--keyboard` 表示由按键唤起，弹窗不随鼠标移开而关闭。水杉输入法的查词快捷键用的就是这两项。默认数据目录为 `%LOCALAPPDATA%\DRDict`。

## 目录

- `engine/` 词典格式（`mdict.cpp`、`stardict.cpp`）、解压（zlib/gzip/dictzip/LZO1X）、RIPEMD-128、文本折叠、SQLite 索引与词典库（`library.cpp`）。
- `ui/` 原生窗口外壳（沿用 DR Reader）。
- `app/` WebView2 宿主、消息桥、`https://dict.dr-dict.local/` 资源服务。
- `web/` Vite + TypeScript 界面；`web/public/frame.js|css` 注入每个词条 iframe。
- `tests/` 引擎测试（生成合成的 MDX/MDD/StarDict 文件）；`tools/dict_cli.cpp` 用真实词典做验证。

详见 [docs/architecture.md](docs/architecture.md)。
