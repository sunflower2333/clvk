// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#include <vector>

// Resolve from this module, independently of cwd, PATH or an app-local DLL.
// Return the absolute intended sibling even if it is missing: never fall back
// to an unrelated compiler executable from the application's search path.
inline std::filesystem::path cvk_windows_module_sibling(const void* anchor,
                                                       const wchar_t* name) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(anchor), &module))
        return {};
    std::vector<wchar_t> path(32768);
    DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (!length || length >= path.size()) return {};
    return std::filesystem::path(path.data()).parent_path() / name;
}
#endif
