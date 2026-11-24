@echo off
rem Launch BoxingHawkEyeSceneCreator with the MCP server on port 8000
if "%UE_ROOT%"=="" set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
start "" "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "%~dp0BoxingHawkEyeSceneCreator.uproject" -ModelContextProtocolPort=8000
