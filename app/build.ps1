$ErrorActionPreference = "Stop"
$Project = Split-Path -Parent $MyInvocation.MyCommand.Path
$Venv = Join-Path $Project ".venv"
$Python = Join-Path $Venv "Scripts\python.exe"

if (-not (Test-Path $Python)) {
    py -3.12 -m venv $Venv
}

& $Python -m pip install --upgrade pip
& $Python -m pip install -r (Join-Path $Project "requirements-build.txt")
& $Python -m PyInstaller --noconfirm --clean (Join-Path $Project "NMSE_OLEDer_RS485.spec")

Write-Host ""
Write-Host "Ready: $Project\dist\NMSE_OLEDer_RS485.exe" -ForegroundColor Green
