<#
.SYNOPSIS
  Builds Sony's published PS5 JavaScriptCore (JSC-400, the source of libSceJsc in FW 4.00-4.51)
  as a Win64 JavaScriptCore.dll + WTF.dll + jsc.exe.

.DESCRIPTION
  Source : https://www.playstation.com/content/dam/global_pdc/en-us/external-resources/oss/PS5/webkit-javascriptcore/JSC-400.zip
           (WebKit trunk r266022, 2020-08-22, plus Sony patches; 1,288,815,910 bytes)
  Port   : WinCairo (shared JavaScriptCore/WTF DLLs, Win RunLoop, socket remote inspector),
           configured like OptionsPlayStation.cmake for the engine: JIT/DFG/FTL off (LLInt / CLoop-free
           offlineasm interpreter only), WebAssembly off, remote inspector on.
  Deps   : WebKitRequirements v2020.08.20 (ICU 67.1 = the ICU of the PS5 libicu.sprx), portable
           Ruby 2.7 (offlineasm), GnuWin32 gperf 3.0.1, Strawberry Perl, Python 3, CMake, Ninja, MSVC.
  Edits  : see deps\jsc\PATCHES.md. The script re-applies them idempotently (Apply-Patches).

  Everything lives under deps\ (git-ignored). Nothing is installed system-wide.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\jsc\build_jsc.ps1            # all stages
  powershell -ExecutionPolicy Bypass -File tools\jsc\build_jsc.ps1 -Stage build
#>
param(
    [ValidateSet('all', 'fetch', 'extract', 'patch', 'configure', 'build', 'verify')]
    [string]$Stage = 'all',
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Zip = '',
    [string]$VcVars = 'C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat',
    [string]$BuildType = 'Release',
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

$Deps   = Join-Path $Root 'deps'
$JscDir = Join-Path $Deps 'jsc'
$DlDir  = Join-Path $JscDir 'dl'
$SrcDir = Join-Path $JscDir 'src\JSC-400'
$BldDir = Join-Path $JscDir 'build'
$ReqDir = Join-Path $JscDir 'WebKitRequirements'
$Tools  = Join-Path $Deps 'tools'
$Ruby   = Join-Path $Tools 'rubyinstaller-2.7.8-1-x64\bin\ruby.exe'
$Gperf  = Join-Path $Tools 'gperf\bin\gperf.exe'
$SevenZ = 'C:\Program Files\7-Zip\7z.exe'
if (-not $Zip) { $Zip = Join-Path $Root 'work\oss\jsc400_dl\JSC-400.zip' }

# Keep every temp/cache off C:/D: (D: is nearly full).
$TmpDir = Join-Path $JscDir 'tmp'
New-Item -ItemType Directory -Force $DlDir, $Tools, $TmpDir | Out-Null
$env:TEMP = $TmpDir; $env:TMP = $TmpDir

function Get-File($Url, $Out) {
    if (-not (Test-Path $Out)) {
        Write-Host "download $Url"
        & curl.exe -sSfL -o $Out $Url
        if ($LASTEXITCODE) { throw "download failed: $Url" }
    }
}

function Invoke-Fetch {
    $req = Join-Path $DlDir 'WebKitRequirementsWin64.zip'
    Get-File 'https://github.com/WebKitForWindows/WebKitRequirements/releases/download/v2020.08.20/WebKitRequirementsWin64.zip' $req
    if (-not (Test-Path (Join-Path $ReqDir 'lib64\icuuc.lib'))) {
        New-Item -ItemType Directory -Force $ReqDir | Out-Null
        & $SevenZ x -y -bd "-o$ReqDir" $req | Out-Null
    }
    $ruby7z = Join-Path $Tools 'ruby27.7z'
    Get-File 'https://github.com/oneclick/rubyinstaller2/releases/download/RubyInstaller-2.7.8-1/rubyinstaller-2.7.8-1-x64.7z' $ruby7z
    if (-not (Test-Path $Ruby)) { & $SevenZ x -y -bd "-o$Tools" $ruby7z | Out-Null }
    $gperfZip = Join-Path $Tools 'gperf.zip'
    Get-File 'https://downloads.sourceforge.net/project/gnuwin32/gperf/3.0.1/gperf-3.0.1-bin.zip' $gperfZip
    if (-not (Test-Path $Gperf)) { & $SevenZ x -y -bd ("-o" + (Join-Path $Tools 'gperf')) $gperfZip | Out-Null }
}

function Invoke-Extract {
    if (Test-Path (Join-Path $SrcDir 'Source\JavaScriptCore\CMakeLists.txt')) { Write-Host 'source already extracted'; return }
    if (-not (Test-Path $Zip)) { Get-File 'https://secure.cdn.us.playstation.com/oss/PS5/webkit-javascriptcore/JSC-400.zip' $Zip }
    & $SevenZ x -y -bd ("-o" + (Join-Path $JscDir 'src')) $Zip | Out-Null
    if ($LASTEXITCODE) { throw '7z extraction failed' }
}

# Each patch: file, unique anchor text, replacement. Applied once (skipped when the marker is present).
function Edit-Once($File, $Marker, $Old, $New) {
    $p = Join-Path $SrcDir $File
    $s = [IO.File]::ReadAllText($p)
    if ($s.Contains($Marker)) { return }
    $nl = if ($s.Contains("`r`n")) { "`r`n" } else { "`n" }
    $o = $Old -replace "`r?`n", $nl; $n = $New -replace "`r?`n", $nl
    $i = $s.IndexOf($o)
    if ($i -lt 0) { throw "patch anchor not found in $File" }
    if (-not (Test-Path "$p.orig")) { Copy-Item $p "$p.orig" }
    $s = $s.Substring(0, $i) + $n + $s.Substring($i + $o.Length)
    [IO.File]::WriteAllText($p, $s)
    Write-Host "patched $File"
}

# Replaces every line exactly equal to $Old (ignoring a trailing CR) with $New.
function Edit-AllLines($File, $Old, $New) {
    $p = Join-Path $SrcDir $File
    $s = [IO.File]::ReadAllText($p)
    if ($s.Contains($New)) { return }
    if (-not (Test-Path "$p.orig")) { Copy-Item $p "$p.orig" }
    $s = [regex]::Replace($s, '(?m)^' + [regex]::Escape($Old) + '(?=\r?$)', $New.Replace('$', '$$'))
    [IO.File]::WriteAllText($p, $s)
    Write-Host "patched $File"
}

function Invoke-Patch {
    # P1: compile Sony's PlayStation C API additions on Windows (only PlatformPlayStation.cmake lists them).
    Edit-Once 'Source\JavaScriptCore\PlatformWin.cmake' 'API/playstation/JSContextRefPrivatePlayStation.cpp' @'
list(APPEND JavaScriptCore_SOURCES
    API/JSStringRefBSTR.cpp
)
'@ @'
list(APPEND JavaScriptCore_SOURCES
    API/JSStringRefBSTR.cpp
)

# SceShellNative: Sony PlayStation C API additions (same as PlatformPlayStation.cmake)
list(APPEND JavaScriptCore_SOURCES
    API/playstation/JSContextRefPrivatePlayStation.cpp
)
list(APPEND JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES
    "${JAVASCRIPTCORE_DIR}/API/playstation"
)
list(APPEND JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS
    API/playstation/JSContextRefPrivatePlayStation.h
)
'@
    # P2: JSRemoteInspectorServerStart (API/JSRemoteInspectorServer.cpp) with the Win socket inspector.
    Edit-Once 'Source\JavaScriptCore\PlatformWin.cmake' 'API/JSRemoteInspectorServer.cpp' @'
        API/JSRemoteInspector.cpp

'@ @'
        API/JSRemoteInspector.cpp
        API/JSRemoteInspectorServer.cpp

'@
    # P3: Sony's per-target FunctionDispatcher (what JSGlobalContextSet/UnsetDebuggerRunLoop set) is
    # compiled only for PLATFORM(PLAYSTATION); enable the same code on Windows.
    foreach ($f in 'Source\JavaScriptCore\inspector\remote\RemoteControllableTarget.h',
                   'Source\JavaScriptCore\inspector\remote\RemoteConnectionToTarget.cpp') {
        Edit-AllLines $f '#if PLATFORM(PLAYSTATION)' '#if PLATFORM(PLAYSTATION) || OS(WINDOWS) // SceShellNative: Sony target dispatcher on Win'
    }
    # P4: WTF StackBounds on Windows assumed the reserve + guard page + commit stack layout; threads
    # created with a fully committed stack (Mono's managed threads) got origin == bound, so every JS
    # entry threw "Maximum call stack size exceeded". Use the thread's actual stack limits.
    Edit-Once 'Source\WTF\wtf\StackBounds.cpp' 'SceShellNative: GetCurrentThreadStackLimits' @'
StackBounds StackBounds::currentThreadStackBoundsInternal()
{
    MEMORY_BASIC_INFORMATION stackOrigin { };
'@ @'
StackBounds StackBounds::currentThreadStackBoundsInternal()
{
    // SceShellNative: GetCurrentThreadStackLimits. The layout walk below assumes a reserved stack
    // (uncommitted memory, guard page, committed memory); a thread created with a fully committed
    // stack has none of that and the walk returned origin == bound. Take the limits from the
    // thread itself and keep the guaranteed overflow-handling area plus one guard page out of reach.
    {
        ULONG_PTR lowLimit = 0, highLimit = 0;
        // Declared from kernel32 at run time: the WinCairo build targets a pre-Windows 8 API level.
        using GetLimits = VOID (WINAPI*)(PULONG_PTR, PULONG_PTR);
        static auto getLimits = reinterpret_cast<GetLimits>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits"));
        getLimits(&lowLimit, &highLimit);
        ULONG guarantee = 0;
        SetThreadStackGuarantee(&guarantee);
        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);
        void* origin = reinterpret_cast<void*>(highLimit);
        void* bound = reinterpret_cast<char*>(lowLimit) + guarantee + systemInfo.dwPageSize;
        return StackBounds { origin, bound };
    }
    MEMORY_BASIC_INFORMATION stackOrigin { };
'@
    #@@PATCHES@@ (further patches are appended here)
}

function Import-VcVars {
    if ($env:VCToolsInstallDir) { return }
    $lines = & cmd.exe /c "`"$VcVars`" >nul && set"
    foreach ($l in $lines) { if ($l -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
}

function Invoke-Configure {
    Import-VcVars
    $env:WEBKIT_LIBRARIES = $ReqDir
    $env:PATH = "$(Split-Path $Ruby);$(Split-Path $Gperf);$ReqDir\bin64;$ReqDir\tools\bin;$env:PATH"
    $args = @(
        '-G', 'Ninja', '-S', $SrcDir, '-B', $BldDir,
        "-DCMAKE_BUILD_TYPE=$BuildType",
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
        '-DPORT=WinCairo',
        "-DWEBKIT_LIBRARIES_DIR=$($ReqDir -replace '\\','/')",
        '-DENABLE_JIT=OFF', '-DENABLE_DFG_JIT=OFF', '-DENABLE_FTL_JIT=OFF',
        '-DENABLE_WEBASSEMBLY=OFF', '-DENABLE_SAMPLING_PROFILER=OFF',
        '-DENABLE_REMOTE_INSPECTOR=ON',
        '-DENABLE_TOOLS=OFF', '-DENABLE_WEBINSPECTORUI=OFF', '-DENABLE_API_TESTS=OFF',
        '-DDEVELOPER_MODE=OFF',
        "-DRUBY_EXECUTABLE=$($Ruby -replace '\\','/')",
        "-DGPERF_EXECUTABLE=$($Gperf -replace '\\','/')"
    )
    & cmake @args
    if ($LASTEXITCODE) { throw 'cmake configure failed' }
}

function Invoke-Build {
    Import-VcVars
    $env:PATH = "$(Split-Path $Ruby);$(Split-Path $Gperf);$ReqDir\bin64;$env:PATH"
    $j = @(); if ($Jobs -gt 0) { $j = @('-j', $Jobs) }
    & cmake --build $BldDir --target jsc @j
    if ($LASTEXITCODE) { throw 'build failed' }
    # Runtime DLLs from WebKitRequirements so bin64 is self-contained (ICU 67.1 + CFLite + its pthreads).
    foreach ($d in 'CFLite.dll', 'pthreadsVC2.dll', 'icudt67.dll', 'icuin67.dll', 'icuuc67.dll') {
        Copy-Item (Join-Path $ReqDir "bin64\$d") (Join-Path $BldDir 'bin64') -Force
    }
}

function Invoke-Verify {
    Import-VcVars
    $bin = Join-Path $BldDir 'bin64'
    $env:PATH = "$bin;$ReqDir\bin64;$env:PATH"
    $t = Join-Path $TmpDir 'smoke.js'
    @'
print(1 + 1);
print(JSON.stringify({ a: [1, 2, { b: "c" }] }));
let p = Promise.resolve(42); p.then(v => print("promise " + v));
print(new Intl.NumberFormat("de-DE").format(1234567.891));
print(new Intl.DateTimeFormat("en-US", { timeZone: "UTC" }).format(new Date(0)));
print("abc".localeCompare("abd"), "ICU", typeof Intl.DisplayNames);
'@ | Set-Content -Encoding ascii $t
    & (Join-Path $bin 'jsc.exe') $t
    if ($LASTEXITCODE) { throw 'jsc smoke test failed' }
    $want = 'JSGlobalContextCreateInGroup', 'JSEvaluateScript', 'JSValueMakeFromJSONString', 'JSValueCreateJSONString',
            'JSObjectMakeFunctionWithCallback', 'JSGlobalContextSetUnhandledRejectionCallback', 'JSContextGroupAddHeapFinalizer',
            'JSGlobalContextSetDebuggerRunLoopWithCurrentRunLoop', 'JSGlobalContextUnsetDebuggerRunLoop',
            'JSRunLoopIterateCurrentRunLoop', 'JSGlobalContextSetEvalEnabled', 'JSGetMemoryUsageStatistics',
            'JSSetOptions', 'JSRunLoop', 'JSRemoteInspectorServerStart'
    $ex = & dumpbin.exe /exports (Join-Path $bin 'JavaScriptCore.dll') | ForEach-Object { ($_ -split '\s+')[-1] }
    $missing = $want | Where-Object { $ex -notcontains $_ }
    if ($missing) { throw "missing exports: $($missing -join ', ')" }
    Write-Host "all $($want.Count) checked exports present"
}

switch ($Stage) {
    'fetch'     { Invoke-Fetch }
    'extract'   { Invoke-Extract }
    'patch'     { Invoke-Patch }
    'configure' { Invoke-Configure }
    'build'     { Invoke-Build }
    'verify'    { Invoke-Verify }
    'all'       { Invoke-Fetch; Invoke-Extract; Invoke-Patch; Invoke-Configure; Invoke-Build; Invoke-Verify }
}
