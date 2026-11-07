@echo off
title Race Broadcast - voice check
echo Plays one line from each commentator (sets up the voices first if needed).
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0race_broadcast_commentary.ps1" -Test
pause
