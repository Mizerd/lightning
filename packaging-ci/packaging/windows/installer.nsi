Unicode true
; THE INSTALLER NEVER ASKS FOR ADMINISTRATOR RIGHTS UP FRONT.
;
; `user` is deliberate and must stay. `highest` (what the stock MultiUser.nsh
; wants for an All Users choice) raises a UAC prompt on EVERY double-click by
; an administrator -- which is most home users -- for a per-user install that
; needs no rights at all. Instead the installer runs unprivileged, installs
; "Just for me" by default exactly as every release up to 0.9.9 did, and
; only when "All users" is chosen (page, /ALLUSERS, or an existing per-machine
; installation being upgraded) does it relaunch ITSELF elevated through
; ShellExecuteEx "runas" and wait for that copy's exit code.
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"

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

; The internal registry path keeps its historical Software\Mizerd\Lightning
; location on purpose (build-windows.sh explains why). It holds ONLY the
; installer's own InstallDir; the application's settings live under
; Software\MatrixClient and are never touched by this script.
!define REG_APP "Software\Mizerd\Lightning"
!define REG_UNINSTALL "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lightning"
; Written into $INSTDIR: "user" or "machine". Read by the uninstaller (which
; context to clean, whether to elevate) and by the in-app updater
; (src/update/InstallType.cpp), which must hand an upgrade to the SAME scope
; -- a per-user upgrade of a per-machine install would leave two copies.
!define SCOPE_MARKER ".lightning-install-scope"
; ShellExecuteEx failed because the user declined the UAC prompt. Returned as
; the installer's exit code so a silent caller can tell "not approved" from
; "failed", and the in-app updater explains it (UpdateManager.cpp).
!define EXIT_ELEVATION_DECLINED 1223
; Elevation was "granted" but the relaunched copy is still not an
; administrator (UAC disabled for a standard account), or the command line
; asked for both scopes at once.
!define EXIT_ELEVATION_UNAVAILABLE 5
!define EXIT_BAD_COMMAND_LINE 87

Name "Lightning ${PRODUCT_VERSION}"
OutFile "${OUTPUT_FILE}"
; The per-user default. .onInit replaces it per scope; there is deliberately
; no InstallDirRegKey, because the remembered directory lives under HKCU for a
; per-user installation and under HKLM for a per-machine one, and a static
; attribute can only name one of them.
InstallDir "$LOCALAPPDATA\Programs\Lightning"
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

Var InstallScope          ; "user" | "machine"
Var ScopeFromCommandLine  ; "1" when /ALLUSERS or /CURRENTUSER was given
Var DirFromCommandLine    ; "1" when /D= was given
Var ElevatedRelaunch      ; "1" when this copy is the relaunched, elevated one
Var ExistingUserDir       ; a real per-user installation, or ""
Var ExistingMachineDir    ; a real per-machine installation, or ""
Var ModeRadioUser
Var ModeRadioMachine

!define MUI_ABORTWARNING
!define MUI_ICON "${STAGE_DIR}/Lightning.ico"
!define MUI_UNICON "${STAGE_DIR}/Lightning.ico"
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipPageInElevatedCopy
!insertmacro MUI_PAGE_WELCOME
Page custom ModePageCreate ModePageLeave
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; Elevation, shared by the installer and the uninstaller.
; ---------------------------------------------------------------------------

; Leaves 1 on the stack when this process holds an administrator token. Under
; UAC an administrator's UNELEVATED token answers 0 here, which is exactly the
; question: can this process write Program Files and HKLM right now.
!macro LIGHTNING_IS_ELEVATED
  System::Call 'shell32::IsUserAnAdmin() i .s'
!macroend

; Relaunches $EXEPATH through ShellExecuteEx "runas" with PARAMS, waits for it
; and leaves its exit code in $R0 -- or ${EXIT_ELEVATION_DECLINED} when the UAC
; prompt was declined, or ${EXIT_ELEVATION_UNAVAILABLE} when the elevated copy
; could not be started at all. (Not 1: an NSIS installer exits 1 when the
; person cancels it, and that must stay distinguishable.)
;
; SHELLEXECUTEINFOW on x64 is 112 bytes with natural alignment. The System
; plugin packs lowercase struct members WITHOUT alignment, so the two 4-byte
; holes (after nShow and after dwHotKey) are spelled out as `i` fields; without
; them every pointer after nShow is read 4 bytes off and hProcess comes back as
; garbage. Offsets: cbSize 0, fMask 4, hwnd 8, lpVerb 16, lpFile 24,
; lpParameters 32, lpDirectory 40, nShow 48, (pad) 52, hInstApp 56, lpIDList 64,
; lpClass 72, hkeyClass 80, dwHotKey 88, (pad) 92, hIcon 96, hProcess 104.
; 0x140 = SEE_MASK_NOCLOSEPROCESS (0x40) | SEE_MASK_NOASYNC (0x100).
!macro LIGHTNING_RUN_ELEVATED PARAMS
  Push $1
  Push $2
  Push $3
  Push $4
  Push $5
  Push $6
  Push $7
  ; The strings go in through REGISTERS (`w r5`), never pasted into the call
  ; definition: System::Call parses that text, so a path such as
  ; "Lightning-setup (1).exe" -- what a browser names a second download --
  ; would break it at the parenthesis.
  StrCpy $4 "runas"
  StrCpy $5 "$EXEPATH"
  StrCpy $6 "${PARAMS}"
  StrCpy $7 "$EXEDIR"
  System::Call '*(i 112, i 0x140, p $HWNDPARENT, w r4, w r5, w r6, w r7, i 1, i 0, p 0, p 0, p 0, p 0, i 0, i 0, p 0, p 0) p .r1'
  System::Call 'shell32::ShellExecuteExW(p r1) i .r2 ?e'
  Pop $3
  ${If} $2 = 0
    ${If} $3 = ${EXIT_ELEVATION_DECLINED}
      StrCpy $R0 ${EXIT_ELEVATION_DECLINED}
    ${Else}
      StrCpy $R0 ${EXIT_ELEVATION_UNAVAILABLE}
    ${EndIf}
  ${Else}
    System::Call '*$1(i, i, p, p, p, p, p, i, i, p, p, p, p, i, i, p, p .r2)'
    System::Call 'kernel32::WaitForSingleObject(p r2, i -1) i'
    StrCpy $R0 ${EXIT_ELEVATION_UNAVAILABLE}
    System::Call 'kernel32::GetExitCodeProcess(p r2, *i .R0) i'
    System::Call 'kernel32::CloseHandle(p r2) i'
  ${EndIf}
  System::Free $1
  Pop $7
  Pop $6
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
!macroend

; ---------------------------------------------------------------------------
; Scope
; ---------------------------------------------------------------------------

; A remembered directory counts only when it still holds a Lightning install.
; HKCU is writable by anything running as the user and there is no directory
; page to confirm the value, so an arbitrary path there must never become
; $INSTDIR (File /r and, later, the uninstaller's RMDir /r act on it).
Function DetectExistingInstalls
  ReadRegStr $ExistingUserDir HKCU "${REG_APP}" "InstallDir"
  ${If} $ExistingUserDir != ""
  ${AndIfNot} ${FileExists} "$ExistingUserDir\Lightning.exe"
    StrCpy $ExistingUserDir ""
  ${EndIf}
  ReadRegStr $ExistingMachineDir HKLM "${REG_APP}" "InstallDir"
  ${If} $ExistingMachineDir != ""
  ${AndIfNot} ${FileExists} "$ExistingMachineDir\Lightning.exe"
    StrCpy $ExistingMachineDir ""
  ${EndIf}
FunctionEnd

; Sets the shell-folder context and, unless /D= chose it, the directory for
; $InstallScope. The per-user default must be read while the context is
; "current": under "all", $LOCALAPPDATA names ProgramData.
Function ApplyScope
  ${If} $InstallScope == "machine"
    SetShellVarContext all
    ${If} $DirFromCommandLine != "1"
      ${If} $ExistingMachineDir != ""
        StrCpy $INSTDIR $ExistingMachineDir
      ${Else}
        StrCpy $INSTDIR "$PROGRAMFILES64\Lightning"
      ${EndIf}
    ${EndIf}
  ${Else}
    SetShellVarContext current
    ${If} $DirFromCommandLine != "1"
      ${If} $ExistingUserDir != ""
        StrCpy $INSTDIR $ExistingUserDir
      ${Else}
        StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Lightning"
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

; The parameters for the elevated copy: the scope switch and the loop guard,
; then this copy's own parameters, then /D= again when one was given. NSIS
; CUTS "/D=..." out of $CMDLINE before any script code runs (it must be the
; last token), so ${GetParameters} never contains it and it has to be put back
; from $INSTDIR -- LAST, unquoted, which is the only form NSIS accepts.
Function ElevatedParameters
  ${GetParameters} $R1
  StrCpy $R1 "/ALLUSERS /ELEVATED $R1"
  ${If} $DirFromCommandLine == "1"
    StrCpy $R1 "$R1 /D=$INSTDIR"
  ${EndIf}
FunctionEnd

; Relaunches this installer elevated for a per-machine install and quits with
; the elevated copy's exit code.
Function ElevateForMachineScopeAndQuit
  Call ElevatedParameters
  !insertmacro LIGHTNING_RUN_ELEVATED "$R1"
  SetErrorLevel $R0
  Quit
FunctionEnd

Function .onInit
  StrCpy $InstallScope "user"
  StrCpy $ScopeFromCommandLine ""
  StrCpy $DirFromCommandLine ""
  StrCpy $ElevatedRelaunch ""

  ${GetParameters} $R1
  ; GetOptions is case-insensitive, so /AllUsers and /CurrentUser (the
  ; spelling MultiUser.nsh documents) are accepted too.
  ClearErrors
  ${GetOptions} $R1 "/ALLUSERS" $R2
  ${IfNot} ${Errors}
    StrCpy $InstallScope "machine"
    StrCpy $ScopeFromCommandLine "1"
  ${EndIf}
  ClearErrors
  ${GetOptions} $R1 "/CURRENTUSER" $R2
  ${IfNot} ${Errors}
    ${If} $ScopeFromCommandLine == "1"
      MessageBox MB_ICONSTOP|MB_OK "Use either /ALLUSERS or /CURRENTUSER, not both." /SD IDOK
      SetErrorLevel ${EXIT_BAD_COMMAND_LINE}
      Quit
    ${EndIf}
    StrCpy $InstallScope "user"
    StrCpy $ScopeFromCommandLine "1"
  ${EndIf}
  ClearErrors
  ${GetOptions} $R1 "/ELEVATED" $R2
  ${IfNot} ${Errors}
    StrCpy $ElevatedRelaunch "1"
  ${EndIf}
  ; /D= never reaches ${GetParameters} (NSIS removes it from $CMDLINE), so
  ; the only trace of one is $INSTDIR differing from the InstallDir default it
  ; replaced. A /D= naming exactly the per-user default is indistinguishable
  ; from none -- and harmless for a per-user install, which is where that path
  ; belongs.
  ${If} $INSTDIR != "$LOCALAPPDATA\Programs\Lightning"
    StrCpy $DirFromCommandLine "1"
  ${EndIf}

  Call DetectExistingInstalls

  ; With no switch, follow the installation that is already there, so a
  ; silent `setup.exe /S` -- which is exactly what Lightning 0.9.9 and older
  ; run when they update themselves -- upgrades the copy the user has and
  ; never quietly creates a second one in the other scope. The per-user copy
  ; wins a tie because it is the one this user can update without asking.
  ${If} $ScopeFromCommandLine != "1"
    ${If} $ExistingUserDir != ""
      StrCpy $InstallScope "user"
    ${ElseIf} $ExistingMachineDir != ""
      StrCpy $InstallScope "machine"
    ${EndIf}
  ${EndIf}

  Call ApplyScope

  ; An interactive run with no switch asks on the mode page and elevates from
  ; there. Everything else -- silent, or an explicit /ALLUSERS -- has made its
  ; choice already and elevates now.
  ${If} $InstallScope == "machine"
    !insertmacro LIGHTNING_IS_ELEVATED
    Pop $R3
    ${If} $R3 != 1
      ${If} $ElevatedRelaunch == "1"
        MessageBox MB_ICONSTOP|MB_OK "Installing Lightning for all users needs administrator rights, and Windows did not grant them." /SD IDOK
        SetErrorLevel ${EXIT_ELEVATION_UNAVAILABLE}
        Quit
      ${EndIf}
      ${If} ${Silent}
      ${OrIf} $ScopeFromCommandLine == "1"
        Call ElevateForMachineScopeAndQuit
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

; The elevated copy was started from a page the user has already seen.
Function SkipPageInElevatedCopy
  ${If} $ElevatedRelaunch == "1"
    Abort
  ${EndIf}
FunctionEnd

Function ModePageCreate
  ; A scope chosen on the command line, or already decided by the relaunch,
  ; is not asked again.
  ${If} $ScopeFromCommandLine == "1"
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Installation type" "Choose who can use Lightning on this computer."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  ${NSD_CreateRadioButton} 0 0u 100% 12u "Just for &me (recommended)"
  Pop $ModeRadioUser
  ${NSD_CreateLabel} 12u 13u -12u 20u "Installs into your own profile. No administrator rights are needed, and Lightning keeps itself up to date without asking."
  Pop $0
  ${NSD_CreateRadioButton} 0 38u 100% 12u "For &all users of this computer"
  Pop $ModeRadioMachine
  ${NSD_CreateLabel} 12u 51u -12u 28u "Installs into Program Files for everyone who signs in to this computer. Needs administrator rights now and for every update. Each person's settings and account stay in their own profile."
  Pop $0
  ${If} $ExistingUserDir != ""
    ${NSD_CreateLabel} 0 84u 100% 24u "Lightning is already installed just for you in $ExistingUserDir. Keep 'Just for me' to update it."
    Pop $0
  ${ElseIf} $ExistingMachineDir != ""
    ${NSD_CreateLabel} 0 84u 100% 24u "Lightning is already installed for all users in $ExistingMachineDir. Keep 'For all users' to update it."
    Pop $0
  ${EndIf}
  ${If} $InstallScope == "machine"
    ${NSD_Check} $ModeRadioMachine
  ${Else}
    ${NSD_Check} $ModeRadioUser
  ${EndIf}
  nsDialogs::Show
FunctionEnd

Function ModePageLeave
  ${NSD_GetState} $ModeRadioMachine $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $InstallScope "machine"
  ${Else}
    StrCpy $InstallScope "user"
  ${EndIf}

  ; TWO COPIES IN TWO SCOPES is legal, and confusing: two Start-menu entries,
  ; two entries in Settings -> Apps, and an update to one leaves the other
  ; behind. Say so, and let the person choose.
  ${If} $InstallScope == "machine"
  ${AndIf} $ExistingUserDir != ""
    MessageBox MB_ICONEXCLAMATION|MB_YESNO|MB_DEFBUTTON2 "Lightning is already installed just for you in$\r$\n$ExistingUserDir$\r$\n$\r$\nInstalling it for all users adds a second copy, and updating one does not update the other. Uninstall the existing copy first (Settings > Apps) unless you really want both.$\r$\n$\r$\nInstall a second copy for all users anyway?" IDYES modeMachineConfirmed
    Abort
    modeMachineConfirmed:
  ${EndIf}
  ${If} $InstallScope == "user"
  ${AndIf} $ExistingMachineDir != ""
    MessageBox MB_ICONEXCLAMATION|MB_YESNO|MB_DEFBUTTON2 "Lightning is already installed for all users in$\r$\n$ExistingMachineDir$\r$\n$\r$\nInstalling it just for you adds a second copy, and updating one does not update the other.$\r$\n$\r$\nInstall a second copy just for you anyway?" IDYES modeUserConfirmed
    Abort
    modeUserConfirmed:
  ${EndIf}

  Call ApplyScope

  ${If} $InstallScope == "machine"
    !insertmacro LIGHTNING_IS_ELEVATED
    Pop $R3
    ${If} $R3 != 1
      HideWindow
      Call ElevatedParameters
      !insertmacro LIGHTNING_RUN_ELEVATED "$R1"
      ; Declined, or impossible: come back to this page and say so, so the
      ; person can pick "Just for me" instead of being left with nothing.
      ${If} $R0 = ${EXIT_ELEVATION_DECLINED}
      ${OrIf} $R0 = ${EXIT_ELEVATION_UNAVAILABLE}
        ShowWindow $HWNDPARENT ${SW_SHOW}
        BringToFront
        MessageBox MB_ICONINFORMATION|MB_OK "Installing for all users needs administrator approval, and it was not given. Choose 'Just for me', or try again and approve the prompt."
        Abort
      ${EndIf}
      SetErrorLevel $R0
      Quit
    ${EndIf}
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------
; Install
; ---------------------------------------------------------------------------

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
  ; The marker files decide what the UNINSTALLER and the UPDATER may do:
  ; without the install-root marker the uninstaller refuses forever, without
  ; the install-type marker an installed copy reads as portable and the
  ; updater swaps a directory the installer owns, and without the scope
  ; marker a per-machine copy would be upgraded per-user. Every write is
  ; checked, so a failure cannot leave a broken installation reported as good.
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
  ClearErrors
  FileOpen $0 "$INSTDIR\${SCOPE_MARKER}" w
  IfErrors markerFailed
  FileWrite $0 "$InstallScope$\r$\n"
  FileClose $0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  ; SHCTX is HKCU after `SetShellVarContext current` and HKLM after `all`, so
  ; every registration below lands in the scope ApplyScope chose.
  WriteRegStr SHCTX "${REG_APP}" "InstallDir" "$INSTDIR"
  WriteRegStr SHCTX "${REG_UNINSTALL}" "DisplayName" "Lightning"
  WriteRegStr SHCTX "${REG_UNINSTALL}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr SHCTX "${REG_UNINSTALL}" "Publisher" "${PUBLISHER}"
  WriteRegStr SHCTX "${REG_UNINSTALL}" "DisplayIcon" "$INSTDIR\Lightning.exe"
  WriteRegStr SHCTX "${REG_UNINSTALL}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  ; What deployment tools (Intune, SCCM, WAPT, winget) run to remove it.
  WriteRegStr SHCTX "${REG_UNINSTALL}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr SHCTX "${REG_UNINSTALL}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${REG_UNINSTALL}" "URLInfoAbout" "https://gitlab.smetonis.net/Mizerd/lightning"
  WriteRegDWORD SHCTX "${REG_UNINSTALL}" "NoModify" 1
  WriteRegDWORD SHCTX "${REG_UNINSTALL}" "NoRepair" 1
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
  ; Under "all" this is the Public desktop, so every user sees it.
  CreateShortcut "$DESKTOP\Lightning.lnk" "$INSTDIR\Lightning.exe"
SectionEnd

; ---------------------------------------------------------------------------
; Uninstall
; ---------------------------------------------------------------------------

Function un.onInit
  ; The scope the installer recorded. An installation made before the marker
  ; existed (0.9.9 and older) was always per-user, which is what a missing or
  ; unreadable marker means here.
  StrCpy $InstallScope "user"
  ClearErrors
  FileOpen $0 "$INSTDIR\${SCOPE_MARKER}" r
  ${IfNot} ${Errors}
    FileRead $0 $1
    FileClose $0
    ${If} $1 == "machine$\r$\n"
    ${OrIf} $1 == "machine$\n"
    ${OrIf} $1 == "machine"
      StrCpy $InstallScope "machine"
    ${EndIf}
  ${EndIf}

  ${If} $InstallScope == "machine"
    SetShellVarContext all
    !insertmacro LIGHTNING_IS_ELEVATED
    Pop $R3
    ${If} $R3 != 1
      ${GetParameters} $R1
      ClearErrors
      ${GetOptions} $R1 "/ELEVATED" $R2
      ${IfNot} ${Errors}
        MessageBox MB_ICONSTOP|MB_OK "Lightning is installed for all users, and removing it needs administrator rights that Windows did not grant." /SD IDOK
        SetErrorLevel ${EXIT_ELEVATION_UNAVAILABLE}
        Quit
      ${EndIf}
      ; This is the copy NSIS runs from %TEMP% with `_?=<install dir>`, and
      ; NSIS cuts `_?=` out of $CMDLINE exactly as it does /D=. Without it the
      ; elevated copy would take %TEMP% for the installation, find no markers
      ; and refuse; so it is put back from $INSTDIR, LAST and unquoted.
      !insertmacro LIGHTNING_RUN_ELEVATED "/ELEVATED $R1 _?=$INSTDIR"
      SetErrorLevel $R0
      Quit
    ${EndIf}
  ${Else}
    SetShellVarContext current
  ${EndIf}
FunctionEnd

Section "Uninstall"
  IfFileExists "$INSTDIR\.lightning-install-root" 0 unsafe_uninstall
  IfFileExists "$INSTDIR\Lightning.exe" 0 unsafe_uninstall
  Delete "$DESKTOP\Lightning.lnk"
  Delete "$SMPROGRAMS\Lightning\Lightning.lnk"
  Delete "$SMPROGRAMS\Lightning\Uninstall Lightning.lnk"
  RMDir "$SMPROGRAMS\Lightning"
  DeleteRegKey SHCTX "${REG_UNINSTALL}"
  DeleteRegKey SHCTX "${REG_APP}"
  RMDir /r "$INSTDIR"
  ; User profiles, Matrix stores, and settings live outside $INSTDIR and are
  ; intentionally never removed by this uninstaller -- for a per-machine
  ; installation that means every user's own profile, none of which it reads.
  Goto uninstall_done
unsafe_uninstall:
  MessageBox MB_ICONSTOP "Lightning install markers are missing; refusing recursive removal." /SD IDOK
  Abort
uninstall_done:
SectionEnd
