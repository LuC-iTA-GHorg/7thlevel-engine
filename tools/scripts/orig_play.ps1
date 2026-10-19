# Graphic reference: starts the ORIGINAL game (Win16) under portable OTVDM and replays the same
# timed event file used by the port (GAME --game --input FILE), to compare the screenshots.
# Format (one line per event, '#' = comment; MS = milliseconds since start):
#   MS key VK | keydown VK | keyup VK          (VK = Windows virtual key code, e.g. 32 or 0x1B)
#   MS click X Y | rclick X Y | move X Y       (logical screen 800x600 or 640x480, 0,0 at top left;
#                                               the click releases the button 100 ms later)
#   MS char CODE | text WORD                    (WM_CHAR)
#   MS waitpix X Y RRGGBB [TOL] [MAX]          (events stop until the pixel has that colour, at
#                                               most MAX ms; the following times are shifted
#                                               by the wait, as in the port)
#   MS shot FILE.png | quit
# Lines that only the port understands (focus, script, global) are ignored. The OTVDM screen is
# the real one (e.g. 1920x1080, client area 1921x1081): the central 800x600 area is cropped.
# Keys and mouse reach the window as messages (PostMessage: the real mouse does not move);
# the game's GetKeyState does not see them. With -RealMouse the real cursor also goes to the click
# or move point (the game's cursor follows the real position): do not touch the mouse during the run.
# OTVDM is used ONLY as a reference: the port does not use it.
# Usage: orig_play.ps1 -EventFile events.txt [-Exe GAME_EXE of build\game.h] [-Keep] [-RealMouse]
param(
	[Parameter(Mandatory)] [string] $EventFile,
	[string] $Exe = '',
	[switch] $Keep,
	[switch] $RealMouse
)
. (Join-Path $PSScriptRoot 'game.ps1')
$root = Get-GameRoot
if (-not $Exe) { $Exe = Join-Path $root (Get-Game $root).EXE }
$otvdm = Get-ChildItem (Join-Path $root 'tools\otvdm') -Recurse -Filter otvdmw.exe | Select-Object -First 1
if (-not $otvdm) { throw 'OTVDM not found in tools\otvdm' }
Add-Type -AssemblyName System.Drawing
if (-not ('OrigPlay' -as [type])) {
	Add-Type @'
using System; using System.Runtime.InteropServices;
public class OrigPlay {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
	[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
	[DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
	[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
	[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
'@
}

function Get-Origin($h) {
	# game area: the client area, centred at 800x600 if larger (as in orig_shots.ps1)
	$r = New-Object OrigPlay+RECT
	[OrigPlay]::GetClientRect($h, [ref]$r) | Out-Null
	$pt = New-Object OrigPlay+POINT
	[OrigPlay]::ClientToScreen($h, [ref]$pt) | Out-Null
	$cw = $r.Right; $ch = $r.Bottom
	if ($cw -gt 800 -and $ch -gt 600) { $pt.X += [int](($cw - 800) / 2); $pt.Y += [int](($ch - 600) / 2); $cw = 800; $ch = 600 }
	return @{ X = $pt.X; Y = $pt.Y; W = $cw; H = $ch; CW = $r.Right; CH = $r.Bottom }
}

function Send-Mouse($h, [uint32]$msg, [int]$x, [int]$y, [int]$wp) {
	$o = Get-Origin $h
	$p = New-Object OrigPlay+POINT
	$p.X = $o.X + $x; $p.Y = $o.Y + $y
	if ($RealMouse) { [OrigPlay]::SetCursorPos($p.X, $p.Y) | Out-Null }
	$t = [OrigPlay]::WindowFromPoint($p)
	if ($t -eq [IntPtr]::Zero) { $t = $h }
	[OrigPlay]::ScreenToClient($t, [ref]$p) | Out-Null
	$lp = [IntPtr](($p.Y -shl 16) -bor ($p.X -band 0xFFFF))
	[OrigPlay]::PostMessage($t, $msg, [IntPtr]$wp, $lp) | Out-Null
	return $t
}

function Send-Key($h, [uint32]$msg, [int]$vk) {
	$lp = if ($msg -eq 0x101) { [IntPtr]0xC0000001 } else { [IntPtr]1 }
	[OrigPlay]::PostMessage($h, $msg, [IntPtr]$vk, $lp) | Out-Null
}

function Parse-Int([string]$s) { if ($s -match '^0x') { [Convert]::ToInt32($s.Substring(2), 16) } else { [int]$s } }

$events = Get-Content $EventFile | Where-Object { $_ -match '\S' -and $_ -notmatch '^\s*#' } | ForEach-Object {
	$f = $_.Trim() -split '\s+'
	[pscustomobject]@{ Ms = [int]$f[0]; Cmd = $f[1]; Args = $f[2..($f.Count - 1)] }
} | Sort-Object Ms

$p = Start-Process $otvdm.FullName -ArgumentList "`"$((Resolve-Path $Exe).Path)`"" -WorkingDirectory (Split-Path (Resolve-Path $Exe).Path) -PassThru
try {
	$h = [IntPtr]::Zero
	for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) {
		Start-Sleep -Milliseconds 200
		$w = Get-Process otvdmw -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
		if ($w) { $h = $w.MainWindowHandle }
	}
	if ($h -eq [IntPtr]::Zero) { throw 'the original game window did not appear' }
	$clock = [Diagnostics.Stopwatch]::StartNew()
	$shift = 0 # sum of the waitpix waits
	$lastTarget = $h
	[OrigPlay]::SetForegroundWindow($h) | Out-Null
	foreach ($e in $events) {
		if ($e.Cmd -eq 'quit') { break }
		while ($clock.ElapsedMilliseconds - $shift -lt $e.Ms) { Start-Sleep -Milliseconds 10 }
		# the main window can change (the game creates a new one): look for it again
		$w = Get-Process otvdmw -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
		if ($w) { $h = $w.MainWindowHandle }
		switch ($e.Cmd) {
			'key'     { $vk = Parse-Int $e.Args[0]; Send-Key $lastTarget 0x100 $vk; Start-Sleep -Milliseconds 50; Send-Key $lastTarget 0x101 $vk }
			'keydown' { Send-Key $lastTarget 0x100 (Parse-Int $e.Args[0]) }
			'keyup'   { Send-Key $lastTarget 0x101 (Parse-Int $e.Args[0]) }
			'move'    { $lastTarget = Send-Mouse $h 0x200 (Parse-Int $e.Args[0]) (Parse-Int $e.Args[1]) 0 }
			'click'   { $x = Parse-Int $e.Args[0]; $y = Parse-Int $e.Args[1]
			            $lastTarget = Send-Mouse $h 0x200 $x $y 0
			            Send-Mouse $h 0x201 $x $y 1 | Out-Null; Start-Sleep -Milliseconds 100; Send-Mouse $h 0x202 $x $y 0 | Out-Null }
			'rclick'  { $x = Parse-Int $e.Args[0]; $y = Parse-Int $e.Args[1]
			            Send-Mouse $h 0x204 $x $y 2 | Out-Null; Start-Sleep -Milliseconds 100; Send-Mouse $h 0x205 $x $y 0 | Out-Null }
			'char'    { [OrigPlay]::PostMessage($lastTarget, 0x102, [IntPtr](Parse-Int $e.Args[0]), [IntPtr]1) | Out-Null }
			'text'    { foreach ($ch in $e.Args[0].ToCharArray()) { [OrigPlay]::PostMessage($lastTarget, 0x102, [IntPtr][int]$ch, [IntPtr]1) | Out-Null } }
			'waitpix' { $x = Parse-Int $e.Args[0]; $y = Parse-Int $e.Args[1]; $want = [Convert]::ToInt32($e.Args[2], 16)
			            $tol = if ($e.Args.Count -gt 3) { [int]$e.Args[3] } else { 24 }
			            $max = if ($e.Args.Count -gt 4) { [int]$e.Args[4] } else { 30000 }
			            $t0 = $clock.ElapsedMilliseconds; $ok = $false
			            $px = New-Object Drawing.Bitmap 1, 1; $gp = [Drawing.Graphics]::FromImage($px)
			            while (-not $ok -and $clock.ElapsedMilliseconds - $t0 -lt $max) {
			                $o = Get-Origin $h
			                $gp.CopyFromScreen($o.X + $x, $o.Y + $y, 0, 0, $px.Size)
			                $c = $px.GetPixel(0, 0)
			                $d = [Math]::Max([Math]::Abs($c.R - (($want -shr 16) -band 255)), [Math]::Max([Math]::Abs($c.G - (($want -shr 8) -band 255)), [Math]::Abs($c.B - ($want -band 255))))
			                $ok = if ($tol -ge 0) { $d -le $tol } else { $d -gt -$tol } # TOL < 0: different colour
			                if (-not $ok) { Start-Sleep -Milliseconds 30 }
			            }
			            $gp.Dispose(); $px.Dispose()
			            $shift += $clock.ElapsedMilliseconds - $shift - $e.Ms
			            "$($e.Ms) ms: waitpix $(if ($ok) { 'found' } else { 'TIMED OUT' }) after$($clock.ElapsedMilliseconds - $t0) ms" }
			'shot'    { [OrigPlay]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 100
			            $o = Get-Origin $h
			            $bmp = New-Object Drawing.Bitmap $o.W, $o.H
			            [Drawing.Graphics]::FromImage($bmp).CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
			            $bmp.Save($e.Args[0], [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
			            "$($e.Ms) ms: $($e.Args[0]) (client area $($o.CW)x$($o.CH), taken at$($clock.ElapsedMilliseconds - $shift) ms)" }
		}
	}
}
finally {
	if (-not $Keep) {
		Get-Process otvdmw, otvdm -ErrorAction SilentlyContinue | Stop-Process -Confirm:$false -ErrorAction SilentlyContinue
	}
}
