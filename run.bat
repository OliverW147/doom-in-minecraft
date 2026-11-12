@echo off
rem Starts the server from its own folder (it needs DOOM1.WAD, tags.bin and registries.bin next to it).
cd /d "%~dp0"
doom-mc-server.exe %*
