# Convert icons to DDS for Skyrim VR UI
# 1. Resize images to 512x512 (scaled to fit, padded with transparency)
# 2. Convert to DDS using texconv
# 3. Move source and DDS to converted folder
# 4. Copy DDS to mod texture folder

param(
    [string]$ModTextureDir = "C:\games\skyrim\VRDEV\mods\DressVR\textures\3DUI\icons"
)

$ErrorActionPreference = "Stop"

# Paths
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ConvertMeDir = Join-Path $ScriptDir "convert_me"
$ConvertedDir = Join-Path $ScriptDir "converted"
$ResizeScript = Join-Path $ScriptDir "resize_image.py"

# Python via pyenv
$Python = "$env:USERPROFILE\.pyenv\pyenv-win\shims\python.bat"

# texconv settings
$TexconvPath = "texconv"
$DdsFormat = "BC3_UNORM"

# Ensure directories exist
if (-not (Test-Path $ConvertedDir)) {
    New-Item -ItemType Directory -Path $ConvertedDir | Out-Null
    Write-Host "Created converted directory"
}

if (-not (Test-Path $ModTextureDir)) {
    New-Item -ItemType Directory -Path $ModTextureDir -Force | Out-Null
    Write-Host "Created mod texture directory: $ModTextureDir"
}

# Get all image files in convert_me
$ImageExtensions = @("*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga", "*.tiff")
$ImageFiles = @()
foreach ($ext in $ImageExtensions) {
    $ImageFiles += Get-ChildItem -Path $ConvertMeDir -Filter $ext -ErrorAction SilentlyContinue
}

if ($ImageFiles.Count -eq 0) {
    Write-Host "No images found in $ConvertMeDir"
    exit 0
}

Write-Host "=" * 60
Write-Host "Icon Converter - Processing $($ImageFiles.Count) images"
Write-Host "=" * 60

$ProcessedCount = 0
$FailedCount = 0

foreach ($ImageFile in $ImageFiles) {
    $BaseName = [System.IO.Path]::GetFileNameWithoutExtension($ImageFile.Name)
    Write-Host ""
    Write-Host "Processing: $($ImageFile.Name)"
    Write-Host "-" * 40

    # Paths for this image
    $SourcePath = $ImageFile.FullName
    $ResizedPath = Join-Path $ConvertMeDir "${BaseName}_512.png"
    $DdsPath = Join-Path $ConvertMeDir "${BaseName}_512.dds"

    # Destination paths
    $DestSourcePath = Join-Path $ConvertedDir $ImageFile.Name
    $DestDdsPath = Join-Path $ConvertedDir "${BaseName}.dds"

    try {
        # Step 1: Resize image to 512x512 square (padded with transparency)
        Write-Host "  Resizing to 512x512..."
        & $Python $ResizeScript $SourcePath $ResizedPath
        if ($LASTEXITCODE -ne 0) {
            throw "Python resize failed with exit code $LASTEXITCODE"
        }

        # Step 2: Convert to DDS using texconv
        Write-Host "  Converting to DDS..."
        $texconvResult = & $TexconvPath -f $DdsFormat -y -o $ConvertMeDir $ResizedPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "texconv failed: $texconvResult"
        }
        Write-Host "  Created: ${BaseName}_512.dds"

        # Step 3: Move source image to converted folder
        Write-Host "  Moving source to converted..."
        Move-Item -Path $SourcePath -Destination $DestSourcePath -Force

        # Step 4: Rename and move DDS to converted folder (remove _512 suffix)
        Write-Host "  Moving DDS to converted..."
        Move-Item -Path $DdsPath -Destination $DestDdsPath -Force

        # Step 5: Delete the resized PNG (intermediate file)
        Write-Host "  Cleaning up resized PNG..."
        Remove-Item -Path $ResizedPath -Force

        $ProcessedCount++
        Write-Host "  Done!" -ForegroundColor Green

    } catch {
        Write-Host "  ERROR: $_" -ForegroundColor Red
        $FailedCount++

        # Clean up any intermediate files on error
        if (Test-Path $ResizedPath) { Remove-Item -Path $ResizedPath -Force -ErrorAction SilentlyContinue }
        if (Test-Path $DdsPath) { Remove-Item -Path $DdsPath -Force -ErrorAction SilentlyContinue }
    }
}

Write-Host ""
Write-Host "=" * 60
Write-Host "Conversion Complete"
Write-Host "  Processed: $ProcessedCount"
Write-Host "  Failed: $FailedCount"
Write-Host "=" * 60

# Step 6: Copy all DDS files from converted to mod texture folder
Write-Host ""
Write-Host "Deploying DDS files to mod folder..."
$DdsFiles = Get-ChildItem -Path $ConvertedDir -Filter "*.dds" -ErrorAction SilentlyContinue

if ($DdsFiles.Count -gt 0) {
    foreach ($DdsFile in $DdsFiles) {
        $DestPath = Join-Path $ModTextureDir $DdsFile.Name
        Copy-Item -Path $DdsFile.FullName -Destination $DestPath -Force
        Write-Host "  Copied: $($DdsFile.Name) -> $ModTextureDir"
    }
    Write-Host ""
    Write-Host "Deployed $($DdsFiles.Count) DDS files to: $ModTextureDir" -ForegroundColor Green
} else {
    Write-Host "No DDS files to deploy"
}

Write-Host ""
Write-Host "All done!" -ForegroundColor Cyan
