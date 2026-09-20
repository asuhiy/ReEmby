; =============================================================================
; ReEmby Win64 安装包脚本（Inno Setup 6）
;
; 由 CI 调用，路径与版本通过命令行 define 注入：
;   iscc /DSrcDir=<绝对路径> /DOutDir=<绝对路径> /DAppVersion=x.y.z packaging/windows/ReEmby.iss
; 本地手动构建示例：
;   iscc /DSrcDir=build\bin\Release /DOutDir=dist /DAppVersion=0.0.7 packaging/windows/ReEmby.iss
;
; SrcDir 必须是 POST_SERVICE 之后的完整绿色包目录
; （exe + Qt DLL + 插件 + libmpv-2.dll，由 CMake windeployqt 步骤产出）。
; =============================================================================

#ifndef SrcDir
#define SrcDir "build\bin\Release"
#endif

#ifndef OutDir
#define OutDir "dist"
#endif

#ifndef AppVersion
#define AppVersion "0.0.0"
#endif

[Setup]
; 固定 AppId：保证升级/卸载识别同一安装实例
AppId={{7C1F4A62-9B3E-4D58-A1F7-2C8D5E6B9014}
AppName=ReEmby
AppVersion={#AppVersion}
AppPublisher=ReEmby
; 与绿色包文件名保持一致的产物名（ReEmby-<ver>-win-x64-Setup.exe）
OutputBaseFilename=ReEmby-{#AppVersion}-win-x64-Setup
OutputDir={#OutDir}
DefaultDirName={autopf}\ReEmby
DefaultGroupName=ReEmby
DisableProgramGroupPage=yes
; 允许用户在安装时选择「为当前用户安装」（免管理员权限）
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\ReEmby.exe

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 整目录递归打包；ignoreversion 保证 Qt DLL 等始终覆盖
Source: "{#SrcDir}\*"; DestDir: "{app}"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\ReEmby"; Filename: "{app}\ReEmby.exe"
Name: "{group}\卸载 ReEmby"; Filename: "{uninstallexe}"
Name: "{autodesktop}\ReEmby"; Filename: "{app}\ReEmby.exe"; \
    Tasks: desktopicon

[Run]
Filename: "{app}\ReEmby.exe"; Description: "{cm:LaunchProgram,ReEmby}"; \
    Flags: nowait postinstall skipifsilent
