# DressUpVR Build Script - delegates to main build script
param(
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [switch]$Clean,
    [switch]$Reconfigure
)
$mainScript = Join-Path $PSScriptRoot "..\..\build-skse-mods.ps1"
& $mainScript -Mod VR-Dress-Up -Config $Config -Clean:$Clean -Reconfigure:$Reconfigure
exit $LASTEXITCODE
