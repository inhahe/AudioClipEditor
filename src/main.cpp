#include "app.h"
#include <cwchar>

int runSelfTest();  // selftest.cpp

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    if (cmdLine && wcsstr(cmdLine, L"--selftest"))
        return runSelfTest();
    return runApp(hInst, nCmdShow);
}
