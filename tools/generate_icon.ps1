# Regenerate the vector mark and Windows icon without third-party tools.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assetDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../resources'))
$svg = @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <rect x="8" y="8" width="240" height="240" rx="56" fill="#2b6f62"/>
  <path d="M128 86C106 72 82 72 58 82V178C82 168 106 168 128 182C150 168 174 168 198 178V82C174 72 150 72 128 86ZM128 86V182" fill="none" stroke="#ffffff" stroke-width="11" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M158 77V122L170 112L182 122V74" fill="#ffffff"/>
</svg>
'@
[IO.File]::WriteAllText((Join-Path $assetDirectory 'dr-dict.svg'), $svg + "`n", [Text.UTF8Encoding]::new($false))
$background = [Drawing.Drawing2D.GraphicsPath]::new()
$background.AddArc(8, 8, 112, 112, 180, 90)
$background.AddArc(136, 8, 112, 112, 270, 90)
$background.AddArc(136, 136, 112, 112, 0, 90)
$background.AddArc(8, 136, 112, 112, 90, 90)
$background.CloseFigure()
$book = [Drawing.Drawing2D.GraphicsPath]::new()
$book.AddBezier(128, 86, 106, 72, 82, 72, 58, 82)
$book.AddLine(58, 82, 58, 178)
$book.AddBezier(58, 178, 82, 168, 106, 168, 128, 182)
$book.AddBezier(128, 182, 150, 168, 174, 168, 198, 178)
$book.AddLine(198, 178, 198, 82)
$book.AddBezier(198, 82, 174, 72, 150, 72, 128, 86)
$book.CloseFigure()
$book.StartFigure()
$book.AddLine(128, 86, 128, 182)
$brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#2b6f62'))
$pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#ffffff'), 11)
$pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
$pen.StartCap = $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = [Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $large = [Drawing.Bitmap]::new($size * 4, $size * 4)
    $graphics = [Drawing.Graphics]::FromImage($large)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size * 4 / 256.0, $size * 4 / 256.0)
    $graphics.FillPath($brush, $background)
    $graphics.DrawPath($pen, $book)
    $graphics.FillPolygon([Drawing.Brushes]::White, [Drawing.PointF[]]@([Drawing.PointF]::new(158, 77), [Drawing.PointF]::new(158, 122), [Drawing.PointF]::new(170, 112), [Drawing.PointF]::new(182, 122), [Drawing.PointF]::new(182, 74)))
    $graphics.Dispose()
    $bitmap = [Drawing.Bitmap]::new($size, $size)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.DrawImage($large, 0, 0, $size, $size)
    $graphics.Dispose()
    $large.Dispose()
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    $images.Add($stream.ToArray())
    $stream.Dispose()
    $bitmap.Dispose()
}
$stream = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($stream)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($bytes in $images) { $writer.Write($bytes) }
[IO.File]::WriteAllBytes((Join-Path $assetDirectory 'dr-dict.ico'), $stream.ToArray())
$writer.Dispose()
$stream.Dispose()
$pen.Dispose()
$brush.Dispose()
$book.Dispose()
$background.Dispose()
Write-Output "Generated dr-dict.svg and dr-dict.ico ($($sizes -join ', ') px)."
