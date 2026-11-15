# 桌面收纳盒 · DesktopBox

一个 Windows 11 x64 桌面整理工具。把文件放进收纳盒，鼠标停在图标上就能展开。

[下载 0.5.0 安装包](https://github.com/ShuLing18/Desktop-tools/releases/download/v0.5.0/DesktopBox-0.5.0-Setup.exe) · [查看所有版本](https://github.com/ShuLing18/Desktop-tools/releases)

## 主要功能

- 可以建多个收纳盒，支持桌面拖动、改名和右键操作。
- 拖入时默认移动真实文件、文件夹或已有快捷方式，同名项目保留两份。
- 支持搜索、排序、多选、框选和整组拖动。
- 可以更换图标、背景和展开方向，也能统一设置所有盒子。
- 安装窗口采用 Win11 风格，程序位置和文件保存位置可以分开选。
- 删除盒子时归还内容；卸载程序会保留收纳文件和个人图标。

## 怎么用

1. 下载安装包，选择程序安装位置和收纳文件保存位置。
2. 启动后，把桌面上的文件拖进收纳盒；悬停展开，双击打开。
3. 右键收纳盒调整设置，右键托盘图标新建盒子或管理全局设置。

更新会沿用原保存位置，已有文件不会自动迁移。删除盒子时，没有原位置记录的项目需要另选归还目录。

## 源码编译

需要 Windows、PowerShell 和 Python 3.9+。首次编译先准备工具链：

```powershell
python tools/bootstrap_compiler.py
.\build.ps1 -OutputName DesktopBox-0.5.0.exe -Test
.\package.ps1 -Test
```

程序输出到 `build/`，安装包输出到 `release/DesktopBox-0.5.0-Setup.exe`。性能与验证记录见 [OPTIMIZATION.md](OPTIMIZATION.md)。
