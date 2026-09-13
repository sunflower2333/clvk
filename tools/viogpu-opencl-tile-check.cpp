// SPDX-License-Identifier: Apache-2.0
// Candidate ICD semantic control. --any-device permits software Vulkan in CI.
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static unsigned checks;
static void require(bool yes, const char* what) {
    ++checks;
    if (!yes) { std::fprintf(stderr,"FAIL %s\n",what); std::exit(1); }
}
static void check(cl_int status, const char* what) {
    if (status != CL_SUCCESS) {
        std::fprintf(stderr,"FAIL %s status=%d\n",what,status); std::exit(1);
    }
    ++checks;
}
static void env(const char* name, const char* value) {
#ifdef _WIN32
    require(_putenv_s(name,value)==0,"environment");
#else
    require(setenv(name,value,1)==0,"environment");
#endif
}
// The executable and ICD use distinct static CRT environments on Windows.
// If --groups changes the inherited value, start a child whose DLLs see the
// correct value from process creation. Changing only this EXE's getenv state
// after DLL load is not a reliable way to configure the candidate ICD.
static int prepare_budget(unsigned budget) {
    const auto value=std::to_string(budget);
#ifdef _WIN32
    const auto* inherited=std::getenv("CLVK_MAX_DISPATCH_WORKGROUPS");
    if(inherited&&value==inherited) return -1;
    require(SetEnvironmentVariableA("CLVK_MAX_DISPATCH_WORKGROUPS",value.c_str())!=0,"set inherited tile budget");
    wchar_t executable[32768]{};
    const auto length=GetModuleFileNameW(nullptr,executable,32768);
    require(length>0&&length<32768,"executable path");
    const std::wstring original=GetCommandLineW();
    std::vector<wchar_t> command(original.begin(),original.end());command.push_back(0);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process)!=0,"start inherited-environment control");
    CloseHandle(process.hThread);
    const auto waited=WaitForSingleObject(process.hProcess,INFINITE);
    DWORD code=1;const auto queried=GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess);
    require(waited==WAIT_OBJECT_0&&queried,"wait inherited-environment control");
    return code==0 ? 0 : 1;
#else
    env("CLVK_MAX_DISPATCH_WORKGROUPS",value.c_str());
    return -1;
#endif
}
static void CL_CALLBACK callback(cl_event, cl_int status, void* data) {
    if (status <= CL_COMPLETE) static_cast<std::atomic<unsigned>*>(data)->fetch_add(1);
}
static const char* kernel_source = R"CLC(
__kernel void tile_semantics(__global uint* output, __global uint* histogram,
                             __local uint* scratch, uint salt) {
    size_t i=get_global_linear_id(), li=get_local_linear_id();
    size_t base=i*28;
    for(uint d=0;d<3;d++) {
        output[base+d*8+0]=(uint)get_global_id(d);
        output[base+d*8+1]=(uint)get_global_offset(d);
        output[base+d*8+2]=(uint)get_global_size(d);
        output[base+d*8+3]=(uint)get_group_id(d);
        output[base+d*8+4]=(uint)get_num_groups(d);
        output[base+d*8+5]=(uint)get_local_id(d);
        output[base+d*8+6]=(uint)get_local_size(d);
#ifdef UNIFORM_CONTROL
        // Matching clspv currently asserts on get_enqueued_local_size in
        // uniform-only mode. For this negative gate control the sizes coincide.
        output[base+d*8+7]=(uint)get_local_size(d);
#else
        output[base+d*8+7]=(uint)get_enqueued_local_size(d);
#endif
    }
    output[base+24]=(uint)i; output[base+25]=(uint)li;
    output[base+26]=get_work_dim(); output[base+27]=salt;
    scratch[li]=(uint)i+salt;
    barrier(CLK_LOCAL_MEM_FENCE);
    atomic_add(histogram+i%7,salt+1);
    if(li==0) {
        uint sum=0;
        size_t n=get_local_size(0)*get_local_size(1)*get_local_size(2);
        for(size_t j=0;j<n;j++) sum+=scratch[j];
        atomic_add(histogram+7,sum);
    }
}
__kernel void batch_order(__global uint* output, uint salt) {
    size_t i=get_global_linear_id();
    output[i]=output[i]*1664525u+1013904223u+salt+
              (uint)get_group_id(0)+(uint)get_num_groups(0);
}
)CLC";

static cl_program build(cl_context ctx, cl_device_id device, const char* options,
                        unsigned binary_import) {
    cl_int status;
    auto program=clCreateProgramWithSource(ctx,1,&kernel_source,nullptr,&status);
    check(status,"create source");
    status=clBuildProgram(program,1,&device,options,nullptr,nullptr);
    if(status!=CL_SUCCESS) {
        size_t n=0; clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG,0,nullptr,&n);
        std::vector<char> log(n+1); clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG,n,log.data(),nullptr);
        std::fprintf(stderr,"BUILD_LOG %s\n",log.data());
    }
    check(status,"source build");
    if(binary_import) {
        size_t size=0;
        check(clGetProgramInfo(program,CL_PROGRAM_BINARY_SIZES,sizeof(size),&size,nullptr),"binary size");
        std::vector<unsigned char> binary(size); auto* ptr=binary.data();
        check(clGetProgramInfo(program,CL_PROGRAM_BINARIES,sizeof(ptr),&ptr,nullptr),"binary bytes");
        const unsigned char* input=binary.data(); cl_int binary_status;
        if(binary_import==2) {
            require(size>=16,"CLVK binary header size");
            cl_uint header[4]{};std::memcpy(header,input,sizeof(header));
            require(header[0]==0x6b766c63&&header[1]==2&&header[3]<=size-16,"CLVK v2 executable header");
            input+=16+header[3];size-=16+header[3];
            require(size>=4,"raw SPIR-V payload");
        }
        auto imported=clCreateProgramWithBinary(ctx,1,&device,&size,&input,&binary_status,&status);
        check(status,"import executable"); check(binary_status,"executable status");
        check(clBuildProgram(imported,1,&device,nullptr,nullptr,nullptr),"build imported executable");
        check(clReleaseProgram(program),"release source executable"); program=imported;
    }
    return program;
}

static void run(cl_context ctx,cl_device_id device,cl_program program,
                unsigned dims,std::array<size_t,3> gws,std::array<size_t,3> lws,
                const char* label,bool blocked=true) {
    const std::array<size_t,3> offset{13,19,23};
    constexpr cl_uint guard=0xcafef00du,salt=3;
    const size_t n=gws[0]*gws[1]*gws[2],words=n*28;
    cl_int status;
    auto queue=clCreateCommandQueue(ctx,device,CL_QUEUE_PROFILING_ENABLE,&status);check(status,"queue");
    auto kernel=clCreateKernel(program,"tile_semantics",&status);check(status,"kernel");
    cl_uint align_bits=0;
    check(clGetDeviceInfo(device,CL_DEVICE_MEM_BASE_ADDR_ALIGN,sizeof(align_bits),&align_bits,nullptr),"alignment");
    const size_t align_bytes=std::max<size_t>(4,(align_bits+7)/8);
    const size_t prefix=((1024+align_bytes-1)/align_bytes)*align_bytes/sizeof(cl_uint);
    std::vector<cl_uint> output(prefix+words+prefix,guard),replacement(words,guard);
    auto parent=clCreateBuffer(ctx,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,output.size()*4,output.data(),&status);check(status,"parent buffer");
    cl_buffer_region region{prefix*4,words*4};
    auto sub=clCreateSubBuffer(parent,CL_MEM_READ_WRITE,CL_BUFFER_CREATE_TYPE_REGION,&region,&status);check(status,"subbuffer");
    auto other=clCreateBuffer(ctx,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,replacement.size()*4,replacement.data(),&status);check(status,"other buffer");
    std::array<cl_uint,8> histogram{},expected{};
    auto hist=clCreateBuffer(ctx,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,sizeof(histogram),histogram.data(),&status);check(status,"histogram");
    check(clSetKernelArg(kernel,0,sizeof(sub),&sub),"output arg");
    check(clSetKernelArg(kernel,1,sizeof(hist),&hist),"histogram arg");
    check(clSetKernelArg(kernel,2,lws[0]*lws[1]*lws[2]*4,nullptr),"local arg");
    check(clSetKernelArg(kernel,3,sizeof(salt),&salt),"salt arg");
    auto dependency=clCreateUserEvent(ctx,&status);check(status,"user event");
    cl_event event=nullptr;
    std::atomic<unsigned> callbacks{0};
    std::printf("CASE %s dims=%u gws=%zu,%zu,%zu lws=%zu,%zu,%zu\n",label,dims,gws[0],gws[1],gws[2],lws[0],lws[1],lws[2]);
    check(clEnqueueNDRangeKernel(queue,kernel,dims,offset.data(),gws.data(),lws.data(),blocked?1:0,blocked?&dependency:nullptr,&event),"enqueue tiled command");
    check(clSetEventCallback(event,CL_COMPLETE,callback,&callbacks),"event callback");
    check(clFlush(queue),"flush blocked command");
    cl_int state=99;
    check(clGetEventInfo(event,CL_EVENT_COMMAND_EXECUTION_STATUS,sizeof(state),&state,nullptr),"blocked status");
    if(blocked) require(state>CL_COMPLETE&&callbacks==0,"dependency prevents early completion");
    // Freeze both a scalar and a resource before later tile recording. Release
    // the application's subbuffer reference; the command must retain it.
    const cl_uint changed=99;
    check(clSetKernelArg(kernel,3,sizeof(changed),&changed),"mutate scalar");
    check(clSetKernelArg(kernel,0,sizeof(other),&other),"mutate buffer");
    check(clReleaseMemObject(sub),"release queued subbuffer");
    check(clSetUserEventStatus(dependency,CL_COMPLETE),"unblock");
    check(clWaitForEvents(1,&event),"wait all tiles");
    require(callbacks==1,"one final callback");
    check(clEnqueueReadBuffer(queue,parent,CL_TRUE,0,output.size()*4,output.data(),1,&event,nullptr),"dependent readback");
    check(clEnqueueReadBuffer(queue,hist,CL_TRUE,0,sizeof(histogram),histogram.data(),0,nullptr,nullptr),"histogram readback");
    for(size_t i=0;i<n;++i) {
        std::array<size_t,3> pos{i%gws[0],(i/gws[0])%gws[1],i/(gws[0]*gws[1])};
        std::array<size_t,3> actual_lws{};
        for(unsigned d=0;d<3;++d) {
            const size_t gid=pos[d]/lws[d],lid=pos[d]%lws[d];
            actual_lws[d]=std::min(lws[d],gws[d]-gid*lws[d]);
            const std::array<cl_uint,8> values{cl_uint(pos[d]+(d<dims?offset[d]:0)),cl_uint(d<dims?offset[d]:0),
                cl_uint(gws[d]),cl_uint(gid),cl_uint((gws[d]+lws[d]-1)/lws[d]),cl_uint(lid),
                cl_uint(actual_lws[d]),cl_uint(lws[d])};
            for(unsigned k=0;k<8;++k) require(output[prefix+i*28+d*8+k]==values[k],"builtin exact oracle");
        }
        size_t li=(pos[0]%lws[0])+actual_lws[0]*((pos[1]%lws[1])+actual_lws[1]*(pos[2]%lws[2]));
        require(output[prefix+i*28+24]==i,"global linear ID");
        require(output[prefix+i*28+25]==li,"local linear ID");
        require(output[prefix+i*28+26]==dims,"work dimension");
        require(output[prefix+i*28+27]==salt,"enqueued scalar snapshot");
        expected[i%7]+=salt+1; expected[7]+=cl_uint(i)+salt;
    }
    require(histogram==expected,"shared atomic histogram and local barrier sum");
    for(size_t i=0;i<prefix;++i) require(output[i]==guard&&output[prefix+words+i]==guard,"subbuffer canaries");
    cl_ulong times[4]{};
    const cl_profiling_info properties[]={CL_PROFILING_COMMAND_QUEUED,CL_PROFILING_COMMAND_SUBMIT,CL_PROFILING_COMMAND_START,CL_PROFILING_COMMAND_END};
    for(unsigned i=0;i<4;++i) check(clGetEventProfilingInfo(event,properties[i],sizeof(times[i]),&times[i],nullptr),"profiling");
    require(times[0]<=times[1]&&times[1]<=times[2]&&times[2]<=times[3],"one monotonic profiling interval");
    std::printf("EVENT %s event=%p queued=%llu submit=%llu start=%llu end=%llu\n",label,(void*)event,
        (unsigned long long)times[0],(unsigned long long)times[1],(unsigned long long)times[2],(unsigned long long)times[3]);
    check(clEnqueueReadBuffer(queue,other,CL_TRUE,0,replacement.size()*4,replacement.data(),0,nullptr,nullptr),"untouched replacement");
    for(auto word:replacement) require(word==guard,"mutated argument leaves replacement untouched");
    // Failed dependency must execute no work, produce one negative callback and
    // never wait for an unwritten profiling END timestamp.
    auto failed=clCreateUserEvent(ctx,&status);check(status,"failed dependency");
    cl_event rejected=nullptr;std::atomic<unsigned> rejected_callbacks{0};
    check(clEnqueueNDRangeKernel(queue,kernel,dims,offset.data(),gws.data(),lws.data(),1,&failed,&rejected),"enqueue rejected command");
    check(clSetEventCallback(rejected,CL_COMPLETE,callback,&rejected_callbacks),"negative callback");
    check(clSetUserEventStatus(failed,-1),"reject dependency");
    require(clWaitForEvents(1,&rejected)==CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST,"negative wait");
    require(rejected_callbacks==1,"one negative callback");
    cl_ulong unavailable=0;
    require(clGetEventProfilingInfo(rejected,CL_PROFILING_COMMAND_END,sizeof(unavailable),&unavailable,nullptr)==CL_PROFILING_INFO_NOT_AVAILABLE,"failed profiling unavailable");
    check(clReleaseEvent(rejected),"release rejected");check(clReleaseEvent(failed),"release failed dependency");
    check(clReleaseEvent(event),"release event");check(clReleaseEvent(dependency),"release dependency");
    check(clReleaseMemObject(hist),"release histogram");check(clReleaseMemObject(other),"release replacement");
    check(clReleaseMemObject(parent),"release parent");check(clReleaseKernel(kernel),"release kernel");
    check(clReleaseCommandQueue(queue),"release queue");
    std::printf("CASE_PASS %s pixels=%zu\n",label,n);
}

static void empty_range(cl_context ctx,cl_device_id device,cl_program program) {
    cl_int status;
    auto queue=clCreateCommandQueue(ctx,device,0,&status);check(status,"empty queue");
    auto kernel=clCreateKernel(program,"tile_semantics",&status);check(status,"empty kernel");
    std::array<cl_uint,8> words{};words.fill(0x12345678u);
    auto buffer=clCreateBuffer(ctx,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,sizeof(words),words.data(),&status);check(status,"empty buffer");
    check(clSetKernelArg(kernel,0,sizeof(buffer),&buffer),"empty output");
    check(clSetKernelArg(kernel,1,sizeof(buffer),&buffer),"empty histogram");
    check(clSetKernelArg(kernel,2,16*4,nullptr),"empty local");
    cl_uint salt=3;check(clSetKernelArg(kernel,3,sizeof(salt),&salt),"empty scalar");
    const size_t gws=0,lws=16;cl_event event=nullptr;
    check(clEnqueueNDRangeKernel(queue,kernel,1,nullptr,&gws,&lws,0,nullptr,&event),"empty enqueue");
    check(clWaitForEvents(1,&event),"empty complete");
    check(clEnqueueReadBuffer(queue,buffer,CL_TRUE,0,sizeof(words),words.data(),0,nullptr,nullptr),"empty readback");
    for(auto word:words) require(word==0x12345678u,"empty NDRange does no work");
    check(clReleaseEvent(event),"release empty event");check(clReleaseMemObject(buffer),"release empty buffer");
    check(clReleaseKernel(kernel),"release empty kernel");check(clReleaseCommandQueue(queue),"release empty queue");
    std::puts("EMPTY_PASS zero global size keeps prior no-work path");
}

// Same arguments and geometry train the ordinary duration key once, then
// enqueue twelve ready commands before a flush. The recurrence requires each
// command's compute memory barrier and the original scalar snapshot to hold.
// Submission traces establish actual shared buffers, not just absence of tiles.
static void run_batch(cl_context ctx,cl_device_id device,cl_program program,
                      size_t gws,const char* label) {
    constexpr unsigned repeats=12;
    constexpr cl_uint guard=0xcafef00du,salt=3;
    const size_t lws=16,offset=13,groups=(gws+lws-1)/lws;
    cl_int status;
    auto queue=clCreateCommandQueue(ctx,device,CL_QUEUE_PROFILING_ENABLE,&status);check(status,"batch queue");
    auto kernel=clCreateKernel(program,"batch_order",&status);check(status,"batch kernel");
    std::vector<cl_uint> words(gws+8,guard);
    auto buffer=clCreateBuffer(ctx,CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,words.size()*4,words.data(),&status);check(status,"batch buffer");
    check(clSetKernelArg(kernel,0,sizeof(buffer),&buffer),"batch buffer arg");
    check(clSetKernelArg(kernel,1,sizeof(salt),&salt),"batch scalar arg");
    std::printf("BATCH_CASE %s groups=%zu gws=%zu lws=%zu repeats=%u\n",label,groups,gws,lws,repeats);
    cl_event warmup=nullptr;
    check(clEnqueueNDRangeKernel(queue,kernel,1,&offset,&gws,&lws,0,nullptr,&warmup),"batch warmup enqueue");
    check(clWaitForEvents(1,&warmup),"batch warmup wait");
    check(clFinish(queue),"batch warmup duration sample");
    std::array<cl_event,repeats> events{};
    std::array<std::atomic<unsigned>,repeats> callbacks{};
    for(unsigned i=0;i<repeats;++i) {
        callbacks[i].store(0);
        check(clEnqueueNDRangeKernel(queue,kernel,1,&offset,&gws,&lws,0,nullptr,&events[i]),"batch ready enqueue");
        check(clSetEventCallback(events[i],CL_COMPLETE,callback,&callbacks[i]),"batch callback");
    }
    const cl_uint changed=99;
    check(clSetKernelArg(kernel,1,sizeof(changed),&changed),"batch mutate scalar");
    check(clReleaseKernel(kernel),"batch release retained kernel");
    check(clFlush(queue),"batch flush ready commands");
    check(clWaitForEvents(repeats,events.data()),"batch wait ready events");
    check(clFinish(queue),"batch complete");
    check(clEnqueueReadBuffer(queue,buffer,CL_TRUE,0,words.size()*4,words.data(),0,nullptr,nullptr),"batch ordered readback");
    for(size_t i=0;i<gws;++i) {
        cl_uint expected=guard;
        for(unsigned iteration=0;iteration<=repeats;++iteration)
            expected=expected*1664525u+1013904223u+salt+cl_uint(i/lws)+cl_uint(groups);
        require(words[i]==expected,"batch recurrence/barrier/snapshot oracle");
    }
    for(size_t i=gws;i<words.size();++i) require(words[i]==guard,"batch tail canary");
    cl_ulong previous_end=0;
    for(unsigned i=0;i<repeats;++i) {
        require(callbacks[i]==1,"batch one callback per public event");
        cl_int state=99;
        check(clGetEventInfo(events[i],CL_EVENT_COMMAND_EXECUTION_STATUS,sizeof(state),&state,nullptr),"batch event status");
        require(state==CL_COMPLETE,"batch event complete");
        cl_ulong times[4]{};
        const cl_profiling_info properties[]={CL_PROFILING_COMMAND_QUEUED,CL_PROFILING_COMMAND_SUBMIT,CL_PROFILING_COMMAND_START,CL_PROFILING_COMMAND_END};
        for(unsigned t=0;t<4;++t) check(clGetEventProfilingInfo(events[i],properties[t],sizeof(times[t]),&times[t],nullptr),"batch profiling");
        require(times[0]<=times[1]&&times[1]<=times[2]&&times[2]<=times[3],"batch monotonic public interval");
        require(previous_end<=times[2],"batch device intervals preserve queue order");
        previous_end=times[3];
        std::printf("BATCH_EVENT %s ordinal=%u event=%p start=%llu end=%llu\n",label,i+1,(void*)events[i],(unsigned long long)times[2],(unsigned long long)times[3]);
        check(clReleaseEvent(events[i]),"batch release event");
    }
    check(clReleaseEvent(warmup),"batch warmup release");
    check(clReleaseMemObject(buffer),"batch release buffer");check(clReleaseCommandQueue(queue),"batch release queue");
    std::printf("BATCH_PASS %s groups=%zu commands=%u\n",label,groups,repeats+1);
}

int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    unsigned budget=7;bool any=false,environment_only=false;
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--help")) {
            std::puts("Candidate tiled semantics: --groups 0..65535 (default7), --any-device (explicit CI software allowance), --check-environment (no GPU; inherited process setting). Builtin IDs/sizes/offsets, nonuniform tails, barrier+atomics, subbuffer canaries, argument lifetime, events/profiling and imported uniform binary control. No benchmark claim.");return 0;
        }
        if(!std::strcmp(argv[i],"--any-device")) any=true;
        else if(!std::strcmp(argv[i],"--check-environment")) environment_only=true;
        else if(!std::strcmp(argv[i],"--groups")&&i+1<argc) {
            char* end=nullptr;auto n=std::strtoul(argv[++i],&end,10);
            if(!*argv[i]||*end||n>65535)return 2;
            budget=unsigned(n);
        } else return 2;
    }
    const int child=prepare_budget(budget);
    if(child>=0) return child;
    if(environment_only) {
        require(std::getenv("CLVK_MAX_DISPATCH_WORKGROUPS")&&
                std::to_string(budget)==std::getenv("CLVK_MAX_DISPATCH_WORKGROUPS"),"effective environment budget");
        std::printf("PASS inherited tile environment groups=%u bits=%zu; no GPU calls\n",budget,sizeof(void*)*8);
        return 0;
    }
    cl_uint count=0;check(clGetPlatformIDs(0,nullptr,&count),"platform count");
    std::vector<cl_platform_id> platforms(count);check(clGetPlatformIDs(count,platforms.data(),nullptr),"platforms");
    cl_device_id device=nullptr;
    for(auto platform:platforms) {
        cl_uint n=0;auto result=clGetDeviceIDs(platform,CL_DEVICE_TYPE_ALL,0,nullptr,&n);
        if(result==CL_DEVICE_NOT_FOUND)continue;
        check(result,"device count");
        std::vector<cl_device_id> devices(n);check(clGetDeviceIDs(platform,CL_DEVICE_TYPE_ALL,n,devices.data(),nullptr),"devices");
        for(auto candidate:devices) {
            char name[512]{};check(clGetDeviceInfo(candidate,CL_DEVICE_NAME,sizeof(name),name,nullptr),"device name");
            if(any||std::strstr(name,"Adreno")||std::strstr(name,"Turnip")) {
                device=candidate;std::printf("DEVICE %s groups=%u bits=%zu any=%u\n",name,budget,sizeof(void*)*8,unsigned(any));break;
            }
        }
        if(device)break;
    }
    require(device!=nullptr,"expected GPU device");
    cl_int status;auto ctx=clCreateContext(nullptr,1,&device,nullptr,nullptr,&status);check(status,"context");
    auto source=build(ctx,device,"-cl-std=CL3.0",false);
    empty_range(ctx,device,source);
    run_batch(ctx,device,source,67,"within-tail");
    run_batch(ctx,device,source,97,"exact-tail");
    run_batch(ctx,device,source,113,"crossing-tail");
    run(ctx,device,source,1,{67,1,1},{16,1,1},"source-1d-tail");
    run(ctx,device,source,2,{17,7,1},{4,2,1},"source-2d-tails");
    run(ctx,device,source,3,{9,7,5},{4,2,2},"source-3d-tails");
    run(ctx,device,source,3,{9,7,5},{4,2,2},"source-3d-ready",false);
    check(clReleaseProgram(source),"release source");
    auto uniform=build(ctx,device,"-cl-std=CL3.0 -cl-uniform-work-group-size -DUNIFORM_CONTROL",false);
    run(ctx,device,uniform,3,{8,6,4},{4,2,2},"uniform-source-bypass");
    check(clReleaseProgram(uniform),"release uniform source");
    auto imported=build(ctx,device,"-cl-std=CL3.0 -cl-uniform-work-group-size -DUNIFORM_CONTROL",true);
    run(ctx,device,imported,3,{8,6,4},{4,2,2},"uniform-binary-bypass");
    check(clReleaseProgram(imported),"release imported");
    auto raw=build(ctx,device,"-cl-std=CL3.0 -cl-uniform-work-group-size -DUNIFORM_CONTROL",2);
    run(ctx,device,raw,3,{8,6,4},{4,2,2},"uniform-raw-bypass");
    check(clReleaseProgram(raw),"release raw");check(clReleaseContext(ctx),"release context");
    std::printf("PASS %u actual compiled-kernel builtin/barrier/atomic/lifetime/event/profiling checks; device identity above\n",checks);
}
