; Machine-wide URL protocol registration (mirrors macOS CFBundleURLSchemes).
; Included by CPack-generated NSIS installer via CPACK_NSIS_INSTALL_SCRIPT.
; On 64-bit Windows, 32-bit NSIS must use the 64-bit registry view so the 64-bit app sees these keys.
SetRegView 64

WriteRegStr HKLM "Software\Classes\snapmaker-orca" "" "URL:Snapmaker Orca"
WriteRegStr HKLM "Software\Classes\snapmaker-orca" "URL Protocol" ""
WriteRegStr HKLM "Software\Classes\snapmaker-orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'

WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "" "URL:Snapmaker Orca"
WriteRegStr HKLM "Software\Classes\Snapmaker_Orca" "URL Protocol" ""
WriteRegStr HKLM "Software\Classes\Snapmaker_Orca\shell\open\command" "" '"$INSTDIR\snapmaker-orca.exe" "%1"'

SetRegView 32
