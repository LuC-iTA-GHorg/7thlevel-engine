# Side-by-side comparison original | port for the series produced by orig_play.ps1 and GAME --input.
# The files are in %TEMP%\inp\cmp\NAME_o_NN.png (original) and NAME_p_NN.bmp (port).
# Usage: compare_pair.ps1 -Name fin -Indices '0,4,8' [-Crop 'x,y,w,h'] [-TileW 600] [-Out sheet.png]
#        (-Crop zooms into an area; one row per index: original on the left, port on the right)
param(
	[Parameter(Mandatory)] [string] $Name,
	[Parameter(Mandatory)] [string] $Indices,
	[string] $Crop = '',
	[int] $TileW = 600,
	[string] $Out = ''
)
Add-Type -AssemblyName System.Drawing
$dir = Join-Path $env:TEMP 'inp\cmp'
if (-not $Out) { $Out = Join-Path $dir ("{0}_cmp.png" -f $Name) }
function Load($path) {
	$img = [Drawing.Image]::FromFile($path)
	$bmp = New-Object Drawing.Bitmap $img.Width, $img.Height
	[Drawing.Graphics]::FromImage($bmp).DrawImage($img, 0, 0, $img.Width, $img.Height)
	$img.Dispose()
	return $bmp
}
$idx = @($Indices.Split(',') | ForEach-Object { [int]$_ })
$cr = @(if ($Crop) { $Crop.Split(',') | ForEach-Object { [int]$_ } })
$rows = New-Object System.Collections.ArrayList
foreach ($i in $idx) {
	$o = Load (Join-Path $dir ("{0}_o_{1:D2}.png" -f $Name, $i))
	$p = Load (Join-Path $dir ("{0}_p_{1:D2}.bmp" -f $Name, $i))
	[void]$rows.Add(@($o, $p))
}
$w = $rows[0][0].Width; $h = $rows[0][0].Height
if ($cr.Count -eq 4) { $w = $cr[2]; $h = $cr[3] }
$tileH = [int]($TileW * $h / $w)
$sheet = New-Object Drawing.Bitmap (2 * $TileW + 6), ($rows.Count * ($tileH + 16))
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::DimGray)
$g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$font = New-Object Drawing.Font 'Arial', 9
for ($r = 0; $r -lt $rows.Count; $r++) {
	for ($c = 0; $c -lt 2; $c++) {
		$src = $rows[$r][$c]
		$sx = 0; $sy = 0; $sw = $src.Width; $sh = $src.Height
		if ($cr.Count -eq 4) { $sx = $cr[0]; $sy = $cr[1]; $sw = $cr[2]; $sh = $cr[3] }
		$dest = New-Object Drawing.Rectangle (($c * ($TileW + 6)), ($r * ($tileH + 16) + 16), $TileW, $tileH)
		$g.DrawImage($src, $dest, $sx, $sy, $sw, $sh, [Drawing.GraphicsUnit]::Pixel)
		$label = (@('original', 'port')[$c]) + ' #' + $idx[$r]
		$g.DrawString($label, $font, [Drawing.Brushes]::White, ($c * ($TileW + 6) + 2), ($r * ($tileH + 16)))
	}
}
$sheet.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$Out
