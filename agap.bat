@echo off
cd /d "%~dp0"
where python >nul 2>nul && (python agap.py %*) || (py -3 agap.py %*)
if "%~1"=="" pause
