; explorer-lite installer (NSIS; adapted from wa-lite's). Built by package.sh: makensis -DVERSION=0.1.0 installer/explorer-lite.nsi
; Installs for the current user, without administrator, in %LOCALAPPDATA%\Programs\explorer-lite.
; The settings (%LOCALAPPDATA%\explorer-lite) are kept on install and update; on uninstall it asks whether to delete them
; (default no; with /S, never).
; With /S it shows nothing: the silent update. It asks the running explorer-lite of this folder to save its windows and
; quit (--quit), puts the new files in, and reopens it with --updated (the same windows where they were) if it was open.
Unicode true
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
; VIProductVersion only accepts digits: with a prerelease (0.2.8-branch.1) package.sh passes VERSION_NUM=0.2.8
!ifndef VERSION_NUM
  !define VERSION_NUM "${VERSION}"
!endif
!define APP "explorer-lite"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\explorer-lite"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APP}"
OutFile "dist\explorer-lite-Setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\explorer-lite"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails nevershow
ShowUninstDetails nevershow
BrandingText " "

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"

!define MUI_ICON "res\explorer-lite.ico"
!define MUI_UNICON "res\explorer-lite.ico"
!define MUI_COMPONENTSPAGE_NODESC
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\explorer-lite.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open explorer-lite"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
; English always (Windows' language does not choose it); the app has its own language setting
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION_NUM}.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "explorer-lite"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "explorer-lite Setup"
VIAddVersionKey /LANG=${LANG_ENGLISH} "CompanyName" "justdiego"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "justdiego"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "${VERSION}"

Var Reopen
Var Updater

; the explorer-lite.exe running from $INSTDIR (and only that one) is asked to quit; 10 = it was running
!macro CloseRunning
  StrCpy $0 0
  ${If} ${FileExists} "$INSTDIR\explorer-lite.exe"
    nsExec::Exec /TIMEOUT=20000 '"$INSTDIR\explorer-lite.exe" --quit'
    Pop $0
    ${If} $0 == 11
      ; it did not quit in 15 s: ended by force, only the one of this folder
      nsExec::Exec /TIMEOUT=20000 `powershell -NoProfile -NonInteractive -WindowStyle Hidden -Command "Get-Process explorer-lite -ErrorAction SilentlyContinue | Where-Object { $$_.Path -eq '$INSTDIR\explorer-lite.exe' } | Stop-Process -Force; Start-Sleep -Milliseconds 500"`
      Pop $1
      StrCpy $0 10
    ${EndIf}
  ${EndIf}
!macroend

Section "-explorer-lite" SecApp
  SectionIn RO
  StrCpy $Reopen 0
  StrCpy $Updater 0
  ; launched by the app's own updater (it downloads to %LOCALAPPDATA%\explorer-lite\tmp, src/update.cpp): the app has
  ; already saved its windows and quit, so it is not found running below, but it has to be reopened
  ${If} ${Silent}
  ${AndIf} $EXEDIR == "$LOCALAPPDATA\explorer-lite\tmp"
    StrCpy $Reopen 1
    StrCpy $Updater 1
  ${EndIf}
  !insertmacro CloseRunning
  ${If} $0 == 10
    StrCpy $Reopen 1
  ${EndIf}
  ; the old exe can stay locked for a moment after it exits
  Sleep 300

  SetOutPath "$INSTDIR"
  File "dist\explorer-lite\explorer-lite.exe"
  File "/oname=explorer-lite.ico" "res\explorer-lite.ico"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "explorer-lite"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "justdiego"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\explorer-lite.ico"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $3 $4 $5
  IntFmt $3 "0x%08X" $3
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" "$3"

  CreateShortcut "$SMPROGRAMS\explorer-lite.lnk" "$INSTDIR\explorer-lite.exe" "" "$INSTDIR\explorer-lite.ico" 0
SectionEnd

Section "Start with Windows" SecStartup
  ; --tray: only the resident (the tray icon), no window
  WriteRegStr HKCU "${RUN_KEY}" "explorer-lite" '"$INSTDIR\explorer-lite.exe" --tray'
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\explorer-lite.lnk" "$INSTDIR\explorer-lite.exe" "" "$INSTDIR\explorer-lite.ico" 0
SectionEnd

Section "-finish"
  ; unticked on an update: the startup entry and desktop shortcut go, if they are this folder's
  ${IfNot} ${SectionIsSelected} ${SecStartup}
    ReadRegStr $0 HKCU "${RUN_KEY}" "explorer-lite"
    ${If} $0 == '"$INSTDIR\explorer-lite.exe" --tray'
      DeleteRegValue HKCU "${RUN_KEY}" "explorer-lite"
    ${EndIf}
  ${EndIf}
  ${IfNot} ${SectionIsSelected} ${SecDesktop}
    Delete "$DESKTOP\explorer-lite.lnk"
  ${EndIf}
  ; silent update: reopened as it was (--updated restores the windows; with none it stays in the tray)
  ${If} ${Silent}
  ${AndIf} $Reopen == 1
    Exec '"$INSTDIR\explorer-lite.exe" --updated'
  ${EndIf}
SectionEnd

Function .onInit
  ; on an update, the checkboxes as the previous installation left them
  ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
  ${If} $0 != ""
    ReadRegStr $1 HKCU "${RUN_KEY}" "explorer-lite"
    ${If} $1 == ""
      !insertmacro UnselectSection ${SecStartup}
    ${EndIf}
    ${If} ${FileExists} "$DESKTOP\explorer-lite.lnk"
      !insertmacro SelectSection ${SecDesktop}
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "Uninstall"
  !insertmacro CloseRunning
  ; what explorer-lite may have registered for this user (Win+E, opening folders): back to Windows' own
  nsExec::Exec /TIMEOUT=20000 '"$INSTDIR\explorer-lite.exe" --unregister'
  Pop $0
  Sleep 300
  Delete "$INSTDIR\explorer-lite.exe"
  Delete "$INSTDIR\explorer-lite.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\explorer-lite.lnk"
  Delete "$DESKTOP\explorer-lite.lnk"
  ReadRegStr $0 HKCU "${RUN_KEY}" "explorer-lite"
  ${If} $0 == '"$INSTDIR\explorer-lite.exe" --tray'
    DeleteRegValue HKCU "${RUN_KEY}" "explorer-lite"
  ${EndIf}
  ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
  ${If} $0 == $INSTDIR
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
  ${EndIf}
  ; the settings only if told yes (default no; with /S, never)
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete explorer-lite's settings?" /SD IDNO IDNO done
    RMDir /r "$LOCALAPPDATA\explorer-lite"
  done:
SectionEnd
