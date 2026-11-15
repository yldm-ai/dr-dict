# 应用 Logo

- `dr-dict.svg`：原始矢量 Logo，页面侧栏、首页和 favicon 直接使用。
- `dr-dict.png`：1024 × 1024 透明背景，等比居中、四周留白约 3%，用于通用图片场景。
- `dr-dict.ico`：Windows 应用图标，包含 16、20、24、28、32、36、40、44、48、64、128、256 像素帧，由 `app/app.rc` 嵌入程序。

原始 SVG 保持原有比例和颜色。PNG 和 ICO 使用透明正方形画布，避免拉伸。

项目根目录下运行以下命令重新生成；需要 ImageMagick（`magick` 在 PATH 中）：

```powershell
./scripts/convert-png-to-ico.ps1 ./resources/dr-dict.svg ./resources/dr-dict.ico -Force
magick -background none -density 192 ./resources/dr-dict.svg -resize 962x962 -background none -gravity center -extent 1024x1024 -depth 8 ./resources/dr-dict.png
```

转换脚本移植自 `MSIME-Windows/scripts/convert-png-to-ico.ps1`，保留 PNG/SVG 输入、多尺寸生成和帧校验功能。
