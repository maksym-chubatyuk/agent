; agent installer (NSIS 3, Modern UI 2). Build: make installer -> build/agent-setup.exe
; Pages: directory, install, finish (Run agent / Desktop shortcut). Silent install: agent-setup.exe /S
; Uninstall: Add/Remove Programs, or "$INSTDIR\uninstall.exe". Removes everything: program files, shortcuts,
; the Add/Remove Programs entry and the user's %APPDATA%\agent (settings, sessions, memory).

Unicode true
Target x86-unicode            ; 32-bit: the installer must run on Windows XP
!include "MUI2.nsh"

!define NAME "Agent"
!define VERSION "1.0"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Agent"

Name "${NAME}"
OutFile "..\build\agent-setup.exe"
InstallDir "$PROGRAMFILES\Agent"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\agent.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run agent"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a Desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION DesktopShortcut
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Shortcuts go to the All Users Start Menu and Desktop.
Function .onInit
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetShellVarContext all
FunctionEnd

Function DesktopShortcut
  SetOutPath "$INSTDIR"
  CreateShortCut "$DESKTOP\Agent.lnk" "$INSTDIR\agent.exe"
FunctionEnd

Section "Install"
  SetOutPath "$INSTDIR"
  File "..\build\agent.exe"
  File "..\agent.ini.template"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortCut "$SMPROGRAMS\Agent.lnk" "$INSTDIR\agent.exe"

  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${NAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\agent.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 800
SectionEnd

Section "Uninstall"
  ; agent.exe can't be removed while it runs: ask the user to close it first.
retry:
  ClearErrors
  Delete "$INSTDIR\agent.exe"
  IfErrors 0 removed
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Agent is still running. Close it, then click Retry." /SD IDCANCEL IDRETRY retry
  Abort "Agent is still running."
removed:
  Delete "$SMPROGRAMS\Agent.lnk"
  Delete "$DESKTOP\Agent.lnk"
  Delete "$INSTDIR\agent.ini.template"
  Delete "$INSTDIR\agent.ini"
  Delete "$INSTDIR\agent-error.log"
  RMDir /r "$INSTDIR\data"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"

  ; Settings, sessions, memory and scratch files live in the user's %APPDATA%\agent.
  SetShellVarContext current
  RMDir /r "$APPDATA\agent"
SectionEnd
