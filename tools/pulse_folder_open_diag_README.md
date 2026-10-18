# Pulse 桌面文件夹双击诊断（只读）

此工具用于排查：设置中已选择用 Pulse 打开文件夹，但双击桌面项目仍然打开文件资源管理器。

## 收集的信息

- Windows 版本，以及当前运行的 `pulse.exe` 路径（不收集进程命令行）。
- `Folder`、`Directory`、`Drive` 的当前用户与有效 Shell `open` 注册表值，包括已知的隐藏标记。
- Pulse 接管注册表备份快照是否存在（不读取快照内容）。
- 当前用户桌面上的文件夹、`.lnk` 快捷方式和项目总数（不收集名称）。
- 如果提供 `-Path`，只报告目标属于真实文件系统目录、`.lnk` 快捷方式（以及目标类型），还是其他项目；不报告目标名称或完整目标路径。

工具**不会**修改注册表、启动文件夹、自动发送数据或读取文件内容。报告保存在本机 `%TEMP%` 目录。分享前请先检查报告；其中注册表命令值可能包含应用安装路径。

## 运行方法

1. 解压 ZIP。
2. 打开 PowerShell（不需要管理员权限）。
3. 把 `-Path` 指向那个**双击后仍会打开文件资源管理器的桌面项目**：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\pulse_folder_open_diag.ps1 -Path "$env:USERPROFILE\Desktop\要检查的项目名称"
```

如果桌面项目是 `.lnk` 快捷方式，请传入该 `.lnk` 的路径。如果无法确定项目路径（例如它是特殊 Shell 项目），可省略 `-Path`；脚本仍会报告关联状态和桌面项目计数。

4. 找到最新生成的 `Pulse-Folder-Open-Diagnostic-*.txt`，检查后再发给 Pulse 开发者。报告不会自动上传。

此脚本仅用于诊断，不会尝试修复文件关联。
