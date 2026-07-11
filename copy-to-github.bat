@echo off
rem ---------------------------------------------------------------------------
rem Copy the source files needed to build/push Audio Clip Editor to the GitHub
rem working copy at D:\github\audioclipeditor.
rem
rem Uses robocopy WITHOUT /MIR or /PURGE, so it only adds/updates files and
rem NEVER deletes anything already present in the destination. Build artifacts
rem (build\, bin\, *.log, *.obj, *.exe, *.user) are intentionally skipped.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "DST=D:\github\audioclipeditor"

echo Copying to "%DST%" ...
if not exist "%DST%"      mkdir "%DST%"
if not exist "%DST%\src"  mkdir "%DST%\src"

rem --- root files ---
robocopy "." "%DST%" CMakeLists.txt README.md .gitignore todo.txt copy-to-github.bat /NFL /NDL /NJH /NJS /NP

rem --- source tree (headers, sources, resource + manifest) ---
robocopy "src" "%DST%\src" *.h *.cpp *.rc *.manifest /E /NFL /NDL /NJH /NJS /NP

echo.
echo Done. Files copied to "%DST%" (nothing was deleted).
endlocal
