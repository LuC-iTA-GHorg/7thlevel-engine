# Automatic screenshot comparison: original (OTVDM) against port, with tolerance and masks.
# For each port screenshot (BMP) it looks for the original's one with the same name (PNG or BMP),
# counts the differing pixels (maximum per-channel difference > Tol, outside the masks) and saves a
# sheet "original | port | differences" (differing pixels in red, masked areas in blue).
# Usage: compare_shots.ps1 -Orig dir -Port dir -Out dir [-Tol 40] [-Limit 3.0] [-Masks 'x,y,w,h;...']
# Result per image: OK if the differing pixels are at most Limit %. Exit code 1 if an image
# exceeds the limit or is missing. Summary also in Out\esito.txt.
param(
	[Parameter(Mandatory)] [string] $Orig,
	[Parameter(Mandatory)] [string] $Port,
	[Parameter(Mandatory)] [string] $Out,
	[int] $Tol = 40,
	[double] $Limit = 3.0,
	[string] $Masks = ''
)
Add-Type -AssemblyName System.Drawing
if (-not ('ShotDiff' -as [type])) {
	Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class ShotDiff {
	static byte[] Bytes(Bitmap b, out int stride) {
		var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
		stride = d.Stride; var a = new byte[stride * b.Height];
		Marshal.Copy(d.Scan0, a, 0, a.Length); b.UnlockBits(d); return a;
	}
	// masks: x,y,w,h repeated; returns the differing pixels and the compared pixels; diff = map
	public static int[] Compare(Bitmap o, Bitmap p, int tol, int[] masks, Bitmap diff) {
		int w = Math.Min(o.Width, p.Width), h = Math.Min(o.Height, p.Height), so, sp;
		byte[] a = Bytes(o, out so), b = Bytes(p, out sp);
		var dd = diff.LockBits(new Rectangle(0, 0, diff.Width, diff.Height), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
		var m = new byte[dd.Stride * diff.Height];
		int bad = 0, total = 0;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				int i = y * so + x * 3, j = y * sp + x * 3, k = y * dd.Stride + x * 3;
				bool masked = false;
				for (int q = 0; q + 3 < masks.Length; q += 4)
					if (x >= masks[q] && y >= masks[q + 1] && x < masks[q] + masks[q + 2] && y < masks[q + 1] + masks[q + 3]) { masked = true; break; }
				int g = (b[j] + b[j + 1] + b[j + 2]) / 9; // port in dark grey as the map background
				if (masked) { m[k] = (byte)Math.Min(255, g + 120); m[k + 1] = (byte)g; m[k + 2] = (byte)g; continue; }
				total++;
				int d = Math.Max(Math.Abs(a[i] - b[j]), Math.Max(Math.Abs(a[i + 1] - b[j + 1]), Math.Abs(a[i + 2] - b[j + 2])));
				if (d > tol) { bad++; m[k] = 0; m[k + 1] = 0; m[k + 2] = 255; }
				else { m[k] = (byte)g; m[k + 1] = (byte)g; m[k + 2] = (byte)g; }
			}
		Marshal.Copy(m, 0, dd.Scan0, m.Length); diff.UnlockBits(dd);
		return new int[] { bad, total, w, h };
	}
}
'@
}
function Load($path) {
	$img = [Drawing.Image]::FromFile($path)
	$bmp = New-Object Drawing.Bitmap $img.Width, $img.Height, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
	$g = [Drawing.Graphics]::FromImage($bmp); $g.DrawImage($img, 0, 0, $img.Width, $img.Height); $g.Dispose()
	$img.Dispose()
	return $bmp
}
$m = @()
foreach ($r in ($Masks -split ';' | Where-Object { $_ -match '\d' })) { $m += @($r -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ }) }
New-Item -ItemType Directory -Force $Out | Out-Null
$rows = @(); $fail = 0
foreach ($pf in (Get-ChildItem $Port -Filter *.bmp | Sort-Object Name)) {
	$base = $pf.BaseName
	$of = @('png', 'bmp') | ForEach-Object { Join-Path $Orig "$base.$_" } | Where-Object { Test-Path $_ } | Select-Object -First 1
	if (-not $of) { $rows += ('{0,-20} MISSING the original' -f $base); $fail++; continue }
	$o = Load $of; $p = Load $pf.FullName
	$diff = New-Object Drawing.Bitmap $p.Width, $p.Height, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
	$res = [ShotDiff]::Compare($o, $p, $Tol, [int[]]$m, $diff)
	$pc = if ($res[1]) { 100.0 * $res[0] / $res[1] } else { 0 }
	$ok = ($pc -le $Limit) -and ($o.Width -eq $p.Width) -and ($o.Height -eq $p.Height)
	if (-not $ok) { $fail++ }
	$state = if ($ok) { 'OK' } else { 'DIFFERENT' }
	$size = if ($o.Width -ne $p.Width -or $o.Height -ne $p.Height) { " (sizes $($o.Width)x$($o.Height) / $($p.Width)x$($p.Height))" } else { '' }
	$rows += ('{0,-20} {1,-8} {2,6:N2} % different pixels{3}' -f $base, $state, $pc, $size)
	# sheet: original | port | differences, at half size
	$tw = [int]($p.Width / 2); $th = [int]($p.Height / 2)
	$sheet = New-Object Drawing.Bitmap ($tw * 3 + 8), ($th + 20)
	$g = [Drawing.Graphics]::FromImage($sheet); $g.Clear([Drawing.Color]::FromArgb(40, 40, 40))
	$g.InterpolationMode = 'HighQualityBilinear'
	$g.DrawImage($o, 0, 20, $tw, $th); $g.DrawImage($p, $tw + 4, 20, $tw, $th); $g.DrawImage($diff, 2 * $tw + 8, 20, $tw, $th)
	$font = New-Object Drawing.Font 'Arial', 9
	$g.DrawString("original",$font, [Drawing.Brushes]::White, 2, 3)
	$g.DrawString("port", $font, [Drawing.Brushes]::White, $tw + 6, 3)
	$g.DrawString(("{0}: {1:N2} % different (tolerance {2})" -f $base, $pc, $Tol), $font, [Drawing.Brushes]::White, 2 * $tw + 10, 3)
	$g.Dispose()
	$sheet.Save((Join-Path $Out "$base.png"), [Drawing.Imaging.ImageFormat]::Png)
	$sheet.Dispose(); $diff.Dispose(); $o.Dispose(); $p.Dispose()
}
$rows | Set-Content -Encoding UTF8 (Join-Path $Out 'esito.txt')
$rows
if ($fail) { exit 1 }
