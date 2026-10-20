# Summary of what the translated game logic (port\game\*.c of a development tree) leaves to the
# runtime: instructions handled by rt_op, and builtins used that have no case in rt.c (none
# expected now), with the original handler and its size in the decompiled code (ExportDecomp.java).
# Usage: missing_report.ps1 [-Game port\game] [-Exe src\TOPSY\TOPSY.c] [-Runtime port\src\rt.c]
param(
	[string] $Game = 'port\game',
	[string] $Exe = 'src\TOPSY\TOPSY.c',
	[string] $Runtime = 'port\src\rt.c'
)
$all = (Get-ChildItem "$Game\*.c" | % { Get-Content $_.FullName -Raw }) -join "`n"
$c = Get-Content $Exe -Raw
$bodies = @{}
foreach ($x in [regex]::Matches($c, '// ---- (\S+) (\S+) ----(.*?)(?=// ---- |\z)', 'Singleline')) { $bodies[$x.Groups[2].Value] = $x.Groups[3].Value }

function Handlers($fn) {
	$h = @{}
	$m = $bodies[$fn]
	foreach ($b in [regex]::Matches($m, '((?:\s*case (0x[0-9a-f]+|\d+):)+)(.*?)(?=\n\s*(?:case (?:0x[0-9a-f]+|\d+):|default:)|\z)', 'Singleline')) {
		$call = ([regex]::Match($b.Groups[3].Value, '(FUN_\w+|S_\d+|A_\d+|[A-Z][A-Z0-9_]{3,})\(')).Groups[1].Value
		$len = if ($call -and $bodies[$call]) { $bodies[$call].Length } else { $b.Groups[3].Value.Length }
		foreach ($o in [regex]::Matches($b.Groups[1].Value, 'case (0x[0-9a-f]+|\d+):')) {
			$v = $o.Groups[1].Value
			$n = if ($v -like '0x*') { [Convert]::ToInt32($v.Substring(2), 16) } else { [int]$v }
			$h['{0:X2}' -f $n] = @($call, $len)
		}
	}
	$h
}

$hop = Handlers 'Script_ExecOp'
$hbi = Handlers 'Engine_Command'
$rtc = Get-Content $Runtime -Raw
$done = [regex]::Matches($rtc, 'case 0x([0-9A-F]{2}):') | % { $_.Groups[1].Value }

$ops = [regex]::Matches($all, '/\* op ([0-9A-F]{2})') | % { $_.Groups[1].Value } | Group-Object | Sort-Object Name
$kb = 0
"== Instructions still generic (rt_op): $($ops.Count) opcodes, $(($ops | Measure-Object Count -Sum).Sum) occurrences"
foreach ($o in $ops) { $e = $hop[$o.Name]; $kb += $e[1]; "  {0}  x{1,-4} handler {2,-14} {3,6:N1} KB" -f $o.Name, $o.Count, $e[0], ($e[1] / 1KB) }
"  total handlers: {0:N0} KB of decompiled C" -f ($kb / 1KB)

$bi = [regex]::Matches($all, 'rt_builtin\(rt, 0x([0-9A-F]{2})') | % { $_.Groups[1].Value } | Group-Object | Sort-Object Name
$kb = 0
$todo = $bi | ? { $done -notcontains $_.Name }
"== Builtins used: $($bi.Count) (already ported $($bi.Count - $todo.Count)), to port $($todo.Count)"
foreach ($b in $todo) { $e = $hbi[$b.Name]; $kb += $e[1]; "  {0}  x{1,-4} {2,-14} {3,6:N1} KB" -f $b.Name, $b.Count, $e[0], ($e[1] / 1KB) }
"  total: {0:N0} KB" -f ($kb / 1KB)
$var = [regex]::Matches($all, 'rt_builtin\(rt, [^0][^,]*,').Count
"  calls with non-constant id: $var"
