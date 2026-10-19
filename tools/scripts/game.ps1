# Game profile read for the scripts from build\game.h (copied there by build.bat from games\<name>.h;
# lines '#define GAME_X "value"').
# Usage (dot-source):  . (Join-Path $PSScriptRoot 'game.ps1'); $root = Get-GameRoot; $g = Get-Game $root
#   $g.NAME (e.g. bowl), $g.EXE (e.g. TBOWLPC.EXE), $g.START, $g.TITLE, ...
#   Join-Path $root "build\$($g.NAME).exe" = the port executable
function Get-Game([string] $Root) {
	$h = Join-Path $Root 'build\game.h'
	if (-not (Test-Path $h)) { throw "missing $h (run build.bat first)" }
	$g = @{}
	foreach ($m in [regex]::Matches((Get-Content $h -Raw), '(?m)^#define GAME_(\w+)\s+"([^"]*)"')) {
		$g[$m.Groups[1].Value] = $m.Groups[2].Value
	}
	$g
}

# Folder of the game project: GAME_ROOT if set, otherwise the current folder if it contains
# build\game.h (a game repository with the engine in engine\), otherwise the engine folder.
function Get-GameRoot {
	if ($env:GAME_ROOT) { return (Resolve-Path $env:GAME_ROOT).Path }
	if (Test-Path (Join-Path (Get-Location) 'build\game.h')) { return (Get-Location).Path }
	(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
