param([string]$Zig = "", [switch]$Test)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
if (-not $Zig) { $Zig = Join-Path $taskRoot '.tools\zig-windows-x86_64-0.13.0\zig.exe' }
$taskPayload = Join-Path $taskRoot 'build\DesktopBox-0.5.0.exe'
$taskRelease = Join-Path $taskRoot 'release'
$taskSetup = Join-Path $taskRelease 'DesktopBox-0.5.0-Setup.exe'
$taskBuilt = Join-Path $taskRoot 'build\DesktopBox-0.5.0-Setup.exe'
if (-not (Test-Path -LiteralPath $Zig)) { throw 'Compiler not found. Build the app first with build.ps1.' }
if (-not (Test-Path -LiteralPath $taskPayload)) { throw 'Build the verified DesktopBox-0.5.0.exe first.' }
$taskVersion = (Get-Item -LiteralPath $taskPayload).VersionInfo.FileVersion
if ($taskVersion -notmatch '^0\.5\.0(?:\.0)?$') { throw "Unexpected app version: $taskVersion" }
New-Item -ItemType Directory -Path $taskRelease -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskRoot '.tools\zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskRoot '.zig-cache'
Push-Location $taskRoot
try {
    & $Zig rc /c65001 /fo build/setup.res installer/setup.rc
    if ($LASTEXITCODE -ne 0) { throw 'Installer resource compilation failed.' }
    & $Zig c++ installer/setup.cpp build/setup.res -o $taskBuilt -std=c++17 -Os -s -municode `
        -target x86_64-windows-gnu -mcpu=baseline `
        '-Wl,--subsystem,windows' -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0a00 `
        -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-command-line-argument `
        -luser32 -lgdi32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32 -ldwmapi -lgdiplus
    if ($LASTEXITCODE -ne 0) { throw 'Installer C++ compilation failed.' }
    if ($Test) {
        $taskChecks = Join-Path $taskRoot 'build\DesktopBox-installer-tests.exe'
        & $Zig c++ tests/installer_checks.cpp build/setup.res -o $taskChecks -std=c++17 -Os -s -municode `
            -target x86_64-windows-gnu -mcpu=baseline `
            -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0a00 `
            -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-command-line-argument `
            -luser32 -lgdi32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid -ladvapi32 -lcomctl32 -ldwmapi -lgdiplus
        if ($LASTEXITCODE -ne 0) { throw 'Installer test compilation failed.' }
        & $taskChecks (Join-Path $taskRoot 'build\installer-checks') |
            Tee-Object -FilePath (Join-Path $taskRoot 'build\installer-checks.txt')
        if ($LASTEXITCODE -ne 0) { throw 'Installer checks failed.' }
    }
} finally { Pop-Location }
Copy-Item -LiteralPath $taskBuilt -Destination $taskSetup -Force
Copy-Item -LiteralPath $taskPayload -Destination (Join-Path $taskRelease 'DesktopBox-0.5.0.exe') -Force
$taskHashes = foreach ($taskFile in @('DesktopBox-0.5.0-Setup.exe', 'DesktopBox-0.5.0.exe')) {
    $taskHash = (Get-FileHash -LiteralPath (Join-Path $taskRelease $taskFile) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$taskHash  $taskFile"
}
Set-Content -LiteralPath (Join-Path $taskRelease 'SHA256SUMS.txt') -Value $taskHashes -Encoding ascii
@'
桌面收纳盒 0.5.0 · Windows 11 x64

首次使用建议下载安装包；DesktopBox-0.5.0.exe 是独立程序文件。
独立运行默认将数据保存在当前用户的 LocalAppData/DesktopBox。

下载 DesktopBox-0.5.0-Setup.exe 后运行。安装窗口可分别选择：
1. 程序安装位置：自动创建 DesktopBox 文件夹，存放程序和 ico 图标库。
2. 收纳文件保存位置：拖入的真实文件、文件夹和来源记录保存在这里，可选择其他磁盘。
   未单独选择时，默认保存在程序目录下的 data 文件夹。
保存位置会自动记住，双击程序、登录启动和桌面入口均使用该位置。
安装和运行无需管理员权限，但所选位置必须允许当前用户写入。

更新安装会默认沿用原保存位置。已有数据的安装不允许直接改到其他目录，
以免原有收纳盒和文件不再显示；安装程序不会自动迁移已有文件。

卸载：Windows 设置 → 已安装的应用 → 桌面收纳盒。
卸载会保留所选保存位置中的收纳文件及 ico 中的个人图标。

此发布包只包含程序，不包含开发者的收纳内容或自定义图标。
校验值见 SHA256SUMS.txt。
'@ | Set-Content -LiteralPath (Join-Path $taskRelease '使用说明.txt') -Encoding utf8
Write-Output $taskSetup
