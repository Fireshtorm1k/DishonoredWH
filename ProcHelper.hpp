// proc_pid_by_name.hpp
#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <algorithm>
#include <cctype>


    using pid_opt = std::optional<int>;

    // -------------------------- Windows --------------------------
#  define NOMINMAX
#  include <windows.h>
#  include <tlhelp32.h>
#  include <cwctype>

    inline std::wstring widen_utf8(std::string_view s) {
        if (s.empty()) return {};
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
        return w;
    }

    inline void tolower_inplace(std::wstring& w) {
        std::transform(w.begin(), w.end(), w.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    }

    inline std::wstring strip_exe_suffix(std::wstring w) {
        auto has_suffix = [](const std::wstring& x) {
            if (x.size() < 4) return false;
            std::wstring tail = x.substr(x.size() - 4);
            tolower_inplace(tail);
            return tail == L".exe";
            };
        if (has_suffix(w)) w.erase(w.end() - 4, w.end());
        return w;
    }

    inline bool equal_names_windows(std::wstring a, std::wstring b) {
        tolower_inplace(a); tolower_inplace(b);
        if (a == b) return true;
        return strip_exe_suffix(a) == strip_exe_suffix(b);
    }

    inline pid_opt pid_by_name(std::string_view name_utf8) {
        const std::wstring needle = widen_utf8(name_utf8);

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return std::nullopt;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (!Process32FirstW(snap, &pe)) {
            CloseHandle(snap);
            return std::nullopt;
        }

        pid_opt found;
        do {
            if (equal_names_windows(needle, pe.szExeFile)) {
                found = static_cast<int>(pe.th32ProcessID);
                break;
            }
        } while (Process32NextW(snap, &pe));

        CloseHandle(snap);
        return found;
    }


    uintptr_t GetModuleBaseAddress(DWORD pid, const wchar_t* moduleName)
    {
        uintptr_t baseAddress = 0;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (hSnap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W me;
            me.dwSize = sizeof(me);
            if (Module32FirstW(hSnap, &me))
            {
                do
                {
                    if (!_wcsicmp(me.szModule, moduleName))
                    {
                        baseAddress = (uintptr_t)me.modBaseAddr;
                        break;
                    }
                } while (Module32NextW(hSnap, &me));
            }
            CloseHandle(hSnap);
        }
        return baseAddress;
    }
