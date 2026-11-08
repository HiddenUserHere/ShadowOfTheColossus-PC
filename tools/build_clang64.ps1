# Native CLANG64 build/verification without an MSYS bash process. Never starts the game.
param(
    [string]$Msys2Root = $env:MSYS2_ROOT,
    [int]$Jobs = 6,
    [switch]$NoCopy
)
$ErrorActionPreference = 'Stop'
if (-not $Msys2Root) {
    $Msys2Root = 'C:\msys64'
    if (-not (Test-Path -LiteralPath "$Msys2Root\clang64\bin\clang.exe")) {
        $Msys2Root = Join-Path $env:USERPROFILE 'Documents\Codex\toolchains\msys64'
    }
}
$nativeBin = Join-Path $Msys2Root 'clang64\bin'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $projectRoot 'out\gpu-codex'
if (-not (Test-Path -LiteralPath "$nativeBin\clang.exe")) {
    throw "CLANG64 not found under $Msys2Root"
}
$oldPath = $env:PATH
$oldLedger = $env:SOTC_BLOCK_LEDGER
$oldTrace = $env:SOTC_TRACE
$oldThreshold = $env:SOTC_TRACE_ZONE_MIN_US
try {
    $env:PATH = "$nativeBin;$oldPath"
    & "$nativeBin\cmake.exe" -S "$projectRoot\gpu" -B $buildRoot -G Ninja `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_LTO=ON -DBB_PGO=off `
        "-DCMAKE_C_COMPILER=$nativeBin\clang.exe" "-DCMAKE_CXX_COMPILER=$nativeBin\clang++.exe" `
        "-DCMAKE_RC_COMPILER=$nativeBin\llvm-windres.exe" "-DCMAKE_PREFIX_PATH=$Msys2Root\clang64" `
        "-DCMAKE_MAKE_PROGRAM=$nativeBin\ninja.exe"
    if ($LASTEXITCODE) { throw 'CMake configuration failed' }
    & "$nativeBin\cmake.exe" --build $buildRoot --parallel $Jobs --target `
        bb-probe frame-budget-test block-ledger-test driver-cache-identity-test preparation-status-test trace-ledger-smoke-test manifest-capture-test
    if ($LASTEXITCODE) { throw 'Native build failed; executable not copied' }
    foreach ($test in @('frame-budget-test', 'driver-cache-identity-test', 'preparation-status-test', 'manifest-capture-test')) {
        & "$buildRoot\$test.exe"
        if ($LASTEXITCODE) { throw "$test failed; executable not copied" }
    }
    $env:SOTC_TRACE_ZONE_MIN_US = '0'
    foreach ($mode in @(@('0', '0'), @('1', '0'), @('0', '1'), @('1', '1'))) {
        $env:SOTC_BLOCK_LEDGER = $mode[0]
        $env:SOTC_TRACE = $mode[1]
        & "$buildRoot\block-ledger-test.exe"
        if ($LASTEXITCODE) { throw "block-ledger-test failed in mode $mode; executable not copied" }
    }
    $env:SOTC_TRACE = '1'
    & "$buildRoot\trace-ledger-smoke-test.exe" "$buildRoot\trace-ledger-smoke-output"
    if ($LASTEXITCODE) { throw 'Trace/ledger integration test failed; executable not copied' }
    if (-not $NoCopy) {
        $installed = Join-Path $projectRoot 'out\bb-probe.exe'
        $backup = Join-Path $projectRoot 'out\bb-probe.before-codex-20261107.exe'
        if ((Test-Path -LiteralPath $installed) -and -not (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $installed -Destination $backup
        }
        Copy-Item -LiteralPath "$buildRoot\bb-probe.exe" -Destination $installed -Force
    }
    Write-Output "Build and six native test targets passed. Runtime DLL prefix: $nativeBin"
} finally {
    $env:PATH = $oldPath
    $env:SOTC_BLOCK_LEDGER = $oldLedger
    $env:SOTC_TRACE = $oldTrace
    $env:SOTC_TRACE_ZONE_MIN_US = $oldThreshold
}
