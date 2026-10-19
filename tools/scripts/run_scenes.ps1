# Runs the repeatable scene tests (tools\eventi\*.txt) and merges the coverage.
# Each test: fresh copy of the reference INI, fixed seed, simulated time without waits,
# trace and coverage in build\prove\<name>\. In the event file:
#   #! expect: TEXT    the trace must contain TEXT (also several lines)   [Italian: #! atteso:]
#   #! ini: FILE       starting INI (in tools\eventi\; default rif.ini)
#   #! seed: N         rand() seed (default 1234)                          [Italian: #! seme:]
#   MS shot NAME.bmp   the screenshot ends up in build\prove\<name>\NAME.bmp
# Usage: run_scenes.ps1 [-Only name1,name2] [-Exe build\GAME.exe]
# Result: outcome table per test and build\prove\copertura.txt (union of all the tests).
param(
	[string[]] $Only = @(),
	[string] $Exe = ''
)
. (Join-Path $PSScriptRoot 'game.ps1')
$root = Get-GameRoot
if (-not $Exe) { $Exe = Join-Path $root "build\$((Get-Game $root).NAME).exe" }
$evdir = Join-Path $root 'tools\eventi'
$outroot = Join-Path $root 'build\prove'
New-Item -ItemType Directory -Force $outroot | Out-Null

$kinds = @('script', 'lists', 'sprite (type 17)', 'animations (type 15)')
$union = @{}   # library -> type -> HashSet[int] of the executed ids
$totals = @{}  # library -> type -> total present

function Add-Ids($lib, $kind, $text) {
	foreach ($tok in ($text -split '\s+')) {
		if ($tok -match '^(\d+)-(\d+)$') { $a = [int]$Matches[1]; $b = [int]$Matches[2] }
		elseif ($tok -match '^\d+$') { $a = [int]$tok; $b = $a }
		else { continue }
		for ($i = $a; $i -le $b; $i++) { [void]$union[$lib][$kind].Add($i) }
	}
}

# Reads a --coverage report; continuation lines start with 5 spaces.
function Read-Coverage($file) {
	$lib = $null; $kind = $null
	foreach ($line in Get-Content $file) {
		if ($line -match '^== total') { $lib = $null; continue }
		if ($line -match '^== (.+?) \(opens') {
			$lib = $Matches[1].ToUpper(); $kind = $null
			if (-not $union.ContainsKey($lib)) {
				$union[$lib] = @{}; $totals[$lib] = @{}
				foreach ($k in $kinds) { $union[$lib][$k] = New-Object 'System.Collections.Generic.HashSet[int]' }
			}
			continue
		}
		if (-not $lib) { continue }
		$matched = $false
		foreach ($k in $kinds) {
			$e = [regex]::Escape($k)
			if ($line -match "^  $e +\d+ / +(\d+)") { $totals[$lib][$k] = [int]$Matches[1]; $kind = $null; $matched = $true; break }
			if ($line -match "^  $e executed:(.*)$") { $kind = $k; Add-Ids $lib $k $Matches[1]; $matched = $true; break }
		}
		if ($matched) { continue }
		if ($kind -and $line -match '^     (.*)$') { Add-Ids $lib $kind $Matches[1]; continue }
		$kind = $null
	}
}

$files = Get-ChildItem $evdir -Filter *.txt | Sort-Object Name
if ($Only.Count) { $files = $files | Where-Object { $Only -contains $_.BaseName } }
$fail = 0
foreach ($f in $files) {
	$name = $f.BaseName
	$out = Join-Path $outroot $name
	New-Item -ItemType Directory -Force $out | Out-Null
	$lines = Get-Content $f.FullName
	$ini = 'rif.ini'; $seed = 1234; $expect = @()
	foreach ($l in $lines) {
		if ($l -match '^#!\s*(?:atteso|expect):\s*(.+)$') { $expect += $Matches[1].Trim() }
		elseif ($l -match '^#!\s*ini:\s*(\S+)') { $ini = $Matches[1] }
		elseif ($l -match '^#!\s*(?:seme|seed):\s*(\d+)') { $seed = [int]$Matches[1] }
	}
	$ev = Join-Path $out 'eventi.txt'
	$lines | ForEach-Object {
		if ($_ -match '^(\d+)\s+shot\s+(\S+)') { "$($Matches[1]) shot " + (Join-Path $out $Matches[2]) } else { $_ }
	} | Set-Content -Encoding ASCII $ev
	$testini = Join-Path $out 'test.ini'
	Copy-Item (Join-Path $evdir $ini) $testini -Force
	$trace = Join-Path $out 'traccia.txt'
	$cov = Join-Path $out 'copertura.txt'
	$t0 = Get-Date
	$argList = @('--game', '--800', '--seed', "$seed", '--fixed-step', '--fast', '--ini', "`"$testini`"",
		'--input', "`"$ev`"", '--trace', "`"$trace`"", '--coverage', "`"$cov`"")
	$p = Start-Process -FilePath $Exe -ArgumentList $argList -WorkingDirectory $root -NoNewWindow -Wait -PassThru `
		-RedirectStandardOutput (Join-Path $out 'stdout.txt') -RedirectStandardError (Join-Path $out 'stderr.txt')
	$secs = [int]((Get-Date) - $t0).TotalSeconds
	$missing = @()
	$text = if (Test-Path $trace) { [IO.File]::ReadAllText($trace) } else { '' }
	foreach ($e in $expect) { if (-not $text.Contains($e)) { $missing += $e } }
	$bad = ([regex]::Matches($text, '(?m)^\s*\d+\s+!!')).Count
	$ok = ($p.ExitCode -eq 0) -and ($missing.Count -eq 0)
	if (-not $ok) { $fail++ }
	$state = if ($ok) { 'OK' } else { 'FAILED' }
	Write-Output ("{0,-8} {1,-22} {2,4} s  expected {3}/{4}  errors !! {5}" -f $state, $name, $secs,
		($expect.Count - $missing.Count), $expect.Count, $bad)
	foreach ($m in $missing) { Write-Output "         missing from the trace: $m" }
	if (Test-Path $cov) { Read-Coverage $cov }
}

# Union of the coverage
$sum = Join-Path $outroot 'copertura.txt'
$report = @("Merged coverage of the tests ($($files.Count) event files), executed / present", '')
$gh = @{}; $gn = @{}
foreach ($k in $kinds) { $gh[$k] = 0; $gn[$k] = 0 }
foreach ($lib in ($union.Keys | Sort-Object)) {
	$report += "== $lib"
	foreach ($k in $kinds) {
		$h = $union[$lib][$k].Count; $n = [int]$totals[$lib][$k]
		$gh[$k] += $h; $gn[$k] += $n
		$pc = if ($n) { 100.0 * $h / $n } else { 0 }
		$report += ("  {0,-22} {1,5} / {2,5}  ({3,5:N1} %)" -f $k, $h, $n, $pc)
	}
}
$report += '', '== total'
foreach ($k in $kinds) {
	$pc = if ($gn[$k]) { 100.0 * $gh[$k] / $gn[$k] } else { 0 }
	$report += ("  {0,-22} {1,5} / {2,5}  ({3,5:N1} %)" -f $k, $gh[$k], $gn[$k], $pc)
}
$report | Set-Content -Encoding UTF8 $sum
$report | Select-Object -Last 6 | Write-Output
Write-Output "merged coverage: $sum"
if ($fail) { Write-Output "$fail tests failed"; exit 1 }
