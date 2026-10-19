# Joins several PNGs into a reduced grid (contact sheet), to compare sequences of screenshots.
# Usage: montage.ps1 -Out sheet.png -Files 'folder\*.png' [-Cols 4] [-TileW 400]
#        (-Files: paths separated by ';', wildcards allowed; sorted by name)
param(
	[Parameter(Mandatory)] [string] $Out,
	[Parameter(Mandatory)] [string] $Files,
	[int] $Cols = 4,
	[int] $TileW = 400
)
$list = @($Files.Split(';') | ForEach-Object { Get-ChildItem $_ } | Sort-Object Name | ForEach-Object FullName)
Add-Type -AssemblyName System.Drawing
$imgs = $list | ForEach-Object { [Drawing.Image]::FromFile((Resolve-Path $_).Path) }
$tileH = [int]($TileW * $imgs[0].Height / $imgs[0].Width)
$rows = [Math]::Ceiling($imgs.Count / $Cols)
$sheet = New-Object Drawing.Bitmap ($Cols * $TileW), ($rows * ($tileH + 14))
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::DimGray)
$font = New-Object Drawing.Font 'Arial', 8
for ($i = 0; $i -lt $imgs.Count; $i++) {
	$x = ($i % $Cols) * $TileW; $y = [Math]::Floor($i / $Cols) * ($tileH + 14)
	$g.DrawImage($imgs[$i], $x, $y + 14, $TileW, $tileH)
	$g.DrawString([IO.Path]::GetFileName($list[$i]), $font, [Drawing.Brushes]::White, $x + 2, $y)
}
$sheet.Save($Out, [Drawing.Imaging.ImageFormat]::Png)
$imgs | ForEach-Object { $_.Dispose() }
"$Out ($($imgs.Count) images)"
