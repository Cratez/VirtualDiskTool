# Draws VirtualDiskTool.ico from the same shapes as icon.svg, using only what ships with Windows.
# The icon is checked in, so this only needs running to change it. Change icon.svg to match.

Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256

# Below this size the lines of the large design blur away, so it gets a heavier version.
$smallDesignLimit = 24

function Get-Color([string]$hex) {
	[System.Drawing.ColorTranslator]::FromHtml($hex)
}

function Get-BracePath([double]$spine, [double]$tip, [double]$top) {
	$bottom = 64 - $top
	$path = New-Object System.Drawing.Drawing2D.GraphicsPath
	$path.AddBezier(16, $top, 10, $top, $spine, ($top + 4), $spine, 16)
	$path.AddLine($spine, 16, $spine, 25)
	$path.AddBezier($spine, 25, $spine, 29, ($tip + 4), 32, $tip, 32)
	$path.AddBezier($tip, 32, ($tip + 4), 32, $spine, 35, $spine, 39)
	$path.AddLine($spine, 39, $spine, 48)
	$path.AddBezier($spine, 48, $spine, ($bottom - 4), 10, $bottom, 16, $bottom)
	$path
}

function New-IconImage([int]$size) {
	# The small numbers put the edges of the braces on whole pixels at 16, which keeps them crisp.
	$small = $size -le $smallDesignLimit
	$stroke = if ($small) { 8 } else { 6 }
	$spine = if ($small) { 8 } else { 9 }
	$tip = if ($small) { 4 } else { 3 }
	$top = if ($small) { 8 } else { 6 }
	$hole = if ($small) { 5.5 } else { 4.5 }

	$bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
	$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
	$graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
	$graphics.ScaleTransform($size / 64.0, $size / 64.0)

	$pen = New-Object System.Drawing.Pen((Get-Color '#1D9E75'), $stroke)
	$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
	$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
	$pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

	$left = Get-BracePath $spine $tip $top
	$right = Get-BracePath $spine $tip $top
	$right.Transform((New-Object System.Drawing.Drawing2D.Matrix(-1, 0, 0, 1, 64, 0)))
	$graphics.DrawPath($pen, $left)
	$graphics.DrawPath($pen, $right)

	$platter = New-Object System.Drawing.SolidBrush((Get-Color '#5DCAA5'))
	$spindle = New-Object System.Drawing.SolidBrush((Get-Color '#04342C'))
	$graphics.FillEllipse($platter, 18, 18, 28, 28)
	$graphics.FillEllipse($spindle, (32 - $hole), (32 - $hole), (2 * $hole), (2 * $hole))

	$graphics.Dispose()
	$bitmap
}

$images = foreach ($size in $sizes) {
	$bitmap = New-IconImage $size
	$stream = New-Object System.IO.MemoryStream
	$bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
	$bitmap.Dispose()
	[pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() }
}

$file = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($file)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$images.Count)

$directoryEntrySize = 16
$offset = 6 + $directoryEntrySize * $images.Count
foreach ($image in $images) {
	# A width or height of 256 doesn't fit in the byte, so the format stores it as zero.
	$dimension = [byte]($image.Size % 256)
	$writer.Write($dimension)
	$writer.Write($dimension)
	$writer.Write([byte]0)
	$writer.Write([byte]0)
	$writer.Write([uint16]1)
	$writer.Write([uint16]32)
	$writer.Write([uint32]$image.Bytes.Length)
	$writer.Write([uint32]$offset)
	$offset += $image.Bytes.Length
}
foreach ($image in $images) {
	$writer.Write($image.Bytes)
}
$writer.Flush()

[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'VirtualDiskTool.ico'), $file.ToArray())
