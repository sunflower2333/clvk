// SPDX-License-Identifier: Apache-2.0
#include "../src/windows_driver_path.hpp"
#include "../src/utils.hpp"
#include <CL/cl.h>
#include <cstdio>
#include <fstream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    // The runtime is loaded by absolute path from a different cwd, with its
    // actual private imports. No Vulkan device enumeration is needed here.
    auto runtime_path = std::filesystem::absolute(argv[1]).make_preferred();
    HMODULE runtime = LoadLibraryExW(runtime_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) {
        DWORD error = GetLastError();
        auto vk_path = (runtime_path.parent_path() / argv[2]).make_preferred();
        HMODULE dependency = LoadLibraryExW(vk_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        std::printf("FAIL runtime load error=%lu dependency=%p dependency_error=%lu path=%ls\n",
                    error, static_cast<void*>(dependency), GetLastError(), runtime_path.c_str());
        return 1;
    }
    using Extension = void*(CL_API_CALL*)(const char*);
    auto extension = reinterpret_cast<Extension>(GetProcAddress(runtime, "clGetExtensionFunctionAddress"));
    if (!extension || !extension("clIcdGetPlatformIDsKHR") || extension("clDefinitelyMissingVIOGPU")) return 1;
    HMODULE vk = GetModuleHandleW(argv[2]);
    std::vector<wchar_t> loaded(32768);
    if (!vk || !GetModuleFileNameW(vk, loaded.data(), DWORD(loaded.size())) ||
        !std::filesystem::equivalent(std::filesystem::path(argv[1]).parent_path() / argv[2], loaded.data())) return 1;
    auto compiler = cvk_windows_module_sibling(reinterpret_cast<const void*>(extension), L"viogpu_clspv_x64.exe");
    if (compiler != std::filesystem::path(argv[1]).parent_path() / L"viogpu_clspv_x64.exe") return 1;
    std::string output;
    if (cvk_exec(cvk_quote_path(compiler.string()) + " --version", &output) != 0 || output.empty()) return 1;
    // Compile actual OpenCL source through the production process invocation,
    // using spaces and shell metacharacters in the temporary input path.
    auto temporary = std::filesystem::temp_directory_path() /
        ("clvk flat & compile " + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directory(temporary);
    auto input = temporary / "kernel.cl", binary = temporary / "kernel.spv";
    { std::ofstream file(input); file << "kernel void add(global uint* p) { p[get_global_id(0)] += 7; }\n"; }
    int status = cvk_exec(cvk_quote_path(compiler.string()) + " " + cvk_quote_path(input.string()) +
                          " -o " + cvk_quote_path(binary.string()), &output);
    uint32_t magic = 0;
    { std::ifstream file(binary, std::ios::binary); file.read(reinterpret_cast<char*>(&magic), sizeof(magic)); }
    std::filesystem::remove_all(temporary);
    if (status || magic != 0x07230203) { std::printf("FAIL compile status=%d %s\n", status, output.c_str()); return 1; }
    std::printf("PASS flat runtime/imports/ICD lookup and sibling x64 compiler produced SPIR-V; pointer_bits=%zu; no GPU execution\n", sizeof(void*) * 8);
    return 0;
}
