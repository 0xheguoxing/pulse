# 便携版（免安装版）构建指南

本文说明如何在源码上构建 Pulse 便携版。产物有两种形态：

| 形态 | 产物 | 用途 |
| --- | --- | --- |
| 便携 zip（发布形态） | `dist\Pulse-<版本>-portable-win-x64.zip` | 对外分发，解压即用 |
| 免安装目录（本机常用） | `dist\` 下平铺的 exe/dll/licenses | 本机直接运行、快速迭代目验 |

两种形态内容一致：**7 个 exe + 2 个 DLL + licenses + README.txt**。

## 产物清单

```text
pulse.exe              主程序
Pulse.Index.exe        索引/查询宿主
Pulse.Document.exe     文档解析宿主
Pulse.Preview.exe      预览宿主
pulse_shell.exe        Shell 右键菜单宿主
pulse_integration.exe  Shell 集成注册工具
pulse_elevated.exe     提权文件操作宿主
lumatext.dll           文本渲染库（third_party/lumatext 预编译）
pdfium.dll             PDF 渲染库（third_party/pdfium 预编译）
licenses\              PDFium / LumaText / ib-pinyin / md4c 许可证
README.txt             免安装版说明
```

## 前置条件

- Windows x64 + Visual Studio C++ 工具链（MSVC）、CMake 3.25+、Ninja。
- PowerShell 7.2+（仅打包 zip 需要；目录形态不需要）。
- 仓库内已自带 `third_party/lumatext`、`third_party/pdfium` 预编译产物，无需额外下载。

## 方式一：官方脚本（对外发布）

```powershell
.\build_release.bat                    # Release 全量构建（注意：不限并行度）
pwsh ./scripts/package_portable.ps1    # 打出 dist\Pulse-<版本>-portable-win-x64.zip
```

`package_portable.ps1` 内部会先跑 `check_release_payload.ps1`（确认没有嵌入
selftest 套件），再打 zip 并逐条目校验。

## 方式二：手动构建（限制资源，本机常用）

`build_release.bat` 不限并行度，机器吃不满时可手动执行并限 4 并行：

```cmd
:: 1) 初始化 MSVC 环境
call scripts\vcvars.bat

:: 2) 配置（仅首次或参数变更时需要）
cmake -S . -B build -G Ninja ^
  -DCMAKE_MAKE_PROGRAM=C:/developer/ninja/ninja.exe ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -DPULSE_WITH_SELFTEST=OFF -DPULSE_RELEASE_SYMBOLS=OFF ^
  -DPULSE_UPDATE_MANIFEST_URL=https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest.json ^
  -DPULSE_UPDATE_PUBLIC_KEY_HEX=<cmake\update-public-key.txt 的内容> ^
  -DLUMATEXT_SOURCE_DIR= -DPULSE_WITH_LUMATEXT=ON

:: 3) 只编译便携版需要的 7 个目标（4 并行）
cmake --build build --parallel 4 --target pulse pulse_index pulse_document pulse_preview pulse_shell pulse_integration pulse_elevated
```

要点：

- `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`：静态 CRT，产物不依赖运行库安装。
- `-DPULSE_WITH_SELFTEST=OFF`：发布产物禁止嵌入自测套件（校验脚本会查）。
- 改动少时只需 `cmake --build build --parallel 4 --target pulse`。

## 免安装目录形态：直接铺到 dist

构建完成后把 payload 平铺到 `dist\`（不打 zip）：

```powershell
$build = 'build'; $dist = 'dist'
$files = @('pulse.exe','Pulse.Index.exe','Pulse.Document.exe','Pulse.Preview.exe',
           'pulse_shell.exe','pulse_integration.exe','pulse_elevated.exe',
           'lumatext.dll','pdfium.dll')
foreach ($f in $files) { Copy-Item "build\$f" $dist -Force }
# licenses 与 README 首次铺设一次即可，之后一般不变
```

`licenses\`（PDFium、LumaText、ib-pinyin、md4c）与 `README.txt` 由
`scripts/package_portable.ps1` 的铺设逻辑生成过一次后长期有效。

## 校验

```powershell
pwsh ./scripts/check_release_payload.ps1 -BuildDir build
# 输出 "Release payload check passed" 即可分发
```

## 本机踩坑备注

- **ninja 静默失败**：PATH 里 depot_tools 的 `ninja.bat` 依赖 `python3`；本机已在
  `D:\app\developer\python.3.12.10\` 放了 `python3.exe`。若再失败，先 `where python3`
  确认命中的不是 `WindowsApps` 下的 Store 重定向空壳。
- **CMake 抓到 GCC**：配置时显式加 `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl`。
- **Compress-Archive 报签名错误**：pwsh 的 `Microsoft.PowerShell.Archive` 模块文件
  损坏所致。修复：删除 `C:\app\PowerShell7\Modules\Microsoft.PowerShell.Archive`，再
  `Save-PSResource Microsoft.PowerShell.Archive -Version 1.2.6 -Path C:\app\PowerShell7\Modules -Repository PSGallery -TrustRepository`。
  注意 System32 里 5.1 版模块生成的 zip 条目用 `\` 分隔，官方打包脚本校验不认。
- **并行度**：构建命令一律 `--parallel 4`，避免占满 8 核机器。
