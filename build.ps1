param(
    [string]$Zig = "",
    [ValidatePattern('^[^\\/:*?"<>|]+\.exe$')][string]$OutputName = "DesktopBox.exe",
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
if (-not $Zig) {
    $Zig = Join-Path $taskRoot '.tools\zig-windows-x86_64-0.13.0\zig.exe'
}
if (-not (Test-Path -LiteralPath $Zig)) {
    throw 'Compiler not found. Run: python tools/bootstrap_compiler.py, or pass -Zig with the path to zig.exe. Visual Studio users can use CMakeLists.txt instead.'
}
$taskBuild = Join-Path $taskRoot 'build'
$taskExecutable = Join-Path $taskBuild $OutputName
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskRoot '.tools\zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskRoot '.zig-cache'
Push-Location $taskRoot
try {
    & $Zig rc /c65001 /fo build/app.res resources/app.rc
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    function Build-DesktopBox([string]$Destination, [string[]]$ExtraFlags = @()) {
    & $Zig c++ src/main.cpp src/model.cpp src/selection.cpp src/storage.cpp src/desktop.cpp src/desktop_mouse.cpp src/native_entry.cpp src/backdrop.cpp src/shell_menu.cpp src/shell_registration.cpp src/fluent_settings.cpp build/app.res -o $Destination @ExtraFlags `
        -std=c++17 -Os -s -municode '-Wl,--subsystem,windows' -DUNICODE -D_UNICODE -DNOMINMAX `
        -D_WIN32_WINNT=0x0a00 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-command-line-argument `
        -luser32 -lgdi32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid `
        -lcomdlg32 -lcomctl32 -ldwmapi -ladvapi32 -luxtheme -lgdiplus
    if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
    }
    Build-DesktopBox $taskExecutable
    Write-Output $taskExecutable
    if ($Test) {
        $taskTestPath = Join-Path $taskBuild 'self-test'
        $taskExe = Join-Path $taskBuild 'DesktopBox-tests.exe'
        Build-DesktopBox $taskExe @('-DDESKTOPBOX_TESTS')
        $taskProcess = Start-Process -FilePath $taskExe -ArgumentList @('--self-test', ('"' + $taskTestPath + '"')) -WindowStyle Hidden -PassThru -Wait
        Get-Content -LiteralPath (Join-Path $taskTestPath 'results.txt')
        if ($taskProcess.ExitCode -ne 0) { throw 'Self-test failed.' }
    }
} finally {
    Pop-Location
}

