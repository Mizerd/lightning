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

Section "Lightning application (required)" SEC_APP
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}/*"
  FileOpen $0 "$INSTDIR\.lightning-install-root" w
  FileWrite $0 "Lightning ${PRODUCT_VERSION}$\r$\n"
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
