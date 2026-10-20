# Overview of a game folder: executables, engine, archives.
# Usage: survey.ps1 -GameDir <folder> [-Lb7Tool <path of lb7tool.exe>]
param(
	[Parameter(Mandatory)] [string] $GameDir,
	[string] $Lb7Tool = (Join-Path $PSScriptRoot '..\..\build\lb7tool.exe')
)

function Read-NE([byte[]] $b) {
	$ne = [BitConverter]::ToInt32($b, 0x3C)
	$name = { param($o) [Text.Encoding]::ASCII.GetString($b, $o + 1, $b[$o]) }
	$res = $ne + [BitConverter]::ToUInt16($b, $ne + 0x26)
	$modOff = $ne + [BitConverter]::ToUInt16($b, $ne + 0x28)
	$impOff = $ne + [BitConverter]::ToUInt16($b, $ne + 0x2A)
	$n = [BitConverter]::ToUInt16($b, $ne + 0x1E)
	$imports = for ($i = 0; $i -lt $n; $i++) { & $name ($impOff + [BitConverter]::ToUInt16($b, $modOff + 2 * $i)) }
	[pscustomobject]@{
		Module  = & $name $res
		WinVer  = '{0}.{1}' -f $b[$ne + 0x3F], $b[$ne + 0x3E]
		Imports = $imports -join ','
	}
}

# game files: excludes the folders created by the pipeline
$skip = '\\(tools|build|src|extracted|port)(\\|$)'
$files = Get-ChildItem -LiteralPath $GameDir -File -Recurse |
	? { $_.FullName.Substring((Resolve-Path $GameDir).Path.Length) -notmatch $skip }

"== Executables"
foreach ($f in $files | ? { $_.Extension -in '.exe', '.dll', '.drv' }) {
	$b = [IO.File]::ReadAllBytes($f.FullName)
	if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { continue }
	$ne = [BitConverter]::ToInt32($b, 0x3C)
	$sig = if ($ne -gt 0 -and $ne + 2 -lt $b.Length) { [Text.Encoding]::ASCII.GetString($b, $ne, 2) } else { 'MZ' }
	$ver = ([regex]::Matches([Text.Encoding]::ASCII.GetString($b), '[A-Za-z ]*(Player|Library) \d+\.\d+[^\x00]{0,40}7th Level[^\x00]*') |
		% Value | Select-Object -Unique) -join ' / '
	if ($sig -eq 'NE') {
		$i = Read-NE $b
		"{0,-14} NE  module={1,-9} win={2} {3}`n               imports: {4}" -f $f.Name, $i.Module, $i.WinVer, $ver, $i.Imports
	}
	else { "{0,-14} {1}  {2}" -f $f.Name, $sig, $ver }
}

"`n== 7LB archives (magic '7L', version)"
foreach ($f in $files) {
	$fs =[IO.File]::OpenRead($f.FullName)
	$h = New-Object byte[] 0x58
	$n = $fs.Read($h, 0, $h.Length)
	$fs.Close()
	if ($n -lt 0x58 -or $h[0] -ne 0x37 -or $h[1] -ne 0x4C) { continue }
	"{0,-14} {1,10:N0} bytes  header=0x{2:X} version={3}" -f $f.Name, $f.Length,
		[BitConverter]::ToUInt16($h, 2), [BitConverter]::ToUInt16($h, 0x56)
	if (Test-Path $Lb7Tool) {
		& $Lb7Tool info $f.FullName | Select-Object -Skip 1 | % { "    $_" }
		& $Lb7Tool disasm $f.FullName | Select-String 'aligned' | % { "    $($_.Line)" }
	}
}
