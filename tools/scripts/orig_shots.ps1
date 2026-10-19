# Graphic reference: starts the ORIGINAL game (Win16) under portable OTVDM and captures a series
# of screenshots of its window, to compare them with the port (screenshot.ps1).
# OTVDM is not installed: otvdmw.exe is launched with the path of the executable.
# Usage: orig_shots.ps1 -OutDir folder [-Exe GAME_EXE of build\game.h] [-Count 10] [-EveryMs 2000] [-StartMs 3000]
#        [-Keep] (leaves the game open) [-Attach] (does not launch, uses the already open window)
param(
	[Parameter(Mandatory)] [string] $OutDir,
	[string] $Exe = '',
	[int] $Count = 10,
	[int] $EveryMs = 2000,
	[int] $StartMs = 3000,
	[switch] $Keep,
	[switch] $Attach
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $PSScriptRoot 'game.ps1')
if (-not $Exe) { $Exe = Join-Path $root (Get-Game $root).EXE }
$otvdm = Get-ChildItem (Join-Path $root 'tools\otvdm') -Recurse -Filter otvdmw.exe | Select-Object -First 1
if (-not $otvdm) { throw 'OTVDM not found in tools\otvdm' }
Add-Type -AssemblyName System.Drawing
if (-not ('OrigWin' -as [type])) {
	Add-Type @'
using System; using System.Runtime.InteropServices;
public class OrigWin {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
	[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Attach) {
	$p = Start-Process $otvdm.FullName -ArgumentList "`"$((Resolve-Path $Exe).Path)`"" -WorkingDirectory (Split-Path (Resolve-Path $Exe).Path) -PassThru
}
try {
	$h = [IntPtr]::Zero
	for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) {
		Start-Sleep -Milliseconds 200
		$w = Get-Process otvdmw -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
		if ($w) { $h = $w.MainWindowHandle }
	}
	if ($h -eq [IntPtr]::Zero) { throw 'the original game window did not appear' }
	Start-Sleep -Milliseconds $StartMs
	for ($k = 0; $k -lt $Count; $k++) {
		[OrigWin]::SetForegroundWindow($h) | Out-Null
		$r = New-Object OrigWin+RECT
		[OrigWin]::GetClientRect($h, [ref]$r) | Out-Null
		$pt = New-Object OrigWin+POINT
		[OrigWin]::ClientToScreen($h, [ref]$pt) | Out-Null
		# the full-screen game is centred in the client area: crop 800x600 (or 640x480)
		$cw = $r.Right; $ch = $r.Bottom
		if ($cw -gt 800 -and $ch -gt 600) { $pt.X += [int](($cw - 800) / 2); $pt.Y += [int](($ch - 600) / 2); $cw = 800; $ch = 600 }
		$bmp = New-Object Drawing.Bitmap $cw, $ch
		[Drawing.Graphics]::FromImage($bmp).CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
		$f = Join-Path $OutDir ('orig_{0:D2}.png' -f $k)
		$bmp.Save($f, [Drawing.Imaging.ImageFormat]::Png)
		"$f ($($r.Right)x$($r.Bottom))"
		if ($k -lt $Count - 1) { Start-Sleep -Milliseconds $EveryMs }
	}
}
finally {
	if (-not $Keep -and -not $Attach) {
		Get-Process otvdmw, otvdm -ErrorAction SilentlyContinue | Stop-Process -Confirm:$false -ErrorAction SilentlyContinue
	}
}
