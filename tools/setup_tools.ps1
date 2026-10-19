# Downloads and unpacks into tools\ the portable tools of the project, checking SHA-256.
# Nothing is installed on the system: everything stays in tools\ (original archives in
# tools\_downloads\).
#
#   powershell -ExecutionPolicy Bypass -File tools\setup_tools.ps1            (everything, asks first)
#   powershell -ExecutionPolicy Bypass -File tools\setup_tools.ps1 -Only build (only what the build needs)
#   ... -Yes   do not ask for confirmation
#
# Groups: "build" = w64devkit (GCC), SDL2, SoundFont (needed by build.bat and by the game);
#         "decompile" = Temurin JDK + Ghidra (only for decompiling the original executables).
# Versions, sources and hashes verified on 2026-10-16: to update a tool change its line below.
param(
	[ValidateSet('all', 'build', 'decompile')] [string] $Only = 'all',
	[switch] $Yes
)
$ErrorActionPreference = 'Stop'
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$dl = Join-Path $tools '_downloads'

$items = @(
	@{ Group = 'build'; Name = 'w64devkit 2.10.1 (GCC 16.2, Unlicense)'; Dest = 'w64devkit'; Kind = 'sfx7z'
	   Url = 'https://github.com/skeeto/w64devkit/releases/download/v2.10.1/w64devkit-x64-2.10.1.7z.exe'
	   File = 'w64devkit-x64-2.10.1.7z.exe'; Size = 67263552
	   Sha = 'add1edf984ce4d473cfeb7c52e0ab38db73a855bba6c7f564d5e16648eb2896a'; Top = 'w64devkit' },
	@{ Group = 'build'; Name = 'SDL2 2.32.10 for MinGW (zlib)'; Dest = 'SDL2'; Kind = 'zip'
	   Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip'
	   File = 'SDL2-devel-2.32.10-mingw.zip'; Size = 14260563
	   Sha = 'f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac'; Top = 'SDL2-2.32.10' },
	@{ Group = 'build'; Name = 'SoundFont GeneralUser GS 2.0.3 (GeneralUser v2.0 license)'; Dest = 'soundfont\GeneralUser-GS.sf2'; Kind = 'file'
	   Url = 'https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60/GeneralUser-GS.sf2'
	   File = 'GeneralUser-GS.sf2'; Size = 32319396
	   Sha = '9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe' },
	@{ Group = 'decompile'; Name = 'Eclipse Temurin JDK 21.0.12.1+1 (GPLv2 + Classpath Exception)'; Dest = 'jdk'; Kind = 'zip'
	   Url = 'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip'
	   File = 'OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip'; Size = 205073461
	   Sha = 'f9d6e191ab098c0d416e7d588a24420a8621cd2f4720dab2459b8b7b2d2d8b4e'; Top = 'jdk-21.0.12.1+1' },
	@{ Group = 'decompile'; Name = 'Ghidra 12.1.4 (Apache 2.0)'; Dest = 'ghidra'; Kind = 'zip'
	   Url = 'https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip'
	   File = 'ghidra_12.1.4_PUBLIC_20260921.zip'; Size = 569649598
	   Sha = 'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db'; Top = 'ghidra_12.1.4_PUBLIC' }
)

$todo = @($items | Where-Object { ($Only -eq 'all' -or $_.Group -eq $Only) -and -not (Test-Path (Join-Path $tools $_.Dest)) })
if ($todo.Count -eq 0) {
	Write-Host 'Tools already present in tools\: nothing to download.'
	exit 0
}
$need = @($todo | Where-Object { -not (Test-Path (Join-Path $dl $_.File)) })
Write-Host 'To be installed in tools\ (archives in tools\_downloads\):'
foreach ($t in $todo) {
	$mb = [math]::Round($t.Size / 1MB, 1)
	$src = if (Test-Path (Join-Path $dl $t.File)) { 'already downloaded' } else { $t.Url }
	Write-Host ("  - {0}, {1} MB`n      {2}" -f $t.Name, $mb, $src)
}
if ($need.Count -gt 0 -and -not $Yes) {
	$ans = Read-Host 'Download the files listed above? (y/N)'
	if ($ans -notmatch '^[sSyY]') { Write-Host 'Cancelled.'; exit 1 }
}
New-Item -ItemType Directory -Force $dl | Out-Null
foreach ($t in $todo) {
	$arc = Join-Path $dl $t.File
	if (-not (Test-Path $arc)) {
		Write-Host "Downloading $($t.File)..."
		# curl.exe (Windows 10 1803+) is much faster than Invoke-WebRequest on large files
		& curl.exe -L --fail -o $arc $t.Url
		if ($LASTEXITCODE -ne 0) { throw "download failed: $($t.Url)" }
	}
	$h = (Get-FileHash -Algorithm SHA256 $arc).Hash.ToLower()
	if ($h -ne $t.Sha) { throw "SHA-256 mismatch for $($t.File): $h (expected $($t.Sha)). File left in _downloads for inspection." }
	Write-Host "  SHA-256 verified: $($t.File)"
	$dest = Join-Path $tools $t.Dest
	switch ($t.Kind) {
		'file' {
			New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
			Copy-Item $arc $dest
		}
		'zip' {
			$tmp = Join-Path $dl ('_x_' + $t.Dest)
			if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
			New-Item -ItemType Directory -Force $tmp | Out-Null
			# bsdtar of Windows 10+ (also reads zip files, much faster); otherwise Expand-Archive
			$bsdtar = Join-Path $env:SystemRoot 'System32\tar.exe'
			$ok = $false
			if (Test-Path $bsdtar) { & $bsdtar -xf $arc -C $tmp; $ok = $LASTEXITCODE -eq 0 }
			if (-not $ok) { Expand-Archive -Path $arc -DestinationPath $tmp -Force }
			Move-Item (Join-Path $tmp $t.Top) $dest
			Remove-Item -Recurse -Force $tmp
		}
		'sfx7z' {
			# self-extracting 7-Zip archive: -o destination folder, -y no questions
			$p = Start-Process -FilePath $arc -ArgumentList "-o`"$tools`"", '-y' -Wait -PassThru -NoNewWindow
			if ($p.ExitCode -ne 0) { throw "extraction failed: $($t.File)" }
		}
	}
	Write-Host "  installed in tools\$($t.Dest)"
}
Write-Host 'Done.'
