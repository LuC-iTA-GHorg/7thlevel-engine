# Starts build\GAME.exe (GAME_NAME of build\game.h) with the given arguments, captures the window to PNG and closes it.
# Used to check the port on screen during development.
# Usage: screenshot.ps1 -Out shot.png [-ExeArgs '--800'] [-WaitMs 800]
param(
	[Parameter(Mandatory)] [string] $Out,
	[string] $ExeArgs = '',
	[int] $WaitMs = 800,
	[string] $Exe = '',
	[int] $Count = 1,        # >1: series of shots out_00.png, out_01.png... every EveryMs
	[int] $EveryMs = 2000,
	[switch] $Client         # client area only (no borders), to compare it with orig_shots.ps1
)
# PowerShell 5.1: $PSScriptRoot is not available in parameter defaults
. (Join-Path $PSScriptRoot 'game.ps1')
if (-not $Exe) {
	$r0 = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
	$Exe = Join-Path $r0 "build\$((Get-Game $r0).NAME).exe"
}
Add-Type -AssemblyName System.Drawing
if (-not ('PortWin' -as [type])) {
	Add-Type @'
using System; using System.Runtime.InteropServices;
public class PortWin {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
	[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
}
'@
}
$exe = (Resolve-Path $Exe).Path
$p = if ($ExeArgs) { Start-Process $exe -ArgumentList $ExeArgs -WorkingDirectory (Split-Path $exe) -PassThru }
	else { Start-Process $exe -WorkingDirectory (Split-Path $exe) -PassThru }
try {
	$h = [IntPtr]::Zero
	for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero -and -not $p.HasExited; $i++) {
		Start-Sleep -Milliseconds 200
		$p.Refresh()
		$h = $p.MainWindowHandle
	}
	if ($h -eq [IntPtr]::Zero) { throw "the window did not appear (process exited:$($p.HasExited))" }
	[PortWin]::SetForegroundWindow($h) | Out-Null
	Start-Sleep -Milliseconds $WaitMs
	for ($k = 0; $k -lt $Count; $k++) {
		[PortWin]::SetForegroundWindow($h) | Out-Null
		$r = New-Object PortWin+RECT
		if ($Client) {
			[PortWin]::GetClientRect($h, [ref]$r) | Out-Null
			$pt = New-Object PortWin+POINT
			[PortWin]::ClientToScreen($h, [ref]$pt) | Out-Null
			$r.Left = $pt.X; $r.Top = $pt.Y; $r.Right += $pt.X; $r.Bottom += $pt.Y
		}
		else { [PortWin]::GetWindowRect($h, [ref]$r) | Out-Null }
		$bmp = New-Object Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
		[Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
		$f = if ($Count -gt 1) { $Out -replace '\.png$', ('_{0:D2}.png' -f $k) } else { $Out }
		$bmp.Save($f, [Drawing.Imaging.ImageFormat]::Png)
		$p.Refresh()
		"$($p.MainWindowTitle) -> $f"
		if ($k -lt $Count - 1) { Start-Sleep -Milliseconds $EveryMs }
	}
}
finally {
	if (-not $p.HasExited) {
		$p.CloseMainWindow() | Out-Null
		if (-not $p.WaitForExit(2000)) { Stop-Process -Id $p.Id -Confirm:$false }
	}
}
