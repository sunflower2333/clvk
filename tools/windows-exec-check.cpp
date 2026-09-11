// SPDX-License-Identifier: Apache-2.0
#include "utils.hpp"
#include <windows.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc == 3) {
        std::printf("%s", argv[2]);
        std::fprintf(stderr, " stderr");
        return 7;
    }
    char executable[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, executable, MAX_PATH)) return 1;
    std::string argument = "spaces & literal %PATH% and trailing\\";
    std::string output;
    int status = cvk_exec(cvk_quote_path(executable) + " child " +
                          cvk_quote_path(argument), &output);
    if (status != 7 || output.find(argument) == std::string::npos ||
        output.find("stderr") == std::string::npos) {
        std::fprintf(stderr, "FAIL status=%d output=%s\n", status, output.c_str());
        return 1;
    }
    if (cvk_exec("definitely-missing-clvk-compiler.exe", &output) == 0 ||
        output.find("CreateProcess failed") == std::string::npos) return 1;
    std::puts("PASS compiler subprocess argument quoting, stdout/stderr, exit status, launch failure");
    return 0;
}
