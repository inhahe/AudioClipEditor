#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Entry point for the application UI. Creates the main window and runs the loop.
// `args` are the command-line arguments after the program name: a project to open
// and audio files to add (see launch.h).
int runApp(HINSTANCE hInst, int nCmdShow, const std::vector<std::wstring>& args);
