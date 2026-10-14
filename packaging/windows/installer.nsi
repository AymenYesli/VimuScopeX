; VimuScope X installer (NSIS). Built by build-windows.sh (step 6), which
; passes the version and the folders:
;   makensis -DVERSION=1.0.0 -DSRCDIR=<dist\VimuScopeX> -DOUTFILE=<setup.exe> -DICON=<.ico> installer.nsi
;
; Installs the self-contained folder to Program Files, with Start menu and
; desktop shortcuts, an uninstaller, and an entry in Apps & features.

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"   ; ${GetSize}, used below

!ifndef VERSION
  !define VERSION "1.0.0"
!endif

!define APPNAME   "VimuScope X"
!define EXENAME   "VimuScopeX.exe"
!define REGKEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\VimuScopeX"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\VimuScopeX" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ICON   "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APPNAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  ; A previous version's files go first, so nothing stale is left behind.
  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\VimuScopeX" "InstallDir" "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"

  ; Apps & features
  WriteRegStr HKLM "${REGKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr HKLM "${REGKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr HKLM "${REGKEY}" "Publisher"       "VimuScope X"
  WriteRegStr HKLM "${REGKEY}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APPNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey HKLM "Software\VimuScopeX"
  ; Settings, recordings and probes are the user's: left in place.
SectionEnd

