# Converts to PNG the BMP screenshots saved by the port (GAME --input ... shot FILE.bmp).
# Usage: bmp2png.ps1 FILE.bmp [FILE2.bmp ...]   (wildcards are accepted too)
param([Parameter(ValueFromRemainingArguments)] [string[]] $Files)
Add-Type -AssemblyName System.Drawing
foreach ($f in $Files) {
	foreach ($p in (Get-ChildItem $f)) {
		$b = [Drawing.Bitmap]::FromFile($p.FullName)
		$out = [IO.Path]::ChangeExtension($p.FullName, '.png')
		$b.Save($out, [Drawing.Imaging.ImageFormat]::Png)
		$b.Dispose()
		$out
	}
}
