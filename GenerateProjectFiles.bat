@echo off
rem Generate Visual Studio solution (BoxingHawkEyeSceneCreator.sln)
if "%UE_ROOT%"=="" set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
call "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" -projectfiles -project="%~dp0BoxingHawkEyeSceneCreator.uproject" -game -rocket -progress
if errorlevel 1 pause
