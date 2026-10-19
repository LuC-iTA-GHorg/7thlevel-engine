# Automatic profile of the opcode handlers of the 7th Level interpreter.
# For each "case 0xNN:" of Script_ExecOp it lists the handler function and the APIs/exports
# reachable within -Depth call levels. Output: Markdown table.
# Usage: powershell -File profile_opcodes.ps1 -Source ..\..\src\TOPSY\TOPSY.c -Function Script_ExecOp
param(
	[Parameter(Mandatory)] [string] $Source,
	[string] $Function = 'Script_ExecOp',
	[int] $Depth = 2
)

$c = Get-Content $Source -Raw
$bodies = @{}
foreach ($m in [regex]::Matches($c, '// ---- (\S+) (\S+) ----(.*?)(?=// ---- |\z)', 'Singleline')) {
	$bodies[$m.Groups[2].Value] = $m.Groups[3].Value
}
$exec = $bodies[$Function]
if (-not $exec) { throw "function $Function not found" }

$ignore = '^(CONCAT\d+|SUB\d+|ZEXT\d+|SEXT\d+|CARRY\d|SBORROW\d|POPCOUNT|if|while|for|switch|return|sizeof)$'

function Get-Calls([string] $body) {
	[regex]::Matches($body, '\b([A-Za-z_][A-Za-z0-9_]*)\s*\(') | % { $_.Groups[1].Value } |
		? { $_ -notmatch $ignore } | Sort-Object -Unique
}

function Get-Reach([string] $name, [int] $d, [hashtable] $seen) {
	if ($d -lt 0 -or $seen.ContainsKey($name)) { return }
	$seen[$name] = 1
	if ($bodies.ContainsKey($name)) {
		foreach ($k in Get-Calls $bodies[$name]) { Get-Reach $k ($d - 1) $seen }
	}
}

# case blocks: from "case X:" to the next "case" with code
$blocks = [regex]::Matches($exec, '((?:\s*case (0x[0-9a-f]+|\d+):)+)(.*?)(?=\n\s*(?:case (?:0x[0-9a-f]+|\d+):|default:)|\z)', 'Singleline')
"| Op | Handler | Reaches (APIs, DLL exports, named functions) |"
"|---|---|---|"
foreach ($b in $blocks) {
	$ops = [regex]::Matches($b.Groups[1].Value, 'case (0x[0-9a-f]+|\d+):') | % {
		$v = $_.Groups[1].Value; if ($v -like '0x*') { [Convert]::ToInt32($v.Substring(2), 16) } else { [int]$v } }
	$direct = Get-Calls $b.Groups[3].Value
	$seen = @{}
	foreach ($k in $direct) { Get-Reach $k $Depth $seen }
	$named = $seen.Keys | ? { $_ -notmatch '^FUN_' } | Sort-Object
	$handler = ($direct | Select-Object -First 3) -join ', '
	foreach ($o in $ops) { "| {0:X2} | {1} | {2} |" -f $o, $handler, ($named -join ' ') }
}
