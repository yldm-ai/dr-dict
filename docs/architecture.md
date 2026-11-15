# 架构

## 进程内分层

```
web/ (WebView2, https://app.dr-dict.local/index.html)
  │  chrome.webview.postMessage {id, method, params}  ⇄  {id, result|error} / {event}
app/ Application ── Worker queries_(2 线程)：查询、联想、文章与资源
  │               └─ Worker imports_(1 线程)：导入、重建索引、移除（写索引）
engine/ Library ── index.db (SQLite WAL) + dictionaries.json / settings.json / history.json
          └─ Dictionary（MdictDictionary / StardictDictionary）只读打开原词典文件
```

## 索引

`index.db`：
- `dicts(num, id)` —— 词典 id（路径小写后的 FNV-1a 64）到整数编号。
- `words(dict, key, word, a, b)`，索引 `(key, dict)`。`key = fold_key(word)`：NFKD、去拉丁/希腊/西里尔组合附加符、小写、去标点空白（思路同 GoldenDict `Folding::apply`）。`a/b` 为格式定位：MDict 为解压后记录区的偏移/长度，StarDict 为 `.dict` 偏移/长度。
- `resources(dict, key, volume, a, b)` —— `.mdd` 资源，key 为小写、`/` 分隔的相对路径。

词典库元数据：
- `dictionaries.json` 保存全部词典条目。移除的词典不删条目，而是标记 `removed`：停用、删除索引，界面放在折叠的“已移除”组里，可以恢复；文件不存在后才能清除。
- `sources.json` 记录通过“添加文件夹”加入的文件夹。刷新时递归重扫这些文件夹，只加入新词典，不会动已移除的词典；单独再次添加某个文件则会恢复它。

联想使用 `key >= ? AND key < ? || U+10FFFF` 的范围扫描；查询按 `key` 精确匹配，未命中时尝试英文原形。

## 中文简繁

`ChineseVariants`（`engine/src/chinese.cpp`）基于 OpenCC 1.4.2（Apache-2.0，FetchContent 静态链接）。构建时 OpenCC 用 Python 3 生成 `.ocd2` 词典，与所用配置一起复制到 `bin/<配置>/opencc/`。思路参考 GoldenDict-ng `dict/transliteration/chinese.cc`（多条转换链的结果合并查询），独立实现：

1. 先用 `t2s`、`tw2s`、`hk2s`（选台湾用词或全部时加 `tw2sp`）把输入转成简体，因此任何地区的繁体输入都能命中。
2. 再把每个简体写法转成所选标准：`s2t`、`s2tw`、`s2twp`、`s2hk`，选“全部”时四个都用。

开启 `chineseConversion` 后，查询、联想与文章页都会合并这些写法：先给原写法，再给其他写法，文章按记录偏移去重。查询结果的 `variants` 列出另外命中的写法。

## 词条渲染

每个命中的词典生成一个 iframe：`https://dict.dr-dict.local/<id>/@article?word=…&theme=light|dark`。该主机不映射文件夹，所有请求由 `WebResourceRequested` 延迟回调交给 `queries_` 线程处理：

- `@article` —— 汇总该词全部记录，跟随 `@@@LINK=` 重定向（有界），套用 MDict StyleSheet，包上 `frame.css/frame.js`。
- `@about`、`@icon` —— 简介与图标。
- 其他路径 —— 资源：先找词典目录（StarDict 为 `res/`）中的同名文件，再查 `.mdd`（参考 GoldenDict `MdxDictionary::loadResourceFile` 的优先级），支持 UTF-16 `@@@LINK=` 资源重定向；拒绝 `..`。

因为文章文档的基址就是 `/<id>/`，词典里的相对 `src/href` 无需改写即可命中资源；各词典 CSS 天然隔离。`frame.js` 通过 `postMessage` 把高度、`entry://` 查询、外链、锚点滚动和快捷键交给主界面（主界面校验 `event.origin`）。

## Ctrl+C+C 弹窗

- **热键**（`app/src/copy_hotkey.cpp`）：
  - 参考 GoldenDict-ng `hotkey/winhotkeywrapper.cc` 的思路，独立实现。
  - 用 `WH_KEYBOARD_LL` 钩子观察按键，而不用 RegisterHotKey 加 SendInput：后者在前台是管理员程序时会被 UIPI 拦住，表现为 Ctrl+C 失灵。
  - 第一次 Ctrl+C 原样放行，让前台程序完成复制；500 ms 内的第二次 Ctrl+C 会被吞掉，并触发弹窗。
  - 按住 C 产生的自动重复不算第二次。
- **弹窗窗口**（`app/src/popup.cpp`）：
  - 无边框、可调整大小（WebView 四周留 5 DIP 的原生缩放边），没有所属窗口，因此主窗口最小化时也能弹出。
  - 定位方式与自动关闭规则参考 GoldenDict-ng 的 ScanPopup，均为独立实现。
- **弹窗页面**：拥有自己的 WebView（`popup.html`）。
  - 原生端读取剪贴板后发送 `popup-lookup` 事件；页面查询并渲染完成后，再请求 `popup.show`，因此不会先闪出旧内容。
- **置顶与 uiAccess**：做法沿用 MSIME-Windows（`windows_webview2.cpp`）的经验。
  - 在 uiAccess 进程里，宿主窗口如果在 WebView2 控制器创建之前就已是 TOPMOST，创建会以 E_INVALIDARG 失败（WebView2Feedback #486）。
  - 因此弹窗创建时不置顶，等页面导航完成 1 秒后才设为 TOPMOST。
  - 每次移动、显示或切换置顶层级后，都调用 `NotifyParentWindowPositionChanged`。
  - 隐藏和显示时同步设置控制器的 `IsVisible`；不这样做，再次显示时只会看到旧的空白帧。
- **供其他程序调用**（DR Reader 选词菜单的“查词典”）：
  - 按窗口类 `DrDictPopup` 找到弹窗宿主，发送 `WM_COPYDATA`：`dwData = 0x4B4C5244`（"DRLK"），数据是 UTF-8 JSON `{"word": "...", "x": 屏幕 x, "y": 屏幕 y}`，弹窗在该点旁打开。常量见 `app/src/popup.h`。
  - 发送方先对 DR Dict 进程调用 `AllowSetForegroundWindow`，弹窗才能取得焦点；uiAccess 版本用 `ChangeWindowMessageFilterEx` 放行 `WM_COPYDATA`。
  - DR Dict 未运行时，用 `dr-dict.exe --lookup <词>` 启动：页面就绪、弹窗置顶后再显示（约 1.5 秒）。若已有实例在运行，`--lookup` 只把词转交给它，然后退出。
  - DR Dict 每次启动都会把自身路径写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\dr-dict.exe`，其他程序可以直接用 `ShellExecute("dr-dict.exe")` 启动它。这里记录的是最近一次运行的那个副本。

## 在线来源

网络请求都在 `Application::online_`（3 线程）上进行，不占用查词的 `queries_`。底层是 `app/src/http.cpp` 的 WinHTTP 客户端：只连白名单主机，不跟随重定向，响应限长，退出时 `HttpCancel` 关闭进行中的请求。协议与解析在 `engine/src/online.cpp`，以注入的 `HttpFetch` 工作，测试不联网。

- **小牛翻译**（`NiuTrans`）：v2 文本接口，与 DR Music `native/niutrans.cpp` 相同的签名方式（`authStr` = 按键名排序的 `key=value`（含 apikey）用 `&` 连接后取 MD5）。
  - 只做中英互译：汉字数 × 3 ≥ 拉丁字母数时中译英，否则英译中（`translation_direction`）。
  - 页面判断“长句”（`web/online.ts` 的 `isSentence`），长句以及词典里查不到的词才翻译；译文在内存中按原文缓存。
  - App ID / API Key 用 DPAPI 加密保存在 `<数据目录>/niutrans-key.bin`，页面只能拿到 `{configured, appId}`。
- **维基词典**（`Wiktionary`）：思路参考 GoldenDict-ng `src/dict/mediawiki.cc`，独立实现。
  - 用 `action=parse&redirects` 取页面，页面不存在时再试小写或首字母大写。超过 80 字的查询不发送。
  - 只保留相关语言的二级章节（英文版保留 English、Translingual、Chinese；中文版保留漢語、英語、跨語言），一个都没有时整页保留。
  - 改写时去掉脚本；条目链接改为 `entry://`，其他维基页面和以 `//`、`/` 开头的地址改为绝对 https 地址。
  - 在 `https://dict.dr-dict.local/@wiki-<en|zh>/@article?word=…` 输出，页面带 CSP：只允许本程序的脚本，媒体只能来自 `*.wikimedia.org`。样式和折叠框由 `wiki.css` / `wiki.js` 提供。

## 安全边界

- 每个 WebView 的主框架只允许导航到它自己的页面（主窗口 `index.html`，弹窗 `popup.html`）；子框架只允许 `dict.dr-dict.local`；禁止新窗口。
- 原生端只接受来自这两个页面的消息，并把回复发回给发出请求的 WebView；外链仅限 http/https/mailto。
- 词条 iframe `sandbox="allow-scripts allow-same-origin"`，与界面不同源。
