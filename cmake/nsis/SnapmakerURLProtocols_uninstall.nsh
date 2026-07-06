; Remove URL protocol keys installed by SnapmakerURLProtocols_install.nsh
SetRegView 64
DeleteRegKey HKLM "Software\Classes\snapmaker-orca"
DeleteRegKey HKLM "Software\Classes\Snapmaker_Orca"
SetRegView 32
