; libuvcpp 的 Windows 安装程序（Inno Setup 6.3+）。
;
; 内容就是 `tests/tools/package_release.py` 已经产出的 stage 目录
; （`dist/libuvcpp-<ver>-msvc-<arch>/`），**不在这里重新列文件**：那份 stage 已经
; 过 package_release.py 的全部自检（导入表覆盖、调试版 CRT 泄漏、版本号一致性），
; 这里再抄一遍清单就成了第二个真相来源，迟早和它不一致。
;
; 用法（CI 里就是这么调的）：
;   ISCC.exe /DMyVersion=1.7.4 /DMyArch=x64 \
;            /DStageDir=D:\...\dist\libuvcpp-1.7.4-msvc-x64 \
;            /DOutputDir=D:\...\dist  packaging\windows\libuvcpp.iss
;
; 要签名时再加两项：
;   /DSignSetup=1
;   /Ssigntool="<signtool.exe 的绝对路径> sign /f ... $f"
; 没给就不签，安装包照出 —— 缺证书是配置问题，不是构建失败。
;
; 下面这些开关一律是「**给什么就原样吐出来**」的参数（路径、架构名），脚本里
; 不做比较：ISCC 的 `/D` 把值定义成一个符号，拿它跟字符串比在 ISPP 里的语义
; 不牢靠，而这种错只会在 CI 的某台机器上以莫名其妙的编译错误现形。

#ifndef MyVersion
  #error 必须传 /DMyVersion=<版本号>
#endif
#ifndef MyArch
  #error 必须传 /DMyArch=x64 或 /DMyArch=arm64
#endif
#ifndef ArchAllowed
  ; Inno 6.3+：x64compatible 在 ARM64 的 Windows 上也能装 x64 程序。
  #define ArchAllowed "x64compatible"
#endif
#ifndef StageDir
  #error 必须传 /DStageDir=<package_release.py 产出的 stage 目录>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef SignSetup
  #define SignSetup 0
#endif

#define AppName "libuvcpp"
#define ProjectUrl "https://github.com/Antruly/libuvcpp"

[Setup]
; 升级时这里**不能换**：AppId 变了，新版本会被当成另一个产品并存安装，
; 旧版本的 dll 留在 Program Files 里被 PATH 先找到，症状是"升级完还是旧库"。
AppId={{C6B0E1F2-3D4A-4F6B-9C2E-7A1D5E8F3B90}
AppName={#AppName}
AppVersion={#MyVersion}
AppVerName={#AppName} {#MyVersion} ({#MyArch})
AppPublisher=Antruly
AppPublisherURL={#ProjectUrl}
AppSupportURL={#ProjectUrl}/issues
AppUpdatesURL={#ProjectUrl}/releases
VersionInfoVersion={#MyVersion}
VersionInfoCompany=Antruly
VersionInfoDescription={#AppName} {#MyVersion} ({#MyArch}) installer
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; LICENSE 由 package_release.py 从仓库根拷进 stage；缺了就让这一步红，
; 不设 LicenseFile 静默跳过 —— 安装向导的第一页就是许可。
LicenseFile={#StageDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=libuvcpp-{#MyVersion}-msvc-{#MyArch}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; 图标由仓根的 `uvcpp.png`（1600²、圆角方形、带透明角）转成多尺寸 ICO，
; 7 个尺寸 16~256 都在一个文件里（Windows 按 DPI 自己挑）。`SourcePath`
; 是 .iss 所在目录 —— ISCC 的工作目录会随调用方式变，别写相对路径。
SetupIconFile={#SourcePath}\libuvcpp.ico
SetupLogging=yes
; 机器级安装（写到 Program Files 与系统 PATH），要管理员。
PrivilegesRequired=admin
ChangesEnvironment=yes
UninstallDisplayName={#AppName} {#MyVersion} ({#MyArch})
ArchitecturesAllowed={#ArchAllowed}
ArchitecturesInstallIn64BitMode={#ArchAllowed}
#if SignSetup
SignTool=signtool
; 卸载程序也要签：未签名的 uninstall.exe 从控制面板卸装时会弹 SmartScreen。
SignedUninstaller=yes
#endif

[Files]
; 整棵 stage。bin/ 里除了 uvcpp.dll 还有 MSVC 运行库 dll —— package_release.py
; 按 dll 的真实导入表补的，别在这里凭印象筛。
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "addtopath"; Description: "把 {app}\bin 加入系统 PATH（运行时能找到 uvcpp.dll）"; GroupDescription: "环境变量："; Flags: checkedonce

[Registry]
; `Path` 是 REG_EXPAND_SZ，用 expandsz 写回，别把别人的 %SystemRoot% 变成死字符串。
; Check 判重是必须的：装两次不判重就会得到两条一样的路径。
Root: HKA; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Code]
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKA, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
                             'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

// 卸载时把自己加进 PATH 的那一段摘掉。只摘**完全相同**的那一段，
// 其余原样保留；摘完为空就不写（空 Path 比多一段更难查）。
// 逐段拼回而不是 StringReplace：后者会把 "C:\x\bin2" 这种前缀相同的邻居一起改坏。
procedure RemovePath(AppDir: string);
var
  OrigPath, OutPath, Part: string;
  P, Q, L: Integer;
  Found: Boolean;
begin
  if not RegQueryStringValue(HKA, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
                             'Path', OrigPath) then
    exit;
  Found := False;
  OutPath := '';
  P := 1;
  L := Length(OrigPath);
  while P <= L do
  begin
    Q := Pos(';', Copy(OrigPath, P, L - P + 1));
    if Q = 0 then
      Q := L - P + 2;
    Part := Copy(OrigPath, P, Q - 1);
    if Uppercase(Trim(Part)) = Uppercase(AppDir) then
      Found := True
    else if Part <> '' then
    begin
      if OutPath <> '' then
        OutPath := OutPath + ';';
      OutPath := OutPath + Part;
    end;
    P := P + Q;
  end;
  if Found and (OutPath <> '') then
    RegWriteExpandStringValue(HKA, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
                              'Path', OutPath);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemovePath(ExpandConstant('{app}\bin'));
end;
