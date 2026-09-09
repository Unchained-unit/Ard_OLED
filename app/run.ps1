$ErrorActionPreference = "Stop"
$Project = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python = Join-Path $Project ".venv\Scripts\python.exe"

if (-not (Test-Path $Python)) {
    py -3.12 -m venv (Join-Path $Project ".venv")
    & $Python -m pip install --upgrade pip
    & $Python -m pip install -r (Join-Path $Project "requirements.txt")
}

& $Python (Join-Path $Project "web_station\main.py")
