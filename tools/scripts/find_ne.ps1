# Lists the Win16 "New Executable" (NE) modules of a game folder, DLLs before EXEs.
# Excludes drivers and control panels (.DRV, .CPL) and subfolders.
# Usage: find_ne.ps1 -GameDir <folder> [-Verbose]
param([Parameter(Mandatory)] [string] $GameDir)

$mods = foreach ($f in Get-ChildItem -LiteralPath $GameDir -File | ? { $_.Extension -in '.exe', '.dll' }) {
	$b = [IO.File]::ReadAllBytes($f.FullName)
	if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { continue }
	$ne = [BitConverter]::ToInt32($b, 0x3C)
	if ($ne -le 0 -or $ne + 0x40 -gt $b.Length -or $b[$ne] -ne 0x4E -or $b[$ne + 1] -ne 0x45) {
		Write-Verbose "skipping $($f.Name): not NE"
		continue
	}
	[pscustomobject]@{ Path = $f.FullName; IsDll = $f.Extension -eq '.dll' }
}
$mods | Sort-Object @{ e = { -not $_.IsDll } }, Path | % { $_.Path }
