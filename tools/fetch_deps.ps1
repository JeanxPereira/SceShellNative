# Fetches the third-party runtime pieces into deps/ (ignored by git):
#   deps/mono68 : Mono 6.8.0.123 for Windows x64, extracted from the official MSI
#                 (administrative install, nothing is installed system-wide)
#   deps/yoga   : yoga.dll from NuGet Facebook.Yoga 1.5.0-pre1 (runtimes/win-x64)
param([string]$Deps = (Join-Path $PSScriptRoot '..\deps'))
$ErrorActionPreference = 'Stop'
$Deps = [IO.Path]::GetFullPath($Deps)
$dl = Join-Path $Deps 'dl'
New-Item -ItemType Directory -Force $dl | Out-Null

$msi = Join-Path $dl 'mono-6.8.0.123-x64-0.msi'
if (-not (Test-Path (Join-Path $Deps 'mono68\Mono\bin\mono-2.0-sgen.dll'))) {
    if (-not (Test-Path $msi)) {
        Invoke-WebRequest 'https://download.mono-project.com/archive/6.8.0/windows-installer/mono-6.8.0.123-x64-0.msi' -OutFile $msi
    }
    $p = Start-Process msiexec -ArgumentList '/a', $msi, '/qn', "TARGETDIR=$(Join-Path $Deps 'mono68')" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec /a failed: $($p.ExitCode)" }
}

$yoga = Join-Path $Deps 'yoga\yoga.dll'
if (-not (Test-Path $yoga)) {
    $nupkg = Join-Path $dl 'facebook.yoga.1.5.0-pre1.nupkg'
    if (-not (Test-Path $nupkg)) {
        Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Facebook.Yoga/1.5.0-pre1' -OutFile $nupkg
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($nupkg)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq 'runtimes/win-x64/native/yoga.dll' }
        New-Item -ItemType Directory -Force (Split-Path $yoga) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $yoga, $true)
    } finally { $zip.Dispose() }
}
Write-Host "deps ready in $Deps"
