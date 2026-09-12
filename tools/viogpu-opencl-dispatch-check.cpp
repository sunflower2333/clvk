// SPDX-License-Identifier: Apache-2.0
// Small deterministic dispatch/batch control, NOT a Geekbench kernel replay.
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void check(cl_int result, const char* operation) {
    if (result != CL_SUCCESS) {
        std::fprintf(stderr, "FAIL operation=%s status=%d\n", operation, result);
        std::exit(1);
    }
}
static unsigned number(const char* value, unsigned maximum) {
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (!*value || *end || parsed == 0 || parsed > maximum) std::exit(2);
    return static_cast<unsigned>(parsed);
}
static cl_uint transform(cl_uint value, unsigned iterations) {
    for (unsigned i = 0; i < iterations; ++i) {
        value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    }
    return value;
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned iterations = 64, dispatches = 4, groups = 16, queues = 2;
    if (argc == 2 && !std::strcmp(argv[1], "--help")) {
        std::puts("Small GPU dispatch control, not GB7 replay. Options: --iterations 1..4096 --dispatches 1..32 --groups 1..256 --queues 1..2. Total work <=16777216 integer iterations. Local size=16.");
        return 0;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        if (!std::strcmp(argv[i], "--iterations")) iterations = number(argv[i+1], 4096);
        else if (!std::strcmp(argv[i], "--dispatches")) dispatches = number(argv[i+1], 32);
        else if (!std::strcmp(argv[i], "--groups")) groups = number(argv[i+1], 256);
        else if (!std::strcmp(argv[i], "--queues")) queues = number(argv[i+1], 2);
        else return 2;
    }
    const size_t local = 16, global = groups * local;
    if (uint64_t(iterations) * dispatches * queues * global > 16777216) return 2;
    std::printf("CONTROL pid=%lu bits=%zu iterations=%u dispatches=%u queues=%u gws=%zu lws=%zu\n",
        GetCurrentProcessId(), sizeof(void*) * 8, iterations, dispatches, queues, global, local);
    cl_uint count = 0; check(clGetPlatformIDs(0, nullptr, &count), "platform count");
    std::vector<cl_platform_id> platforms(count);
    check(clGetPlatformIDs(count, platforms.data(), nullptr), "platforms");
    cl_device_id device = nullptr;
    for (auto platform : platforms) {
        cl_uint n = 0; const cl_int result = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &n);
        if (result == CL_DEVICE_NOT_FOUND) continue;
        check(result, "device count"); std::vector<cl_device_id> devices(n);
        check(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, n, devices.data(), nullptr), "devices");
        for (auto candidate : devices) {
            char name[512]{};
            check(clGetDeviceInfo(candidate, CL_DEVICE_NAME, sizeof(name), name, nullptr), "device name");
            if (std::strstr(name, "Adreno") || std::strstr(name, "Turnip")) {
                device = candidate; std::printf("GPU=%s\n", name);
            }
        }
    }
    if (!device) { std::fputs("FAIL no Adreno/Turnip GPU\n", stderr); return 1; }
    for (const char* name : {"OpenCL.dll", "viogpucl.dll", "vulkan-1.dll"}) {
        const auto module = GetModuleHandleA(name); char path[32768]{};
        if (module && GetModuleFileNameA(module, path, sizeof(path))) std::printf("MODULE %s=%s\n", name, path);
    }
    cl_int result;
    auto context = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &result); check(result, "context");
    const char* source = "__kernel void dispatch_control(__global uint* data,uint iterations){"
        "size_t i=get_global_id(0);uint v=data[i];for(uint n=0;n<iterations;++n){"
        "v^=v<<13;v^=v>>17;v^=v<<5;}data[i]=v;}";
    auto program = clCreateProgramWithSource(context, 1, &source, nullptr, &result); check(result, "program");
    result = clBuildProgram(program, 1, &device, "-cl-std=CL1.2", nullptr, nullptr);
    if (result != CL_SUCCESS) {
        size_t bytes = 0; clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &bytes);
        std::vector<char> log(bytes + 1); clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, bytes, log.data(), nullptr);
        std::fprintf(stderr, "BUILD_LOG %s\n", log.data()); check(result, "compile");
    }
    auto kernel = clCreateKernel(program, "dispatch_control", &result); check(result, "kernel");
    std::array<cl_command_queue, 2> command_queues{};
    std::array<cl_mem, 2> buffers{};
    std::array<std::vector<cl_event>, 2> events;
    std::vector<cl_uint> initial(global), output(global);
    for (size_t i = 0; i < global; ++i) initial[i] = cl_uint(i * 17 + 11);
    for (unsigned q = 0; q < queues; ++q) {
        command_queues[q] = clCreateCommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &result); check(result, "queue");
        buffers[q] = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
            global * sizeof(cl_uint), initial.data(), &result); check(result, "buffer");
        events[q].resize(dispatches);
    }
    const auto start = std::chrono::steady_clock::now();
    check(clSetKernelArg(kernel, 1, sizeof(iterations), &iterations), "iteration arg");
    for (unsigned i = 0; i < dispatches; ++i) for (unsigned q = 0; q < queues; ++q) {
        check(clSetKernelArg(kernel, 0, sizeof(buffers[q]), &buffers[q]), "buffer arg");
        check(clEnqueueNDRangeKernel(command_queues[q], kernel, 1, nullptr, &global, &local,
            0, nullptr, &events[q][i]), "enqueue");
    }
    for (unsigned q = 0; q < queues; ++q) check(clFlush(command_queues[q]), "flush");
    for (unsigned q = 0; q < queues; ++q) {
        check(clWaitForEvents(1, &events[q].back()), "wait");
        check(clEnqueueReadBuffer(command_queues[q], buffers[q], CL_TRUE, 0,
            global * sizeof(cl_uint), output.data(), 0, nullptr, nullptr), "readback");
        for (size_t i = 0; i < global; ++i) if (output[i] != transform(initial[i], iterations * dispatches)) {
            std::fprintf(stderr, "FAIL pixel q=%u i=%zu\n", q, i); return 1;
        }
        for (unsigned i = 0; i < dispatches; ++i) {
            cl_int state = 99; cl_ulong first = 0, last = 0;
            check(clGetEventInfo(events[q][i], CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(state), &state, nullptr), "event state");
            if (state != CL_COMPLETE) return 1;
            check(clGetEventProfilingInfo(events[q][i], CL_PROFILING_COMMAND_START, sizeof(first), &first, nullptr), "profile start");
            check(clGetEventProfilingInfo(events[q][i], CL_PROFILING_COMMAND_END, sizeof(last), &last, nullptr), "profile end");
            if (last < first) return 1;
            std::printf("EVENT q=%u index=%u event=%p start_ns=%llu end_ns=%llu duration_ns=%llu\n",
                q, i, (void*)events[q][i], first, last, last - first);
            check(clReleaseEvent(events[q][i]), "release event");
        }
        check(clReleaseMemObject(buffers[q]), "release buffer");
        check(clReleaseCommandQueue(command_queues[q]), "release queue");
    }
    check(clReleaseKernel(kernel), "release kernel"); check(clReleaseProgram(program), "release program");
    check(clReleaseContext(context), "release context");
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("PASS GPU dispatch control: checked=%zu wall_us=%lld; actual integer kernel/readback/events, not GB7 reproduction\n", queues * global, us);
}
