# Build script for VinStyleRankGauge
# Run from VS2022 Developer PowerShell, or this script sets up the environment

$ErrorActionPreference = "Stop"

# Setup VS2022 environment if cl.exe not available
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $vcvars) {
        Write-Host "Setting up VS2022 environment..."
        cmd /c "`"$vcvars`" amd64 && set" | ForEach-Object {
            if ($_ -match "^(.+?)=(.*)$") {
                [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
            }
        }
    } else {
        Write-Error "VS2022 not found!"
        exit 1
    }
}

$buildDir = "$PSScriptRoot\build"
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Push-Location $buildDir
try {
    Write-Host "Configuring CMake..."
    cmake .. -G "Visual Studio 17 2022" -A x64

    Write-Host "Building Release..."
    cmake --build . --config Release

    $dll = "$buildDir\Release\VinStyleRankGauge.dll"
    if (Test-Path $dll) {
        Write-Host "`nBuild successful!"
        Write-Host "DLL: $dll"
        Write-Host "Size: $((Get-Item $dll).Length) bytes"
    } else {
        Write-Error "Build failed - DLL not found"
    }
} finally {
    Pop-Location
}
