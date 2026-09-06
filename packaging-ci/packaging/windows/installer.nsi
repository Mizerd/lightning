Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef PRODUCT_VERSION
  !error "PRODUCT_VERSION is required"
!endif
!ifndef SOURCE_SHORT_SHA
  !error "SOURCE_SHORT_SHA is required"
!endif
!ifndef STAGE_DIR
  !error "STAGE_DIR is required"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif
!ifndef PUBLISHER
  !error "PUBLISHER is required"
!endif
!ifndef SIGNING_STATE
  !error "SIGNING_STATE is required"
!endif
!ifndef COPYRIGHT
  !error "COPYRIGHT is required"
!endif

Name "Lightning ${PRODUCT_VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\Lightning"
; The remembered directory lets a silent upgrade land where the user put the
; first install (including a /D= choice) -- the client updater relies on it
; and passes no /D= of its own. But the value lives under HKCU, writable by
; anything running as the user, and there is no directory page to confirm
; it; so .onInit accepts it ONLY when it already holds a Lightning install.
InstallDirRegKey HKCU "Software\Mizerd\Lightning" "InstallDir"
Icon "${STAGE_DIR}/Lightning.ico"
UninstallIcon "${STAGE_DIR}/Lightning.ico"
BrandingText "Lightning ${PRODUCT_VERSION}"

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Lightning"
VIAddVersionKey /LANG=1033 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName" "${PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "Lightning Windows installer"
VIAddVersionKey /LANG=1033 "LegalCopyright" "${COPYRIGHT}"
VIAddVersionKey /LANG=1033 "Comments" "Built from Lightning source ${SOURCE_SHORT_SHA} (${SIGNING_STATE})"

!define MUI_ABORTWARNING
!define MUI_ICON "${STAGE_DIR}/Lightning.ico"
!define MUI_UNICON "${STAGE_DIR}/Lightning.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ; $INSTDIR is, in order: /D= on the command line, the HKCU InstallDir value,
  ; or the default. Only the REGISTRY value is untrusted (a fresh /D= target
  ; has no Lightning.exe yet and must not be reset), so compare against what
  ; the registry says and validate only when that is what we got.
  ReadRegStr $0 HKCU "Software\Mizerd\Lightning" "InstallDir"
  StrCmp $0 "" onInitDone
  StrCmp $0 $INSTDIR 0 onInitDone
  IfFileExists "$INSTDIR\Lightning.exe" onInitDone
    StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Lightning"
  onInitDone:
FunctionEnd

Section "Lightning application (required)" SEC_APP
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; THE PAYLOAD MUST BE ABLE TO FAIL OUT LOUD.
  ;
  ; `File /r` opens every target CREATE_ALWAYS, which fails with a sharing
  ; violation on any file Windows has mapped -- Lightning.exe if the user
  ; double-clicks setup with Lightning open, or a DLL still held by the
  ; update helper. With no ClearErrors/IfErrors and no SetErrorLevel there
  ; was no path at all from a per-file failure to a non-zero exit, so a
  ; silent /S upgrade could install nothing and report success. The client
  ; trusts that exit code completely and tells the user the update was
  ; installed, with the old version still running.
  ClearErrors
  File /r "${STAGE_DIR}/*"
  IfErrors payloadFailed
  FileOpen $0 "$INSTDIR\.lightning-install-root" w
  ; The two marker files decide what the UNINSTALLER and the UPDATER may do:
  ; without the install-root marker the uninstaller refuses forever, and
  ; without the install-type marker an installed copy reads as portable and
  ; the updater swaps a directory the installer owns. Both writes were
  ; unchecked, so a failure left a broken installation reported as good.
  IfErrors markerFailed
  FileWrite $0 "Lightning ${PRODUCT_VERSION}$\r$\n"
  FileClose $0
  ; Tell the updater which of the three Windows packages this installation is.
  ; All three are built from one staged tree, so the compiled-in value says
  ; windows-portable and only the installer that actually placed these files can
  ; correct it. Without this, an EXE installation would be offered an MSI
  ; upgrade for a directory the Windows Installer does not own.
  ClearErrors
  FileOpen $0 "$INSTDIR\.lightning-install-type" w
  IfErrors markerFailed
  FileWrite $0 "windows-setup$\r$\n"
  FileClose $0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Mizerd\Lightning" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "DisplayName" "Lightning"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "Publisher" "${PUBLISHER}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "DisplayIcon" "$INSTDIR\Lightning.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "URLInfoAbout" "https://gitlab.smetonis.net/Mizerd/lightning"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning" "NoRepair" 1
  CreateDirectory "$SMPROGRAMS\Lightning"
  CreateShortcut "$SMPROGRAMS\Lightning\Lightning.lnk" "$INSTDIR\Lightning.exe"
  CreateShortcut "$SMPROGRAMS\Lightning\Uninstall Lightning.lnk" "$INSTDIR\Uninstall.exe"
  Return

  payloadFailed:
    ; The usual cause is Lightning still running, so say so rather than
    ; leaving a silent partial install behind.
    SetErrorLevel 2
    DetailPrint "Lightning could not write all of its files."
    DetailPrint "Close Lightning, including any copy in the tray, and run this installer again."
    MessageBox MB_ICONSTOP|MB_OK "Lightning could not write all of its files.$\r$\n$\r$\nClose Lightning, including any copy still running in the notification area, then run this installer again." /SD IDOK
    Abort "installation failed: files in use"

  markerFailed:
    SetErrorLevel 3
    DetailPrint "Lightning could not write its installation markers."
    MessageBox MB_ICONSTOP|MB_OK "Lightning could not finish writing to $INSTDIR.$\r$\n$\r$\nThe installation may be incomplete. Please run this installer again." /SD IDOK
    Abort "installation failed: could not write installation markers"
SectionEnd

Section /o "Desktop shortcut" SEC_DESKTOP
  CreateShortcut "$DESKTOP\Lightning.lnk" "$INSTDIR\Lightning.exe"
SectionEnd

Section "Uninstall"
  IfFileExists "$INSTDIR\.lightning-install-root" 0 unsafe_uninstall
  IfFileExists "$INSTDIR\Lightning.exe" 0 unsafe_uninstall
  Delete "$DESKTOP\Lightning.lnk"
  Delete "$SMPROGRAMS\Lightning\Lightning.lnk"
  Delete "$SMPROGRAMS\Lightning\Uninstall Lightning.lnk"
  RMDir "$SMPROGRAMS\Lightning"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning"
  DeleteRegKey HKCU "Software\Mizerd\Lightning"
  RMDir /r "$INSTDIR"
  ; User profiles, Matrix stores, and settings live outside $INSTDIR and are
  ; intentionally never removed by this uninstaller.
  Goto uninstall_done
unsafe_uninstall:
  MessageBox MB_ICONSTOP "Lightning install markers are missing; refusing recursive removal."
  Abort
uninstall_done:
SectionEnd
