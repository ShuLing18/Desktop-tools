param([string]$Zig = "")
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
if (-not $Zig) { $Zig = Join-Path $taskRoot '.tools\zig-windows-x86_64-0.13.0\zig.exe' }
$taskPayload = Join-Path $taskRoot 'build\DesktopBox-0.4.6.exe'
$taskRelease = Join-Path $taskRoot 'release'
$taskSetup = Join-Path $taskRelease 'DesktopBox-0.4.6-Setup.exe'
$taskBuilt = Join-Path $taskRoot 'build\DesktopBox-0.4.6-Setup.exe'
if (-not (Test-Path -LiteralPath $Zig)) { throw 'Compiler not found. Build the app first with build.ps1.' }
if (-not (Test-Path -LiteralPath $taskPayload)) { throw 'Build the verified DesktopBox-0.4.6.exe first.' }
$taskVersion = (Get-Item -LiteralPath $taskPayload).VersionInfo.FileVersion
if ($taskVersion -notmatch '^0\.4\.6(?:\.0)?$') { throw "Unexpected app version: $taskVersion" }
New-Item -ItemType Directory -Path $taskRelease -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskRoot '.tools\zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskRoot '.zig-cache'
Push-Location $taskRoot
try {
    & $Zig rc /c65001 /fo build/setup.res installer/setup.rc
    if ($LASTEXITCODE -ne 0) { throw 'Installer resource compilation failed.' }
    & $Zig c++ installer/setup.cpp build/setup.res -o $taskBuilt -std=c++17 -Os -s -municode `
        '-Wl,--subsystem,windows' -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0a00 `
        -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-command-line-argument `
        -luser32 -lgdi32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid -ladvapi32
    if ($LASTEXITCODE -ne 0) { throw 'Installer C++ compilation failed.' }
} finally { Pop-Location }
Copy-Item -LiteralPath $taskBuilt -Destination $taskSetup -Force
$taskHash = (Get-FileHash -LiteralPath $taskSetup -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $taskRelease 'SHA256SUMS.txt') -Value "$taskHash  DesktopBox-0.4.6-Setup.exe" -Encoding ascii
@'
桌面收纳盒 0.4.6 · Windows 11 x64

下载 DesktopBox-0.4.6-Setup.exe 后运行。安装窗口可选择安装位置；
安装器会自动创建 DesktopBox 文件夹，并在里面放置程序、data 和 ico。
安装和运行无需管理员权限，但所选位置必须允许当前用户写入。

卸载：Windows 设置 → 已安装的应用 → 桌面收纳盒。
卸载会保留 data 和 ico 中的个人文件；如需删除，请先自行备份。

此发布包只包含程序，不包含开发者的收纳内容或自定义图标。
校验值见 SHA256SUMS.txt。
'@ | Set-Content -LiteralPath (Join-Path $taskRelease '使用说明.txt') -Encoding utf8
Write-Output $taskSetup
