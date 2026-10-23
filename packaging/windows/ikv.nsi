; NONCANONICAL wave 17 (server/hosting since wave 19): IKV Windows installer (per-user, no administrator rights). Built by tools/package_windows.py:
;   makensis -INPUTCHARSET UTF8 -DVERSION=X.Y.Z -DSTAGE=<install tree> -DUNINSTALL_LIST=<uninstall.nsh> -DOUTFILE=<Setup.exe> ikv.nsi
; UNINSTALL_LIST (generated from the staged tree) deletes only the files this package installed and then only the
; directories left empty: the install folder may be user-chosen and contain unrelated files.
; Silent install: Setup.exe /S [/D=C:\path]; silent uninstall: Uninstall.exe /S.
; Game data, settings and accounts live in the user profile (Qt standard paths, IKVCommunity), not here; the uninstaller leaves them.
Unicode true
!include MUI2.nsh
!define APP "İstanbul Kıyamet Vakti"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\IKV"
Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\IKV"
InstallDirRegKey HKCU "Software\IKV" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "${APP}"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileDescription" "${APP} native client, launcher and server setup"
VIAddVersionKey /LANG=0 "LegalCopyright" "IKV community port; original game data is not included"

!define MUI_ICON "${__FILEDIR__}/ikv.ico"
!define MUI_UNICON "${__FILEDIR__}/ikv.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\ikv-launcher.exe"
!define MUI_FINISHPAGE_TEXT "$(prereq)"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "English"
LangString prereq ${LANG_TURKISH} "Kurulum tamamlandı.$\r$\n$\r$\nGereksinimler: 64-bit Windows 10/11 ve Vulkan 1.2 ile OpenGL 3.3 destekleyen güncel bir ekran kartı sürücüsü (vulkan-1.dll sürücüyle gelir). Oyun verisi için orijinal kurulum dosyanız (istanbulkur6_0.exe) gerekir; başlatıcı onu içe aktarır. Başlatıcının Sunucu Kur bölümü bu bilgisayarda özel bir sunucu kurar (PostgreSQL 16 dahil); LAN modunda Windows Güvenlik Duvarı izin sorar."
LangString prereq ${LANG_ENGLISH} "Setup finished.$\r$\n$\r$\nRequirements: 64-bit Windows 10/11 and a current graphics driver with Vulkan 1.2 and OpenGL 3.3 (vulkan-1.dll comes with the driver). Game data comes from your own original installer (istanbulkur6_0.exe), imported by the launcher. The launcher's Sunucu Kur hosts a private server on this PC (PostgreSQL 16 included); in LAN mode Windows Firewall asks for permission."

Section
    SetOutPath "$INSTDIR"
    ; Remove the previous version's files with its own uninstaller (its exact file list; _?= runs it in place).
    IfFileExists "$INSTDIR\Uninstall.exe" 0 +2
        ExecWait '"$INSTDIR\Uninstall.exe" /S _?=$INSTDIR'
    File /r "${STAGE}/*"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\bin\ikv-launcher.exe"
    CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\bin\ikv-launcher.exe"
    WriteRegStr HKCU "Software\IKV" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINST}" "DisplayName" "${APP}"
    WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINST}" "Publisher" "IKV community"
    WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\bin\ikv-launcher.exe"
    WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section Uninstall
    Delete "$SMPROGRAMS\${APP}.lnk"
    Delete "$DESKTOP\${APP}.lnk"
    !include "${UNINSTALL_LIST}"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKCU "${UNINST}"
    DeleteRegKey HKCU "Software\IKV"
SectionEnd
