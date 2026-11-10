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

## 安全边界

- 主框架只允许导航到 `index.html`；子框架只允许 `dict.dr-dict.local`；禁止新窗口。
- 原生端只接受来自 `index.html` 的消息；外链仅限 http/https/mailto。
- 词条 iframe `sandbox="allow-scripts allow-same-origin"`，与界面不同源。
