# 项目协作规则

## GoldenDict-ng 参考要求

- 处理词典格式解析（MDict、StarDict 等）、编码、词头折叠、资源查找、重定向与文章渲染兼容性问题时，先查阅 `C:/Users/suxiaofan/EDisk/CppCodes/RefProjects/goldendict-ng` 中对应实现（如 `src/dict/mdictparser.cc`、`src/dict/mdx.cc`、`src/dict/stardict.cc`），理解其处理顺序与边界，再确定本项目方案。
- 采用参考策略时，在代码注释或工作说明中指出具体参考文件或函数，并说明本项目如何独立实现。
- GoldenDict-ng 为 GPL 许可：只参考思路，独立实现；不要复制其源码，也不要让它成为运行时依赖。
- 用触发问题的真实词典复现并验证（可用 `dr-dict-cli`），同时补充 `tests/engine_tests.cpp` 中的回归测试（以合成的小词典文件为主）。词典原文件始终只读。

## UI Design

Design philosophy:
- Notion-inspired minimalism
- Content-first layout
- Neutral color palette
- Precise typography and spacing
- Subtle interactive feedback
- Consistent component system

Avoid:
- Generic AI-generated dashboard aesthetics
- Excessive cards and borders
- Gradients and glassmorphism
- Oversized border-radius
- Decorative shadows and animations

## 功能实现的规则

遇到不熟悉的功能，不要立即从零实现。先检索维护活跃、许可证明确的同类开源项目，比较实现思路、依赖和已知问题。优先借鉴经过验证的设计，不要直接复制不兼容或来源不明的代码。
