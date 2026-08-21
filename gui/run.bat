@echo off
chcp 65001 >nul
echo Starting AIMP NCM GUI...
python "%~dp0app.py"
if errorlevel 1 (
  echo Try: pip install -r requirements.txt
  pause
)
