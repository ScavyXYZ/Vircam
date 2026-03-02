#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <sstream>
#include <vector>
#include <iostream>
#include <shellapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

bool IsRunAsAdministrator() {
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            isElevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }

    return isElevated != FALSE;
}

bool RunAsAdministrator() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, MAX_PATH)) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.hwnd = NULL;
        sei.nShow = SW_NORMAL;

        if (!ShellExecuteExW(&sei)) {
            DWORD dwError = GetLastError();
            if (dwError == ERROR_CANCELLED) {
                std::wcout << L"ERROR: Administrator privileges are required but were denied by user." << std::endl;
            }
            return false;
        }
        return true;
    }
    return false;
}

bool unregisterWithRegsvr32(const std::wstring& dllPath) {
    std::wstring params = L"/u /s \"" + dllPath + L"\"";

    SHELLEXECUTEINFOW sei = { 0 };
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    sei.lpParameters = params.c_str();
    sei.hwnd = NULL;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess != NULL) {
            WaitForSingleObject(sei.hProcess, 10000);
            DWORD exitCode;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            return exitCode == 0;
        }
    }

    return false;
}

std::wstring uninstallAllDrivers() {
    std::wstringstream result;
    std::vector<std::wstring> versions = { L"20", L"30", L"60" };
    int totalFound = 0;
    int successCount = 0;
    int errorCount = 0;

    wchar_t currentDir[MAX_PATH];
    if (!GetCurrentDirectoryW(MAX_PATH, currentDir)) {
        return L"ERROR: Failed to get current directory.";
    }

    wchar_t driversPath[MAX_PATH];
    if (!PathCombineW(driversPath, currentDir, L"Drivers")) {
        return L"ERROR: Failed to build drivers path.";
    }

    if (!PathIsDirectoryW(driversPath)) {
        return L"SUCCESS: Drivers folder not found. Nothing to uninstall.";
    }

    for (const auto& version : versions) {
        wchar_t versionPath[MAX_PATH];
        if (!PathCombineW(versionPath, driversPath, version.c_str())) {
            continue;
        }

        if (!PathIsDirectoryW(versionPath)) {
            continue;
        }

        wchar_t dll32Path[MAX_PATH];
        PathCombineW(dll32Path, versionPath, L"VircamDriver32.dll");

        if (PathFileExistsW(dll32Path)) {
            totalFound++;
            if (unregisterWithRegsvr32(dll32Path)) {
                result << L"SUCCESS: Unregistered " << dll32Path << L"\n";
                successCount++;
            }
            else {
                result << L"ERROR: Failed to unregister " << dll32Path << L"\n";
                errorCount++;
            }
        }

        wchar_t dll64Path[MAX_PATH];
        PathCombineW(dll64Path, versionPath, L"VircamDriver64.dll");

        if (PathFileExistsW(dll64Path)) {
            totalFound++;
            if (unregisterWithRegsvr32(dll64Path)) {
                result << L"SUCCESS: Unregistered " << dll64Path << L"\n";
                successCount++;
            }
            else {
                result << L"ERROR: Failed to unregister " << dll64Path << L"\n";
                errorCount++;
            }
        }
    }

    std::wstringstream finalResult;

    if (totalFound == 0) {
        finalResult << L"SUCCESS: No driver DLLs found to uninstall.\n";
        return finalResult.str();
    }

    if (successCount > 0) {
        finalResult << L"SUCCESS: Successfully uninstalled " << successCount
            << L" out of " << totalFound << L" driver(s).\n";
        finalResult << result.str();
        return finalResult.str();
    }
    else {
        finalResult << L"ERROR: Failed to uninstall any drivers (" << errorCount
            << L" errors encountered).\n";
        finalResult << result.str();
        return finalResult.str();
    }
}

int wmain() {
    // Check if running as administrator
    if (!IsRunAsAdministrator()) {
        std::wcout << L"Requesting administrator privileges..." << std::endl;

        // Re-run as administrator
        if (RunAsAdministrator()) {
            return 0; // The elevated process will handle everything
        }
        else {
            return 1; // Failed to elevate
        }
    }

    // If we get here, we're running as administrator
    std::wstring result = uninstallAllDrivers();
    std::wcout << result << std::endl;

    // Wait for user input before closing (optional)
    std::wcout << L"\nPress Enter to exit...";
    std::wcin.get();

    if (result.find(L"ERROR:") == 0) {
        return 1;
    }

    return 0;
}