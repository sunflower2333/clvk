// SPDX-License-Identifier: Apache-2.0
// Bounded GPU execution proof. Enumeration or a CPU OpenCL device is not a pass.
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

static void check(cl_int status, const char* operation) {
    if (status != CL_SUCCESS) {
        std::fprintf(stderr, "FAIL %s status=%d\n", operation, status);
        std::exit(1);
    }
}

static cl_program build(cl_context context, cl_device_id device,
                        const char* source, bool expect_failure = false) {
    cl_int status;
    cl_program program = clCreateProgramWithSource(context, 1, &source, nullptr, &status);
    check(status, "clCreateProgramWithSource");
    auto started = std::chrono::steady_clock::now();
    status = clBuildProgram(program, 1, &device, "-cl-std=CL1.2", nullptr, nullptr);
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("compiler_wall_us=%lld expected_failure=%u\n", duration, unsigned(expect_failure));
    size_t length = 0;
    clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &length);
    std::vector<char> log(length + 1);
    clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, length, log.data(), nullptr);
    std::printf("build_status=%d log=%s\n", status, log.data());
    if (expect_failure) {
        if (status != CL_BUILD_PROGRAM_FAILURE) {
            std::fprintf(stderr, "FAIL invalid source was not rejected\n");
            std::exit(1);
        }
        cl_build_status build_status;
        check(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_STATUS,
                                    sizeof(build_status), &build_status, nullptr), "build status");
        if (build_status != CL_BUILD_ERROR) std::exit(1);
    } else {
        check(status, "clBuildProgram");
    }
    return program;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("pointer_bits=%zu pid=%lu\n", sizeof(void*) * 8, GetCurrentProcessId());
    const char* modules[] = {"OpenCL.dll", "vulkan-1.dll"};
    for (const char* name : modules) {
        char path[MAX_PATH] = {};
        HMODULE module = GetModuleHandleA(name);
        if (!module || !GetModuleFileNameA(module, path, MAX_PATH)) return 1;
        std::printf("module %s=%s\n", name, path);
    }
    HMODULE opencl = GetModuleHandleA("OpenCL.dll");
    if (!GetProcAddress(opencl, "clGetPlatformIDs") ||
        !GetProcAddress(opencl, "clIcdGetPlatformIDsKHR")) {
        std::fprintf(stderr, "FAIL missing undecorated ICD exports\n");
        return 1;
    }
    cl_uint count;
    check(clGetPlatformIDs(0, nullptr, &count), "platform count");
    std::vector<cl_platform_id> platforms(count);
    check(clGetPlatformIDs(count, platforms.data(), nullptr), "platforms");
    cl_device_id device = nullptr;
    for (cl_platform_id platform : platforms) {
        cl_uint devices = 0;
        cl_int status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &devices);
        if (status == CL_DEVICE_NOT_FOUND) continue;
        check(status, "GPU device count");
        std::vector<cl_device_id> candidates(devices);
        check(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, devices, candidates.data(), nullptr), "GPU devices");
        for (auto candidate : candidates) {
            char name[512] = {}, vendor[512] = {}, version[512] = {};
            check(clGetDeviceInfo(candidate, CL_DEVICE_NAME, sizeof(name), name, nullptr), "device name");
            check(clGetDeviceInfo(candidate, CL_DEVICE_VENDOR, sizeof(vendor), vendor, nullptr), "vendor");
            check(clGetDeviceInfo(candidate, CL_DRIVER_VERSION, sizeof(version), version, nullptr), "driver version");
            std::printf("GPU name=%s vendor=%s driver=%s\n", name, vendor, version);
            if (std::strstr(name, "Adreno") || std::strstr(name, "Turnip") || std::strstr(name, "FD650")) device = candidate;
        }
    }
    if (!device) {
        std::fprintf(stderr, "FAIL no Adreno/Turnip GPU selected\n");
        return 1;
    }
    cl_int status;
    cl_context context = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &status);
    check(status, "context");
    cl_program invalid = build(context, device, "__kernel void invalid( {", true);
    check(clReleaseProgram(invalid), "release invalid program");
    cl_program program = build(context, device,
        "__kernel void transform(__global const uint *a, __global uint *b, uint salt) {"
        "size_t i=get_global_id(0); b[i]=(a[i]*1664525u+1013904223u)^salt; }");
    cl_kernel kernel = clCreateKernel(program, "transform", &status);
    check(status, "kernel");
    constexpr size_t n = 4096, bytes = n * sizeof(cl_uint);
    std::vector<cl_uint> input(n), output(n);
    for (size_t i = 0; i < n; ++i) input[i] = cl_uint(i * 17 + 11);
    cl_mem a = clCreateBuffer(context, CL_MEM_READ_WRITE, bytes, nullptr, &status);
    check(status, "buffer a");
    cl_mem b = clCreateBuffer(context, CL_MEM_READ_WRITE, bytes, nullptr, &status);
    check(status, "buffer b");
    cl_mem c = clCreateBuffer(context, CL_MEM_READ_WRITE, bytes, nullptr, &status);
    check(status, "buffer c");
    cl_command_queue_properties supported;
    check(clGetDeviceInfo(device, CL_DEVICE_QUEUE_PROPERTIES, sizeof(supported), &supported, nullptr), "queue properties");
    for (unsigned mode = 0; mode < 2; ++mode) {
        if (mode && !(supported & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE)) {
            std::printf("out_of_order=unsupported (not claimed)\n");
            continue;
        }
        cl_command_queue_properties properties = CL_QUEUE_PROFILING_ENABLE;
        if (mode) properties |= CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE;
        cl_command_queue queue = clCreateCommandQueue(context, device, properties, &status);
        check(status, "queue");
        for (cl_uint iteration = 0; iteration < 8; ++iteration) {
            cl_uint salt = 0x91234567u + iteration;
            cl_event write, execute, copy, read;
            check(clEnqueueWriteBuffer(queue, a, CL_FALSE, 0, bytes, input.data(), 0, nullptr, &write), "write");
            check(clSetKernelArg(kernel, 0, sizeof(a), &a), "arg a");
            check(clSetKernelArg(kernel, 1, sizeof(b), &b), "arg b");
            check(clSetKernelArg(kernel, 2, sizeof(salt), &salt), "arg salt");
            size_t local = 64;
            check(clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &n, &local, 1, &write, &execute), "kernel enqueue");
            check(clEnqueueCopyBuffer(queue, b, c, 0, 0, bytes, 1, &execute, &copy), "copy");
            check(clEnqueueReadBuffer(queue, c, CL_FALSE, 0, bytes, output.data(), 1, &copy, &read), "read");
            check(clFlush(queue), "flush");
            check(clWaitForEvents(1, &read), "wait");
            cl_ulong start, end;
            check(clGetEventProfilingInfo(execute, CL_PROFILING_COMMAND_START, sizeof(start), &start, nullptr), "kernel start");
            check(clGetEventProfilingInfo(execute, CL_PROFILING_COMMAND_END, sizeof(end), &end, nullptr), "kernel end");
            if (end < start) return 1;
            for (size_t i = 0; i < n; ++i) {
                cl_uint expected = (input[i] * 1664525u + 1013904223u) ^ salt;
                if (output[i] != expected) {
                    std::fprintf(stderr, "FAIL mode=%u iteration=%u index=%zu expected=%u actual=%u\n", mode, iteration, i, expected, output[i]);
                    return 1;
                }
            }
            cl_event events[] = {write, execute, copy, read};
            for (auto event : events) {
                cl_int execution;
                check(clGetEventInfo(event, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(execution), &execution, nullptr), "event status");
                if (execution != CL_COMPLETE) return 1;
                check(clReleaseEvent(event), "event release");
            }
            // OpenCL profiling may use host monotonic time when the Vulkan
            // driver lacks calibrated timestamps; this is not GPU-only time.
            std::printf("verified mode=%u iteration=%u elements=%zu event_ns=%llu\n", mode, iteration, n, end - start);
        }
        check(clFinish(queue), "finish");
        check(clReleaseCommandQueue(queue), "release queue");
    }
    check(clReleaseMemObject(c), "release c");
    check(clReleaseMemObject(b), "release b");
    check(clReleaseMemObject(a), "release a");
    check(clReleaseKernel(kernel), "release kernel");
    check(clReleaseProgram(program), "release program");
    check(clReleaseContext(context), "release context");
    std::printf("PASS GPU kernel + copy + readback + events + compiler error propagation\n");
    return 0;
}
