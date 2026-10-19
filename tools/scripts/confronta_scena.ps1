# Phase 0.5: automatic port / original comparison on a test of tools\eventi\.
# 1. the port runs the test (fixed seed and step) and saves the screenshots in build\confronto\<name>\p\;
# 2. the original runs under OTVDM with the same events (orig_play.ps1) and the same starting INI
#    (rif.ini copied over the OTVDM INI, then restored), screenshots in ...\o\;
# 3. compare_shots.ps1 compares the pairs and writes the sheets and the result in ...\diff\.
# In the event file, besides "#! expect" and "#! ini" (Italian "#! atteso" also accepted):
#   #! mask: X Y W H       area excluded from the comparison (random animations, cursor...); several lines
#   #! limit: P            maximum percentage of differing pixels (default 3)
#   #! tolerance: T        maximum per-channel difference (default 40)
#   (Italian keywords still accepted: maschera, limite, tolleranza, seme; English alias: seed)
# The original's timing does not match the port's: before each "shot" it is advisable to use a
# "waitpix" on a pixel that appears only when the screen is ready (same command in both).
# Usage: confronta_scena.ps1 -Scene name [-SkipOrig] [-SkipPort]
param(
	[Parameter(Mandatory)] [string] $Scene,
	[switch] $SkipOrig,
	[switch] $SkipPort
)
. (Join-Path $PSScriptRoot 'game.ps1')
$root = Get-GameRoot
$portexe = Join-Path $root "build\$((Get-Game $root).NAME).exe"
$evfile = Join-Path $root "tools\eventi\$Scene.txt"
if (-not (Test-Path $evfile)) { throw "test does not exist: $evfile" }
$out = Join-Path $root "build\confronto\$Scene"
$pdir = Join-Path $out 'p'; $odir = Join-Path $out 'o'; $ddir = Join-Path $out 'diff'
$lines = Get-Content $evfile
$ini = 'rif.ini'; $seed = 1234; $masks = @(); $limit = 3.0; $tol = 40
foreach ($l in $lines) {
	if ($l -match '^#!\s*ini:\s*(\S+)') { $ini = $Matches[1] }
	elseif ($l -match '^#!\s*(?:seme|seed):\s*(\d+)') { $seed = [int]$Matches[1] }
	elseif ($l -match '^#!\s*(?:maschera|mask):\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)') { $masks += "$($Matches[1]),$($Matches[2]),$($Matches[3]),$($Matches[4])" }
	elseif ($l -match '^#!\s*(?:limite|limit):\s*([\d.]+)') { $limit = [double]$Matches[1] }
	elseif ($l -match '^#!\s*(?:tolleranza|tolerance):\s*(\d+)') { $tol = [int]$Matches[1] }
}
$rif = Join-Path $root "tools\eventi\$ini"

function Write-Events($dir, $ext) {
	New-Item -ItemType Directory -Force $dir | Out-Null
	Get-ChildItem $dir -File | Remove-Item -Force
	$f = Join-Path $out "eventi_$ext.txt"
	$lines | ForEach-Object {
		if ($_ -match '^(\d+)\s+shot\s+(\S+)') {
			"$($Matches[1]) shot " + (Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($Matches[2]) + ".$ext"))
		} else { $_ }
	} | Set-Content -Encoding ASCII $f
	return $f
}

New-Item -ItemType Directory -Force $out | Out-Null
if (-not $SkipPort) {
	$ev = Write-Events $pdir 'bmp'
	$testini = Join-Path $out 'port.ini'
	Copy-Item $rif $testini -Force
	$argList = @('--game', '--800', '--seed', "$seed", '--fixed-step', '--fast', '--ini', "`"$testini`"",
		'--input', "`"$ev`"", '--trace', "`"$(Join-Path $out 'traccia.txt')`"")
	Start-Process -FilePath $portexe -ArgumentList $argList -WorkingDirectory $root `
		-NoNewWindow -Wait -RedirectStandardOutput (Join-Path $out 'port_stdout.txt') -RedirectStandardError (Join-Path $out 'port_stderr.txt')
	Write-Output "port: $((Get-ChildItem $pdir -Filter *.bmp).Count) screenshots"
	Get-Content (Join-Path $out 'port_stdout.txt') | Where-Object { $_ -match '^waitpix' } | ForEach-Object { "  port $_" }
}
if (-not $SkipOrig) {
	$ev = Write-Events $odir 'png'
	$otini = Get-ChildItem (Join-Path $root 'tools\otvdm') -Recurse -Filter 7THLEVEL.INI | Select-Object -First 1
	$otini = if ($otini) { $otini.FullName } else { Join-Path $root 'tools\otvdm\otvdm-v0.9.0\WINDOWS\7THLEVEL.INI' }
	$bak = "$otini.confronto"
	if (Test-Path $otini) { Copy-Item $otini $bak -Force }
	Copy-Item $rif $otini -Force
	try {
		& (Join-Path $PSScriptRoot 'orig_play.ps1') -EventFile $ev -RealMouse | ForEach-Object { "  original $_" }
	}
	finally {
		if (Test-Path $bak) { Move-Item $bak $otini -Force } else { Remove-Item $otini -Force }
	}
	Write-Output "original: $((Get-ChildItem $odir -Filter *.png).Count) screenshots"
}
& (Join-Path $PSScriptRoot 'compare_shots.ps1') -Orig $odir -Port $pdir -Out $ddir -Tol $tol -Limit $limit -Masks ($masks -join ';')
$code = $LASTEXITCODE
Write-Output "comparison sheets: $ddir"
exit $code
