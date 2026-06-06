# Local build script — requires Visual Studio Build Tools (NOT full Visual Studio IDE).
# Install with: winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
#
# Or skip all local installs and use GitHub Actions instead (see README build section).

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Ensure-ImGui {
    if (Test-Path "imgui\imgui.h") { return }

    $sdk = "$env:APPDATA\bakkesmod\bakkesmod\bakkesmodsdk"
    if (-not (Test-Path "$sdk\imgui_bm.zip")) {
        throw "imgui sources missing. Install BakkesMod or run the GitHub Actions build."
    }

    Expand-Archive "$sdk\imgui_bm.zip" -DestinationPath . -Force
    Copy-Item imgui IMGUI -Recurse -Force

    if (-not (Test-Path "IMGUI\imgui_stdlib.h")) {
        Invoke-WebRequest "https://raw.githubusercontent.com/ocornut/imgui/master/misc/cpp/imgui_stdlib.h" -OutFile IMGUI/imgui_stdlib.h
        Invoke-WebRequest "https://raw.githubusercontent.com/ocornut/imgui/master/misc/cpp/imgui_stdlib.cpp" -OutFile IMGUI/imgui_stdlib.cpp
    }
}

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($path) { return $path }
    }
    throw @"
MSBuild not found.

Option A — Cloud build (no install):
  Push to GitHub, open Actions tab, download CustomMapsPlugin artifact.

Option B — Minimal local install (~2 GB, no IDE):
  winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
"@
}

Ensure-ImGui
$msbuild = Find-MSBuild

& $msbuild CustomMapsPLugin.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
Copy-Item plugins/CustomMapsPLugin.dll plugins/CustomMapsPlugin.dll -Force

Write-Host ""
Write-Host "Built: plugins/CustomMapsPlugin.dll" -ForegroundColor Green
Write-Host "Copy to: $env:APPDATA\bakkesmod\bakkesmod\plugins\"
