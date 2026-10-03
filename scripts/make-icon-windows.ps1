# Renders apps/windows/res/Procyon.ico from the shared app icon (apps/macos/Resources/AppIcon.png).
# Every entry is PNG-compressed (supported since Windows Vista), at the sizes Explorer and the
# taskbar ask for.
#
#   powershell -ExecutionPolicy Bypass -File scripts/make-icon-windows.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$source = Join-Path $root "apps/macos/Resources/AppIcon.png"
$target = Join-Path $root "apps/windows/res/Procyon.ico"
$sizes = @(256, 64, 48, 32, 24, 16)

$image = [System.Drawing.Image]::FromFile($source)
$entries = @()
foreach ($size in $sizes) {
    $bitmap = New-Object System.Drawing.Bitmap $size, $size
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($image, 0, 0, $size, $size)
    $graphics.Dispose()
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    $entries += ,@{ Size = $size; Bytes = $stream.ToArray() }
    $stream.Dispose()
}
$image.Dispose()

$output = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $output
# ICONDIR: reserved, type 1 (icon), count.
$writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($entry in $entries) {
    $dimension = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
    $writer.Write([Byte]$dimension); $writer.Write([Byte]$dimension)  # width, height (0 = 256)
    $writer.Write([Byte]0); $writer.Write([Byte]0)                    # palette, reserved
    $writer.Write([UInt16]1); $writer.Write([UInt16]32)               # planes, bits per pixel
    $writer.Write([UInt32]$entry.Bytes.Length); $writer.Write([UInt32]$offset)
    $offset += $entry.Bytes.Length
}
foreach ($entry in $entries) { $writer.Write($entry.Bytes) }
$writer.Flush()
New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
[System.IO.File]::WriteAllBytes($target, $output.ToArray())
$writer.Dispose()
Write-Host "wrote $target ($($output.Length) bytes)"
