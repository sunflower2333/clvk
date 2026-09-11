// Copyright 2018 The clvk authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "utils.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

#ifdef __APPLE__
#include <unistd.h>
#endif

#ifdef WIN32
#include <Windows.h>
#include <io.h>
#endif

#if !defined(WIN32) && !defined(__APPLE__)
#include <pthread.h>
#endif

char* cvk_mkdtemp(std::string& tmpl) {
#ifdef WIN32
    if (_mktemp_s(&tmpl.front(), tmpl.size() + 1) != 0) {
        return nullptr;
    }

    if (!CreateDirectory(tmpl.c_str(), nullptr)) {
        return nullptr;
    }

    return &tmpl.front();
#else
    return mkdtemp(&tmpl.front());
#endif
}

std::string cvk_quote_path(const std::string& path) {
#ifdef WIN32
    // Windows filenames cannot contain quotes. Double trailing backslashes
    // because the CRT parser otherwise treats the closing quote as literal.
    size_t trailing = path.size() - path.find_last_not_of('\\') - 1;
    return "\"" + path + std::string(trailing, '\\') + "\"";
#else
    std::string result = "'";
    for (char c : path) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
#endif
}

int cvk_exec(const std::string& cmd, std::string* output) {
#ifdef WIN32
    // Invoke the compiler directly: cmd.exe misparses quoted executable paths
    // and expands shell metacharacters in temporary directory names/options.
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return -1;
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return -1;
    }
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {};
    std::vector<char> command(cmd.begin(), cmd.end());
    command.push_back('\0');
    BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &startup, &process);
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        if (output) *output = "CreateProcess failed: " + std::to_string(error);
        return -1;
    }
    std::string captured;
    char buffer[4096];
    DWORD count;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &count, nullptr) && count)
        captured.append(buffer, count);
    CloseHandle(read_pipe);
    DWORD exit_code = 1;
    bool success = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                   GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (output) *output = std::move(captured);
    return success ? static_cast<int>(exit_code) : -1;
#else
    cvk_info("About to run \"%s\"", cmd.c_str());

    std::array<char, 512> buffer;
    std::string out;
    std::string cmd_with_err = cmd + " 2>&1";
    FILE* pipe = popen(cmd_with_err.c_str(), "r");

    if (pipe == nullptr) {
        return -1;
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        out += buffer.data();
    }

    if (output != nullptr) {
        *output = std::move(out);
    }

    int ret = pclose(pipe);

    cvk_info("Return code was: %d", ret);

    return ret;
#endif
}

void cvk_set_current_thread_name_if_supported(const std::string& name) {
#if !defined(WIN32) && !defined(__APPLE__)
    pthread_setname_np(pthread_self(), name.c_str());
#endif
}
