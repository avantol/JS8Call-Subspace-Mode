#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppName=JS8Call-Subspace
AppVersion={#AppVersion}
AppPublisher=JS8Call-Subspace Project
DefaultDirName={localappdata}\Programs\JS8Call-Subspace
DefaultGroupName=JS8Call-Subspace
UninstallDisplayIcon={app}\JS8Call.exe
OutputBaseFilename=JS8Call-Subspace-Setup
OutputDir=Output
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
Compression=lzma2
SolidCompression=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "..\build\JS8Call\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\JS8Call-Subspace"; Filename: "{app}\JS8Call.exe"
Name: "{group}\Uninstall JS8Call-Subspace"; Filename: "{uninstallexe}"
Name: "{autodesktop}\JS8Call-Subspace"; Filename: "{app}\JS8Call.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\JS8Call.exe"; Description: "Launch JS8Call-Subspace"; Flags: nowait postinstall skipifsilent
