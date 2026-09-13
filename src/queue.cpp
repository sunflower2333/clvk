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

#include <chrono>
#include <thread>
#include <unordered_set>

#include "config.hpp"
#include "init.hpp"
#include "memory.hpp"
#include "queue.hpp"
#include "queue_controller.hpp"
#include "tracing.hpp"
#include "utils.hpp"

static cvk_executor_thread_pool* get_thread_pool() {
    auto state = get_or_init_global_state();
    return state->thread_pool();
}

cvk_command_queue::cvk_command_queue(
    cvk_context* ctx, cvk_device* device,
    cl_command_queue_properties properties,
    std::vector<cl_queue_properties>&& properties_array)
    : api_object(ctx), m_device(device), m_properties(properties),
      m_properties_array(std::move(properties_array)), m_executor(nullptr),
      m_vulkan_queue(device->vulkan_queue_allocate()),
      m_command_pool(device, m_vulkan_queue.queue_family()),
      m_max_cmd_batch_size(device->get_max_cmd_batch_size()),
      m_max_first_cmd_batch_size(device->get_max_first_cmd_batch_size()),
      m_max_cmd_group_size(device->get_max_cmd_group_size()),
      m_max_first_cmd_group_size(device->get_max_first_cmd_group_size()),
      m_nb_batch_in_flight(0), m_nb_group_in_flight(0) {

    m_groups.push_back(std::make_unique<cvk_command_group>());

    if (properties & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE) {
        cvk_warn_fn("out-of-order execution enabled, will be ignored");
    }

    if (config.dynamic_batches) {
        m_controllers.push_back(
            std::make_unique<cvk_queue_controller_batch_parameters>(this));
    }

    TRACE_CNT_VAR_INIT(batch_in_flight_counter, "batches", this->track());
    TRACE_CNT_VAR_INIT(group_in_flight_counter, "groups", this->track());

    TRACE_CNT(batch_in_flight_counter, 0);
    TRACE_CNT(group_in_flight_counter, 0);
}

cl_int cvk_command_queue::init() {

    if (m_command_pool.init() != VK_SUCCESS) {
        return CL_OUT_OF_RESOURCES;
    }

    return CL_SUCCESS;
}

cvk_command_queue::~cvk_command_queue() {
    if (m_executor != nullptr) {
        get_thread_pool()->return_executor(m_executor);
    }
}

void cvk_command_queue::detach_from_context() { m_context.reset(nullptr); }

cl_int cvk_command_queue::satisfy_data_dependencies(cvk_command* cmd) {
    if (cmd->is_data_movement()) {
        return CL_SUCCESS;
    }
    for (auto mem : cmd->memory_objects()) {
        // Perform memory object initialisation
        auto& tracker = mem->init_tracker();
        std::lock_guard<std::mutex> lock(tracker.mutex());
        auto state = tracker.state();
        if (state == cvk_mem_init_state::completed) {
            continue;
        }
        if (state == cvk_mem_init_state::scheduled) {
            if (tracker.event()->completed()) {
                tracker.set_state(cvk_mem_init_state::completed);
            } else {
                cmd->add_dependency(tracker.event());
            }
            continue;
        }
        CVK_ASSERT(mem->is_image_type());
        auto initcmd =
            new cvk_command_image_init(this, static_cast<cvk_image*>(mem));
        _cl_event* initev;
        cl_int err = enqueue_command_with_retry(initcmd, &initev);
        if (err != CL_SUCCESS) {
            return err;
        }
        auto downcastev = icd_downcast(initev);
        tracker.set_event(downcastev);

        // The event has been retained by `enqueue_command` to give its user
        // a refcount on the event. The tracker will request a refcount so we
        // need to give up the one we got from `enqueue_command`.
        downcastev->release();
    }

    return CL_SUCCESS;
}

void cvk_command_queue::enqueue_command(cvk_command* cmd) {
    TRACE_FUNCTION("queue", (uintptr_t)this, "cmd", (uintptr_t)cmd);
    // clvk only supports inorder queues at the moment.
    // But as the commands can be executed by 2 threads (1 executor and the main
    // thread), we need to explicit the dependency to ensure it will be
    // respected.
    if (!m_groups.back()->commands.empty()) {
        cmd->add_dependency(m_groups.back()->commands.back()->event());
    } else if (m_finish_event != nullptr) {
        cmd->add_dependency(m_finish_event);
    }
    m_groups.back()->commands.push_back(cmd);
}

cl_int cvk_command_queue::enqueue_command_with_retry(cvk_command* cmd,
                                                     _cl_event** event) {
    cl_int err = enqueue_command(cmd, event);
    if (config.enqueue_command_retry_sleep_us == UINT32_MAX ||
        err != CL_OUT_OF_RESOURCES) {
        if (err != CL_SUCCESS) {
            delete cmd;
        }
        return err;
    }
    if (m_nb_group_in_flight == 0) {
        std::lock_guard<std::mutex> lock(m_lock);
        err = end_current_command_batch();
        if (err != CL_SUCCESS) {
            delete cmd;
            return err;
        }
        err = flush_no_lock();
        if (err != CL_SUCCESS) {
            delete cmd;
            return err;
        }
    }
    // Retry every 'config.descriptor_set_allocate_retry_sleep_us' us until
    // we have no more batch in flight, which would mean that all the
    // descriptor should have been freed, thus the error is not about not
    // having enough descriptors.
    do {
        TRACE_BEGIN("descriptor_sets_allocate_retry_sleep");
        std::this_thread::sleep_for(
            std::chrono::microseconds(config.enqueue_command_retry_sleep_us));
        TRACE_END();
        err = enqueue_command(cmd, event);
    } while (err == CL_OUT_OF_RESOURCES && m_nb_group_in_flight != 0);
    if (err != CL_SUCCESS) {
        delete cmd;
    }
    return err;
}

cl_int cvk_command_queue::enqueue_command(cvk_command* cmd, _cl_event** event) {

    cl_int err;

    // Enqueue data movement/consistency commands if needed
    err = satisfy_data_dependencies(cmd);
    if (err != CL_SUCCESS) {
        return err;
    }

    // Enqueue the command
    std::lock_guard<std::mutex> lock(m_lock);
    if (cmd->can_be_batched()) {
        auto* batchable = static_cast<cvk_command_batchable*>(cmd);
        if (config.max_batch_duration_us) {
            batchable->prepare_duration_estimate();
            if (m_command_batch &&
                !m_command_batch->accepts_cost(batchable->admission_cost())) {
                if ((err = end_current_command_batch()) != CL_SUCCESS) return err;
            }
        }
        if (!m_command_batch) {
            // Create a new command batch
            m_command_batch = std::make_unique<cvk_command_batch>(this);
        }

        // Add command to current batch
        err = m_command_batch->add_command(
            static_cast<cvk_command_batchable*>(cmd));
        if (err != CL_SUCCESS) {
            return err;
        }

        // End command batch when size limit reached
        if (m_command_batch->duration_limit_reached() ||
            m_command_batch->batch_size() >= m_max_cmd_batch_size ||
            (m_nb_batch_in_flight == 0 &&
             m_command_batch->batch_size() >= m_max_first_cmd_batch_size)) {
            if ((err = end_current_command_batch()) != CL_SUCCESS) {
                return err;
            }
        }
    } else {
        // End the current command batch
        if ((err = end_current_command_batch(true)) != CL_SUCCESS) {
            return err;
        }

        if (!cmd->is_built_before_enqueue()) {
            if (config.max_batch_duration_us)
                static_cast<cvk_command_batchable*>(cmd)->prepare_duration_estimate();
            // Build batchable command as non-batched (in its own command
            // buffer)
            err = static_cast<cvk_command_batchable*>(cmd)->build();
            if (err != CL_SUCCESS) {
                return err;
            }
        }

        enqueue_command(cmd);
    }

    cvk_debug_fn("enqueued command %p (%s), event %p", cmd,
                 cl_command_type_to_string(cmd->type()), cmd->event());

    cmd->event()->set_profiling_info_from_monotonic_clock(
        CL_PROFILING_COMMAND_QUEUED);

    if (event != nullptr) {
        // The event will be returned to the app, retain it for the user
        cmd->event()->retain();
        *event = cmd->event();
        cvk_debug_fn("returning event %p", *event);
    }

#ifdef CLVK_UNIT_TESTING_ENABLED
    if (!config.early_flush_enabled) {
        return CL_SUCCESS;
    }
#endif

    auto group_size = m_groups.back()->commands.size();
    if (group_size >= m_max_cmd_group_size ||
        (m_nb_group_in_flight == 0 &&
         group_size >= m_max_first_cmd_group_size)) {
        return flush_no_lock();
    }

    return CL_SUCCESS;
}

cl_int cvk_command_queue::enqueue_command_with_deps(
    cvk_command* cmd, cl_uint num_dep_events, _cl_event* const* dep_events,
    _cl_event** event) {
    cmd->set_dependencies(num_dep_events, dep_events);
    return enqueue_command_with_retry(cmd, event);
}

cl_int cvk_command_queue::enqueue_command_with_deps(
    cvk_command* cmd, bool blocking, cl_uint num_dep_events,
    _cl_event* const* dep_events, _cl_event** event) {
    cmd->set_dependencies(num_dep_events, dep_events);

    _cl_event* evt;
    cl_int err = enqueue_command_with_retry(cmd, &evt);
    if (err != CL_SUCCESS) {
        return err;
    }

    if (blocking) {
        err = wait_for_events(1, &evt);
    }

    if (event != nullptr) {
        *event = evt;
    } else {
        icd_downcast(evt)->release();
    }

    return err;
}

cl_int cvk_command_queue::end_current_command_batch(bool from_flush) {
    std::unique_ptr<cvk_command_batch> cmd_batch = std::move(m_command_batch);
    if (cmd_batch == nullptr || cmd_batch->batch_size() == 0) {
        return CL_SUCCESS;
    }

    TRACE_FUNCTION("queue", (uintptr_t)this, "batch_size",
                   cmd_batch->batch_size());

    if (!cmd_batch->end()) {
        return CL_OUT_OF_RESOURCES;
    }
    enqueue_command(cmd_batch.release());

    for (auto& controller : m_controllers) {
        controller->update_after_end_current_command_batch(from_flush);
    }

    batch_enqueued();
    return CL_SUCCESS;
}

cl_int cvk_command_queue::wait_for_events(cl_uint num_events,
                                          const cl_event* event_list) {
    cl_int ret = CL_SUCCESS;

    // Create set of queues to flush
    std::unordered_set<cvk_command_queue*> queues_to_flush;
    for (cl_uint i = 0; i < num_events; i++) {
        cvk_event* event = icd_downcast(event_list[i]);

        if (!event->is_user_event()) {
            queues_to_flush.insert(event->queue());
        }
    }

    // Flush queues
    for (auto q : queues_to_flush) {
        cl_int qerr = q->flush();
        if (qerr != CL_SUCCESS) {
            return qerr;
        }
    }

    if (queues_to_flush.size() == 1) {
        for (auto q : queues_to_flush) {
            auto status = q->execute_cmds_required_by(num_events, event_list);
            if (status != CL_SUCCESS)
                return status;
        }
    }

    // Now wait for all the events
    for (cl_uint i = 0; i < num_events; i++) {
        cvk_event* event = icd_downcast(event_list[i]);
        if (event->wait() != CL_COMPLETE) {
            ret = CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST;
        }
    }

    return ret;
}

void cvk_command_group::execute_cmds_in_executor() {
    CVK_ASSERT(commands.size() > 0);
    cvk_command_queue_holder queue = commands.front()->queue();
    execute_cmds();
    queue->group_completed();
}

cl_int cvk_command_group::execute_cmds() {
    TRACE_FUNCTION();
    cl_int global_status = CL_SUCCESS;
    while (!commands.empty()) {
        cvk_command* cmd = commands.front();
        cvk_debug_fn("executing command %p (%s), event %p", cmd,
                     cl_command_type_to_string(cmd->type()), cmd->event());

        cl_int status = cmd->execute();
        if (status != CL_COMPLETE && global_status == CL_SUCCESS)
            global_status = status;
        cvk_debug_fn("command returned %d", status);

        commands.pop_front();

        // Deleting batch with many commands can take a while. Trace it to be
        // able to understand it easily.
        TRACE_BEGIN("delete_cmd");
        delete cmd;
        TRACE_END();
    }
    return global_status;
}

cl_int cvk_command_queue::execute_cmds_required_by_no_lock(
    cl_uint num_events, _cl_event* const* event_list,
    std::unique_lock<std::mutex>& lock) {
    auto* exec = m_executor;
    if (exec == nullptr) {
        return CL_SUCCESS;
    }

    lock.unlock();
    auto cmds = exec->extract_cmds_required_by(false, num_events, event_list);
    auto ret = cmds.execute_cmds();
    lock.lock();

    return ret;
}

cl_int
cvk_command_queue::execute_cmds_required_by(cl_uint num_events,
                                            _cl_event* const* event_list) {
    std::unique_lock<std::mutex> lock(m_lock);
    return execute_cmds_required_by_no_lock(num_events, event_list, lock);
}

cvk_command_group
cvk_executor_thread::extract_cmds_required_by(bool only_non_batch_cmds,
                                              cl_uint num_events,
                                              _cl_event* const* event_list) {
    std::lock_guard<std::mutex> lock(m_lock);
    cvk_command_group output;
    std::deque<cvk_command*>& output_cmds = output.commands;
    if (m_groups.empty()) {
        return output;
    }

    cvk_command_queue_holder queue = m_groups.back()->commands.front()->queue();
    TRACE_FUNCTION("queue", (uintptr_t) & (*queue));

    std::unique_ptr<cvk_command_group> executor_cmds =
        std::make_unique<cvk_command_group>();
    bool dominated = false;
    while (!m_groups.empty()) {
        auto group = std::move(m_groups.back());
        m_groups.pop_back();
        queue->group_completed();
        while (!group->commands.empty()) {
            auto cmd = group->commands.back();
            group->commands.pop_back();
            if (!dominated) {
                for (unsigned each_event = 0; each_event < num_events;
                     each_event++) {
                    if (cmd->event() == icd_downcast(event_list[each_event])) {
                        dominated = true;
                        break;
                    }
                }
            }
            if (!dominated ||
                (cmd->type() == CLVK_COMMAND_BATCH && only_non_batch_cmds)) {
                executor_cmds->commands.push_front(cmd);
            } else {
                output_cmds.push_front(cmd);
            }
        }
    }
    if (executor_cmds->commands.size() > 0) {
        m_groups.push_back(std::move(executor_cmds));
        queue->group_sent();
    }
    return output;
}

void cvk_executor_thread::executor() {
    cvk_set_current_thread_name_if_supported("clvk-executor");

    std::unique_lock<std::mutex> lock(m_lock);

    while (!m_shutdown) {

        while (m_groups.size() == 0 && !m_shutdown) {
            m_running = false;
            TRACE_BEGIN("executor_wait");
            m_cv.wait(lock);
            TRACE_END();
        }

        if (m_shutdown) {
            continue;
        }

        auto group = std::move(m_groups.front());
        m_groups.pop_front();

        cvk_debug_fn("received group %p", group.get());

        lock.unlock();
        group->execute_cmds_in_executor();
        lock.lock();
    }
}

cl_int cvk_command_queue::flush_no_lock() {
    TRACE_FUNCTION("queue", (uintptr_t)this, "group_size",
                   m_groups.front()->commands.size());
    cvk_debug_fn("queue = %p - group_size = %lu", this,
                 m_groups.front()->commands.size());

    std::unique_ptr<cvk_command_group> group;

    // End current command batch
    cl_int err = end_current_command_batch(true);
    if (err != CL_SUCCESS) {
        return err;
    }

    // Early exit if there are no commands in the queue
    if (m_groups.front()->commands.size() == 0) {
        for (auto& controller : m_controllers) {
            controller->update_after_empty_flush();
        }
        return CL_SUCCESS;
    }

    // Get the commands from the queue and prepare the queue to receive
    // further commands
    group = std::move(m_groups.front());
    m_groups.pop_front();
    m_groups.push_back(std::make_unique<cvk_command_group>());

    cvk_debug_fn("groups.size() = %zu", m_groups.size());

    CVK_ASSERT(group->commands.size() > 0);

    // Set event state and profiling info
    for (auto cmd : group->commands) {
        cmd->set_event_status(CL_SUBMITTED);
    }

    // Create execution thread if it doesn't exist
    if (m_executor == nullptr) {
        m_executor = get_thread_pool()->get_executor();
    }

    auto ev = group->commands.back()->event();
    m_finish_event.reset(ev);
    cvk_debug_fn("set finish event to %p", ev);

    // Submit command group to executor
    m_executor->send_group(std::move(group));
    group_sent();

    return CL_SUCCESS;
}

cl_int cvk_command_queue::flush() {
    std::lock_guard<std::mutex> lock(m_lock);
    return flush_no_lock();
}

cl_int cvk_command_queue::finish() {
    std::unique_lock<std::mutex> lock(m_lock);

    auto status = flush_no_lock();
    if (status != CL_SUCCESS) {
        return status;
    }

    if (m_finish_event != nullptr) {
        _cl_event* evt_list = (_cl_event*)&*m_finish_event;
        execute_cmds_required_by_no_lock(1, &evt_list, lock);
        m_finish_event->wait();
    }

    return CL_SUCCESS;
}

VkResult cvk_command_pool::allocate_command_buffer(VkCommandBuffer* cmdbuf) {

    std::lock_guard<std::mutex> lock(m_lock);

    VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, m_command_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1 // commandBufferCount
    };

    return vkAllocateCommandBuffers(m_device->vulkan_device(),
                                    &commandBufferAllocateInfo, cmdbuf);
}

void cvk_command_pool::free_command_buffer(VkCommandBuffer buf) {
    std::lock_guard<std::mutex> lock(m_lock);
    vkFreeCommandBuffers(m_device->vulkan_device(), m_command_pool, 1, &buf);
}

bool cvk_command_buffer::begin() {
#ifdef CLVK_UNIT_TESTING_ENABLED
    if (config.force_cvk_command_buffer_begin_error()) {
        return false;
    }
#endif

    if (!m_queue->allocate_command_buffer(&m_command_buffer)) {
        return false;
    }

    cvk_command_pool_lock_holder lock(m_queue);

    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr // pInheritanceInfo
    };

    VkResult res = vkBeginCommandBuffer(m_command_buffer, &beginInfo);
    if (res != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool cvk_command_buffer::submit_and_wait() {
    auto& queue = m_queue->vulkan_queue();
    const auto* trace = m_dispatch_trace.get();
    const bool time_submission = trace || config.max_batch_duration_us;
    uint64_t started = time_submission ? cvk_dispatch_trace::monotonic_ns() : 0;
    if (trace) {
        cvk_info("DISPATCH_SUBMIT_BEGIN id=%llu mono_ns=%llu utc_ns=%llu cl_queue=%p vk_queue=%p cmdbuf=%p commands=%llu dispatches=%llu matched=%llu retained=%llu",
            (unsigned long long)trace->id, (unsigned long long)started,
            (unsigned long long)cvk_dispatch_trace::utc_ns(), (void*)&*m_queue,
            (void*)queue.handle(), (void*)m_command_buffer,
            (unsigned long long)trace->commands, (unsigned long long)trace->dispatches,
            (unsigned long long)trace->matched,
            (unsigned long long)std::min<uint64_t>(trace->matched, cvk_dispatch_trace::capacity));
        trace->each([&](const cvk_dispatch_trace::record& r) {
            cvk_info("DISPATCH_RECORD id=%llu ordinal=%llu command=%p event=%p kernel=%p program=%p name=%s truncated=%u dimensions=%u gws={%u,%u,%u} lws={%u,%u,%u} offset={%u,%u,%u} region_gws={%u,%u,%u} region_lws={%u,%u,%u} region_offset={%u,%u,%u}",
                (unsigned long long)trace->id, (unsigned long long)r.ordinal,
                (void*)r.command, (void*)r.event, (void*)r.kernel, (void*)r.program,
                r.name.data(), unsigned(r.name_truncated), r.dimensions,
                r.global[0], r.global[1], r.global[2], r.local[0], r.local[1], r.local[2],
                r.offset[0], r.offset[1], r.offset[2],
                r.region_global[0], r.region_global[1], r.region_global[2],
                r.region_local[0], r.region_local[1], r.region_local[2],
                r.region_offset[0], r.region_offset[1], r.region_offset[2]);
            if (r.arguments) {
                cvk_info("DISPATCH_ARGUMENTS id=%llu ordinal=%llu command=%p total=%llu retained=%llu",
                    (unsigned long long)trace->id, (unsigned long long)r.ordinal,
                    (void*)r.command, (unsigned long long)r.arguments->total,
                    (unsigned long long)std::min<uint64_t>(r.arguments->total, cvk_dispatch_arguments::capacity));
                for (size_t i = 0; i < std::min<uint64_t>(r.arguments->total, cvk_dispatch_arguments::capacity); ++i) {
                    const auto& a = r.arguments->args[i];
                    const auto hex = a.scalar_hex();
                    cvk_info("DISPATCH_ARGUMENT id=%llu ordinal=%llu pos=%u kind=%u name=\"%s\" type=\"%s\" name_truncated=%u type_truncated=%u set=%u binding=%u offset=%u declared_size=%u scalar_valid=%u scalar_bytes=%u scalar_truncated=%u scalar_hex=%s resource=%p resource_bytes=%llu memory_type=%u flags=%llu parent_offset=%llu local_bytes=%llu image={%llu,%llu,%llu,%llu,%llu,%llu}",
                        (unsigned long long)trace->id, (unsigned long long)r.ordinal,
                        a.pos, a.kind, a.name.data(), a.type_name.data(),
                        unsigned(a.name_truncated), unsigned(a.type_truncated),
                        a.set, a.binding, a.offset, a.size, unsigned(a.scalar_valid),
                        a.scalar_bytes, unsigned(a.scalar_truncated), hex.data(),
                        (void*)a.resource, (unsigned long long)a.resource_bytes,
                        a.memory_type, (unsigned long long)a.flags,
                        (unsigned long long)a.parent_offset, (unsigned long long)a.local_bytes,
                        (unsigned long long)a.image[0], (unsigned long long)a.image[1],
                        (unsigned long long)a.image[2], (unsigned long long)a.image[3],
                        (unsigned long long)a.image[4], (unsigned long long)a.image[5]);
                }
            }
        });
        // A TDR/process termination must not strand attribution in stdio.
        cvk_log_flush();
    }

    VkResult res = queue.submit(m_command_buffer);
    if (trace) {
        cvk_info("DISPATCH_SUBMIT_RETURN id=%llu elapsed_ns=%llu result=%d",
            (unsigned long long)trace->id,
            (unsigned long long)(cvk_dispatch_trace::monotonic_ns() - started), res);
        cvk_log_flush();
    }

    if (res != VK_SUCCESS) {
        if (time_submission) m_elapsed_ns = cvk_dispatch_trace::monotonic_ns() - started;
        return false;
    }

    res = queue.wait_idle();
    if (time_submission) m_elapsed_ns = cvk_dispatch_trace::monotonic_ns() - started;
    if (trace) {
        cvk_info("DISPATCH_WAIT_RETURN id=%llu elapsed_ns=%llu result=%d scope=whole-vulkan-queue",
            (unsigned long long)trace->id,
            (unsigned long long)(cvk_dispatch_trace::monotonic_ns() - started), res);
        cvk_log_flush();
    }

    if (res != VK_SUCCESS) {
        return false;
    }

    return true;
}

cl_int cvk_command_kernel::update_global_push_constants(
    cvk_command_buffer& command_buffer) {
    auto program = m_kernel->program();

    if (auto pc = program->push_constant(pushconstant::global_offset)) {
        CVK_ASSERT(pc->size == 12);
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &m_ndrange.offset);
    }

    if (auto pc = program->push_constant(pushconstant::enqueued_local_size)) {
        CVK_ASSERT(pc->size == 12);
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &m_ndrange.lws);
    }

    if (auto pc = program->push_constant(pushconstant::global_size)) {
        CVK_ASSERT(pc->size == 12);
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &m_ndrange.gws);
    }

    if (auto pc = program->push_constant(pushconstant::num_workgroups)) {
        CVK_ASSERT(pc->size == 12);
        uint32_t num_workgroups[3] = {m_ndrange.gws[0] / m_ndrange.lws[0],
                                      m_ndrange.gws[1] / m_ndrange.lws[1],
                                      m_ndrange.gws[2] / m_ndrange.lws[2]};

        for (int i = 0; i < 3; i++) {
            if (m_ndrange.gws[i] % m_ndrange.lws[i] != 0) {
                num_workgroups[i]++;
            }
        }
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &num_workgroups);
    }

    if (auto pc =
            program->push_constant(pushconstant::module_constants_pointer)) {
        CVK_ASSERT(pc->size == 8);

        auto buffer = program->module_constant_data_buffer();
        auto dev_addr = buffer->device_address();

        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &dev_addr);
    }

    if (auto pc = program->push_constant(pushconstant::printf_buffer_pointer)) {
        CVK_ASSERT(pc->size == 8);
        CVK_ASSERT(program->uses_printf());

        auto buffer = m_queue->get_printf_buffer();
        if (buffer == nullptr) {
            cvk_error_fn("printf buffer was not created");
            return CL_OUT_OF_RESOURCES;
        }
        auto dev_addr = buffer->device_address();

        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &dev_addr);
    }

    uint32_t image_metadata_pc_start = UINT32_MAX;
    uint32_t image_metadata_pc_end = 0;
    if (const auto* md = m_kernel->get_image_metadata()) {
        for (const auto& md : *md) {
            if (md.second.has_valid_order()) {
                auto order_offset = md.second.order_offset;
                image_metadata_pc_start =
                    std::min(image_metadata_pc_start, order_offset);
                image_metadata_pc_end =
                    std::max(image_metadata_pc_end,
                             order_offset + (uint32_t)sizeof(uint32_t));
            }
            if (md.second.has_valid_data_type()) {
                auto data_type_offset = md.second.data_type_offset;
                image_metadata_pc_start =
                    std::min(image_metadata_pc_start, data_type_offset);
                image_metadata_pc_end =
                    std::max(image_metadata_pc_end,
                             data_type_offset + (uint32_t)sizeof(uint32_t));
            }
        }
    }
    if (const auto* md = m_kernel->get_sampler_metadata()) {
        for (const auto& md : *md) {
            auto offset = md.second;
            image_metadata_pc_start = std::min(image_metadata_pc_start, offset);
            image_metadata_pc_end = std::max(
                image_metadata_pc_end, offset + (uint32_t)sizeof(uint32_t));
        }
    }
    if (image_metadata_pc_start < image_metadata_pc_end) {
        uint32_t offset = image_metadata_pc_start & ~0x3U;
        uint32_t size = round_up(image_metadata_pc_end - offset, 4);
        CVK_ASSERT(offset + size <= m_argument_values->pod_data().size());
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, offset, size,
                           &m_argument_values->pod_data()[offset]);
    }
    if (m_kernel->has_pod_arguments() &&
        !m_kernel->has_pod_buffer_arguments()) {
        for (auto& arg : m_kernel->arguments()) {
            if (arg.kind == kernel_argument_kind::pod_pushconstant ||
                arg.kind == kernel_argument_kind::pointer_pushconstant) {
                CVK_ASSERT(arg.offset + arg.size <=
                           m_argument_values->pod_data().size());

                // Vulkan valid usage states push constants can only be updated
                // in chunks whose offset and size are a multiple of 4.
                uint32_t size = round_up(arg.size, 4);
                uint32_t offset = arg.offset & ~0x3U;
                vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                                   VK_SHADER_STAGE_COMPUTE_BIT, offset, size,
                                   &m_argument_values->pod_data()[offset]);
            }
        }
    }
    return CL_SUCCESS;
}

cl_int cvk_command_kernel::dispatch_uniform_region_within_vklimits(
    const cvk_ndrange& region, cvk_command_buffer& command_buffer) {

    cvk_debug("region within vklimits: gws = {%u,%u,%u}, lws = {%u,%u,%u}, "
              "offset = "
              "{%u,%u,%u}",
              region.gws[0], region.gws[1], region.gws[2], region.lws[0],
              region.lws[1], region.lws[2], region.offset[0], region.offset[1],
              region.offset[2]);

    // Calculate number of workgroups for region
    std::array<uint32_t, 3> num_workgroups;
    for (cl_uint i = 0; i < 3; i++) {
        CVK_ASSERT(region.gws[i] % region.lws[i] == 0);
        num_workgroups[i] = region.gws[i] / region.lws[i];
    };

    auto program = m_kernel->program();
    auto constants = program->spec_constants();
    // TODO: if all kernels in the module use the same reqd_workgroup_size ,
    // clspv will not generate specialization constants for workgroup size, but
    // these values should be error checked.
    uint32_t wgsize_x_id = 0;
    auto where = constants.find(spec_constant::workgroup_size_x);
    if (where != constants.end()) {
        wgsize_x_id = where->second;
    }
    uint32_t wgsize_y_id = 1;
    where = constants.find(spec_constant::workgroup_size_y);
    if (where != constants.end()) {
        wgsize_y_id = where->second;
    }
    uint32_t wgsize_z_id = 2;
    where = constants.find(spec_constant::workgroup_size_z);
    if (where != constants.end()) {
        wgsize_z_id = where->second;
    }

    cvk_spec_constant_map specConstants = {
        {wgsize_x_id, region.lws[0]},
        {wgsize_y_id, region.lws[1]},
        {wgsize_z_id, region.lws[2]},
    };
    for (auto const& spec_value :
         m_argument_values->specialization_constants()) {
        specConstants[spec_value.first] = spec_value.second;
    }
    // Clspv allocates a spec constant for work dimensions if get_work_dim() is
    // used.
    where = constants.find(spec_constant::work_dim);
    if (where != constants.end()) {
        uint32_t dim_id = where->second;
        specConstants[dim_id] = m_dimensions;
    }

    where = constants.find(spec_constant::subgroup_max_size);
    if (where != constants.end()) {
        uint32_t size_id = where->second;
        specConstants[size_id] = m_queue->device()->sub_group_size();
    }

    // Clspv can allocate spec constants for global offset.
    where = constants.find(spec_constant::global_offset_x);
    if (where != constants.end()) {
        uint32_t offset_id = where->second;
        specConstants[offset_id] = m_ndrange.offset[0];
    }
    where = constants.find(spec_constant::global_offset_y);
    if (where != constants.end()) {
        uint32_t offset_id = where->second;
        specConstants[offset_id] = m_ndrange.offset[1];
    }
    where = constants.find(spec_constant::global_offset_z);
    if (where != constants.end()) {
        uint32_t offset_id = where->second;
        specConstants[offset_id] = m_ndrange.offset[2];
    }

    m_pipeline = m_kernel->create_pipeline(specConstants);

    if (m_pipeline == VK_NULL_HANDLE) {
        return CL_OUT_OF_RESOURCES;
    }

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      m_pipeline);

    if (auto pc = program->push_constant(pushconstant::region_offset)) {
        CVK_ASSERT(pc->size == 12);
        uint32_t region_offsets[3] = {
            m_ndrange.offset[0] + region.offset[0],
            m_ndrange.offset[1] + region.offset[1],
            m_ndrange.offset[2] + region.offset[2],
        };
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &region_offsets);
    }

    if (auto pc = program->push_constant(pushconstant::region_group_offset)) {
        CVK_ASSERT(pc->size == 12);
        uint32_t region_group_offsets[3] = {
            region.offset[0] / m_ndrange.lws[0],
            region.offset[1] / m_ndrange.lws[1],
            region.offset[2] / m_ndrange.lws[2],
        };
        vkCmdPushConstants(command_buffer, m_kernel->pipeline_layout(),
                           VK_SHADER_STAGE_COMPUTE_BIT, pc->offset, pc->size,
                           &region_group_offsets);
    }

    if (auto* trace = command_buffer.dispatch_trace()) {
        cvk_dispatch_trace::record r;
        r.command = (uintptr_t)this; r.event = (uintptr_t)m_event;
        r.kernel = (uintptr_t)&*m_kernel; r.program = (uintptr_t)program;
        r.dimensions = m_dimensions;
        r.global = m_ndrange.gws; r.local = m_ndrange.lws; r.offset = m_ndrange.offset;
        r.region_global = region.gws; r.region_local = region.lws; r.region_offset = region.offset;
        r.arguments = m_trace_arguments;
        trace->dispatch(m_kernel->name().c_str(), r);
    }
    vkCmdDispatch(command_buffer, num_workgroups[0], num_workgroups[1],
                  num_workgroups[2]);

    // If we have a kernel that requires serial execution (i.e. regions are not
    // executed in parallel with other regions or other kernels) then serialize
    // the command buffer
    if (m_kernel->requires_serialized_execution()) {
        VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                                         VK_ACCESS_MEMORY_READ_BIT |
                                             VK_ACCESS_MEMORY_WRITE_BIT};

        // Workaround for a bug on some NVIDIA devices.
        // This should already be covered by VK_ACCESS_MEMORY_READ_BIT.
        memoryBarrier.dstAccessMask |= VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // srcStageMask
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // dstStageMask
            0,                                    // dependencyFlags
            1,                                    // memoryBarrierCount
            &memoryBarrier,
            0,        // bufferMemoryBarrierCount
            nullptr,  // pBufferMemoryBarriers
            0,        // imageMemoryBarrierCount
            nullptr); // pImageMemoryBarriers
    }

    return CL_SUCCESS;
}

cl_int cvk_command_kernel::dispatch_uniform_region_iterate(
    unsigned int dim, const cvk_ndrange& region, const size_t* region_lws,
    size_t* region_gws, size_t* region_offset,
    cvk_command_buffer& command_buffer, uint32_t* num_workgroups) {

    auto& vklimits = m_queue->device()->vulkan_limits();

    size_t num_splitted_regions =
        ceil_div(num_workgroups[dim], vklimits.maxComputeWorkGroupCount[dim]);
    size_t splitted_region_gws =
        vklimits.maxComputeWorkGroupCount[dim] * region_lws[dim];

    for (size_t i = 0; i < num_splitted_regions; ++i) {
        size_t splitted_offset = i * splitted_region_gws;
        region_offset[dim] = splitted_offset + region.offset[dim];
        region_gws[dim] =
            std::min(splitted_region_gws, region.gws[dim] - splitted_offset);

        cl_int err;
        if (dim == 0) {
            const cvk_ndrange region_within_vklimits(3, region_offset,
                                                     region_gws, region_lws);
            err = dispatch_uniform_region_within_vklimits(
                region_within_vklimits, command_buffer);
        } else {
            err = dispatch_uniform_region_iterate(
                dim - 1, region, region_lws, region_gws, region_offset,
                command_buffer, num_workgroups);
        }
        if (err != CL_SUCCESS)
            return err;
    }

    return CL_SUCCESS;
}

cl_int cvk_command_kernel::dispatch_uniform_region(
    const cvk_ndrange& region, cvk_command_buffer& command_buffer) {

    // Calculate number of workgroups for region
    uint32_t num_workgroups[3];
    for (cl_uint i = 0; i < 3; i++) {
        CVK_ASSERT(region.gws[i] % region.lws[i] == 0);
        num_workgroups[i] = region.gws[i] / region.lws[i];
    };

    auto program = m_kernel->program();
    auto& vklimits = m_queue->device()->vulkan_limits();
    if (!program->can_split_region()) {
        for (cl_uint i = 0; i < 3; ++i) {
            if (num_workgroups[i] > vklimits.maxComputeWorkGroupCount[i]) {
                cvk_error_fn("Number of workgroups (%d, %d, %d) required to "
                             "dispatch the uniform region exceeds device limits"
                             " of (%d, %d, %d)",
                             num_workgroups[0], num_workgroups[1],
                             num_workgroups[2],
                             vklimits.maxComputeWorkGroupCount[0],
                             vklimits.maxComputeWorkGroupCount[1],
                             vklimits.maxComputeWorkGroupCount[2]);
                cvk_error_fn(
                    "Splitting this region is required, but it is not possible "
                    "because the support has been disabled (most probably by "
                    "'-cl-uniform-work-group-size').");

                return CL_INVALID_WORK_GROUP_SIZE;
            }
        }
    }

    if (region.lws[0] * region.lws[1] * region.lws[2] >
        vklimits.maxComputeWorkGroupInvocations) {
        cvk_error_fn("Too many work items per workgroup: %u * %u * %u > %u",
                     region.lws[0], region.lws[1], region.lws[2],
                     vklimits.maxComputeWorkGroupInvocations);
        return CL_INVALID_WORK_GROUP_SIZE;
    }

    for (int i = 0; i < 3; i++) {
        if (region.lws[i] > vklimits.maxComputeWorkGroupSize[i]) {
            return CL_INVALID_WORK_ITEM_SIZE;
        }
    }
    size_t region_gws[3];
    size_t region_offset[3];
    const size_t region_lws[3] = {region.lws[0], region.lws[1], region.lws[2]};
    return dispatch_uniform_region_iterate(2, region, region_lws, region_gws,
                                           region_offset, command_buffer,
                                           num_workgroups);
}

cl_int cvk_command_kernel::build_and_dispatch_regions(
    cvk_command_buffer& command_buffer) {

    auto program = m_kernel->program();
    if (!program->can_split_region()) {
        for (uint32_t dim = 0; dim < m_dimensions; dim++) {
            if ((m_ndrange.gws[dim] % m_ndrange.lws[dim]) != 0) {
                return CL_INVALID_WORK_GROUP_SIZE;
            }
        }
    }

    // Split non-uniform NDRange into uniform regions
    cvk_ndrange regions[8];
    regions[0].offset = {0};
    regions[0].gws = m_ndrange.gws;
    regions[0].lws = m_ndrange.lws;
    uint32_t stackpos = 0;
    uint32_t num_regions = 1;
    do {
        cvk_ndrange* region = &regions[stackpos];
        for (uint32_t dim = 0; dim < m_dimensions; dim++) {
            auto mod = region->gws[dim] % region->lws[dim];

            cvk_ndrange* splitout_region = &regions[num_regions];

            if (mod != 0) {
                *splitout_region = *region;

                auto quo = region->gws[dim] - mod;

                region->gws[dim] = quo;

                splitout_region->gws[dim] = mod;
                splitout_region->lws[dim] = mod;
                splitout_region->offset[dim] = quo;

                num_regions++;
            }
        }
    } while (++stackpos < num_regions);

    // Dispatch regions
    for (uint32_t i = 0; i < num_regions; i++) {
        cvk_debug("region %u: gws = {%u,%u,%u}, lws = {%u,%u,%u}, offset = "
                  "{%u,%u,%u}",
                  i, regions[i].gws[0], regions[i].gws[1], regions[i].gws[2],
                  regions[i].lws[0], regions[i].lws[1], regions[i].lws[2],
                  regions[i].offset[0], regions[i].offset[1],
                  regions[i].offset[2]);
        auto err = dispatch_uniform_region(regions[i], command_buffer);
        if (err != CL_SUCCESS) {
            return err;
        }
    }

    return CL_SUCCESS;
}

cl_int cvk_command_kernel::prepare_arguments() {

    if (m_argument_values) return CL_SUCCESS;

    // TODO check against the size specified at compile time, if any
    // TODO CL_INVALID_KERNEL_ARGS if the kernel argument values have not been
    // specified.

    m_argument_values = m_kernel->argument_values();
    m_argument_values->retain_resources();

    // Setup descriptors
    if (!m_argument_values->setup_descriptor_sets()) {
        m_argument_values->release_resources();
        m_argument_values = nullptr;
        return CL_OUT_OF_RESOURCES;
    }

    // Setup printf buffer descriptor if needed
    if (m_kernel->program()->uses_printf()) {
        // Create and initialize the printf buffer
        auto buffer = m_queue->get_or_create_printf_buffer();
        auto err = m_queue->reset_printf_buffer();
        if (err != CL_SUCCESS) {
            return err;
        }

        if (m_kernel->program()->printf_buffer_info().type ==
            module_buffer_type::storage_buffer) {

            VkDescriptorBufferInfo bufferInfo = {buffer->vulkan_buffer(),
                                                 0, // offset
                                                 VK_WHOLE_SIZE};

            auto* ds = m_argument_values->descriptor_sets();
            VkWriteDescriptorSet writeDescriptorSet = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                ds[m_kernel->program()->printf_buffer_info().set],
                m_kernel->program()->printf_buffer_info().binding,
                0,                                 // dstArrayElement
                1,                                 // descriptorCount
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // descriptorType
                nullptr,                           // pImageInfo
                &bufferInfo,
                nullptr, // pTexelBufferView
            };

            vkUpdateDescriptorSets(m_queue->device()->vulkan_device(), 1u,
                                   &writeDescriptorSet, 0, nullptr);
        }
    }

    capture_dispatch_arguments();
    return CL_SUCCESS;
}

cl_int
cvk_command_kernel::build_batchable_inner(cvk_command_buffer& command_buffer) {
    auto err = prepare_arguments();
    if (err != CL_SUCCESS) return err;

    // Bind descriptors and update push constants for every command buffer.
    if (m_kernel->num_set_layouts() > 0) {
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_kernel->pipeline_layout(), 0,
                                m_kernel->num_set_layouts(),
                                m_argument_values->descriptor_sets(), 0, 0);
    }

    err = update_global_push_constants(command_buffer);
    if (err != CL_SUCCESS) {
        return err;
    }

    // Dispatch work
    if (m_tile_budget) {
        cvk_ndrange region;
        region.offset = m_tile.offset;
        region.gws = m_tile.gws;
        region.lws = m_tile.lws;
        err = dispatch_uniform_region_within_vklimits(region, command_buffer);
    } else {
        err = build_and_dispatch_regions(command_buffer);
    }
    if (err != CL_SUCCESS) {
        return err;
    }

    // Synchronise wrt to memory and other commands
    VkMemoryBarrier memoryBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, // sType
                                     nullptr,                          // pNext
                                     VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_ACCESS_MEMORY_READ_BIT |
                                         VK_ACCESS_MEMORY_WRITE_BIT};

    // Workaround for a bug on some NVIDIA devices.
    // This should already be covered by VK_ACCESS_MEMORY_READ_BIT.
    memoryBarrier.dstAccessMask |= VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // srcStageMask
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // dstStageMask
        0,                                        // dependencyFlags
        1,                                        // memoryBarrierCount
        &memoryBarrier,
        0,        // bufferMemoryBarrierCount
        nullptr,  // pBufferMemoryBarriers
        0,        // imageMemoryBarrierCount
        nullptr); // pImageMemoryBarriers

    return CL_SUCCESS;
}

cl_int cvk_command_kernel::build() {
    if (m_tile_budget) {
        const auto* limits = m_queue->device()->vulkan_limits().maxComputeWorkGroupCount;
        if (!m_tiles.init(m_ndrange.offset, m_ndrange.gws, m_ndrange.lws,
                          {limits[0], limits[1], limits[2]}, m_tile_budget) ||
            !m_tiles.next(m_tile)) return CL_INVALID_GLOBAL_WORK_SIZE;
        m_tile_first = true;
        m_tile_ordinal = 0;
    } else if (config.max_dispatch_workgroups) {
        cvk_info("NDRANGE_TILE_BYPASS command=%p event=%p reason=%s",
                 (void*)this, (void*)event(),
                 m_kernel->program()->uses_printf() ? "shared_printf_buffer" :
                 (!m_ndrange.gws[0] || !m_ndrange.gws[1] || !m_ndrange.gws[2]) ?
                     "empty_ndrange" :
                 !m_kernel->program()->has_generated_region_abi() ?
                     "unproven_region_abi" : "within_workgroup_budget");
    }
    return cvk_command_batchable::build();
}

cl_int cvk_command_kernel::do_action() {
    if (!m_tile_budget) return cvk_command_batchable::do_action();
    return cvk_run_tile_submissions(
        [&]() -> int {
            ++m_tile_ordinal;
            if (config.dispatch_trace) {
                cvk_info("NDRANGE_TILE_SUBMIT command=%p event=%p tile=%llu budget=%u first=%u last=%u",
                         (void*)this, (void*)event(),
                         (unsigned long long)m_tile_ordinal, m_tile_budget,
                         unsigned(first_recording()), unsigned(last_recording()));
                cvk_log_flush();
            }
            return m_command_buffer->submit_and_wait() ? CL_SUCCESS : CL_OUT_OF_RESOURCES;
        },
        [&](bool& more) -> int {
            more = m_tiles.next(m_tile);
            if (!more) return CL_SUCCESS;
            m_tile_first = false;
            return cvk_command_batchable::build();
        },
        [&]() -> int { return do_post_action(); });
}

void cvk_command_kernel::capture_dispatch_arguments() {
    if (!config.dispatch_trace || !config.dispatch_trace_arguments) return;
    const std::string& filter = config.dispatch_trace_arguments_kernel();
    if (!filter.empty() && m_kernel->name().find(filter) == std::string::npos) return;
    auto metadata = std::make_shared<cvk_dispatch_arguments>();
    const auto& args = m_kernel->arguments();
    metadata->total = args.size();
    const auto* pod = m_argument_values->pod_data_if_present();
    for (size_t i = 0; i < std::min(args.size(), cvk_dispatch_arguments::capacity); ++i) {
        const auto& arg = args[i];
        auto& out = metadata->args[i];
        out.pos = arg.pos; out.kind = uint32_t(arg.kind);
        out.set = arg.descriptorSet; out.binding = arg.binding;
        out.offset = arg.offset; out.size = arg.size;
        out.name_truncated = cvk_dispatch_arguments::text(out.name, arg.info.name);
        out.type_truncated = cvk_dispatch_arguments::text(out.type_name, arg.info.type_name);
        if (arg.is_pod()) {
            out.capture_scalar(pod ? pod->data() : nullptr, pod ? pod->size() : 0,
                arg.offset, arg.size);
        } else if (arg.kind == kernel_argument_kind::local) {
            out.local_bytes = m_argument_values->local_arg_size(arg.pos);
        } else if (arg.is_mem_object_backed()) {
            const auto* mem = static_cast<cvk_mem*>(m_argument_values->get_arg_value(arg));
            out.resource = uintptr_t(mem);
            if (mem) {
                out.resource_bytes = mem->size(); out.memory_type = mem->type();
                out.flags = mem->flags(); out.parent_offset = mem->parent_offset();
                if (mem->is_image_type()) {
                    const auto* image = static_cast<const cvk_image*>(mem);
                    out.image = {image->width(), image->height(), image->depth(),
                        image->array_size(), image->row_pitch(), image->slice_pitch()};
                }
            }
        }
    }
    m_trace_arguments = std::move(metadata);
}

std::shared_ptr<cvk_batch_cost> cvk_command_kernel::find_batch_cost() {
    if (m_tile_budget) return {};
    cvk_batch_cost_key key;
    key.value(m_dimensions);
    for (auto value : m_ndrange.gws) key.value(value);
    for (auto value : m_ndrange.lws) key.value(value);
    for (auto value : m_ndrange.offset) key.value(value);
    const auto values = m_kernel->argument_values();
    const auto* pod = values->pod_data_if_present();
    key.value(pod ? pod->size() : 0);
    if (pod) key.append(pod->data(), pod->size());
    for (const auto& arg : m_kernel->arguments()) {
        key.value(uint32_t(arg.kind));
        if (arg.kind == kernel_argument_kind::local) {
            key.value(values->local_arg_size(arg.pos));
        } else if (arg.is_mem_object_backed()) {
            const auto* mem = static_cast<cvk_mem*>(values->get_arg_value(arg));
            key.value(mem != nullptr);
            if (mem) {
                key.value(mem->size()); key.value(mem->type());
                key.value(mem->flags()); key.value(mem->parent_offset());
                if (mem->is_image_type()) {
                    const auto* image = static_cast<const cvk_image*>(mem);
                    key.value(image->width()); key.value(image->height());
                    key.value(image->depth()); key.value(image->array_size());
                    key.value(image->row_pitch()); key.value(image->slice_pitch());
                }
            }
        } else if (arg.kind == kernel_argument_kind::sampler) {
            key.value(uintptr_t(values->get_arg_value(arg)));
        }
    }
    return m_kernel->batch_cost(key);
}

cl_int cvk_command_kernel::do_post_action() {
    if (m_kernel->uses_printf()) {
        auto buffer = m_queue->get_printf_buffer();
        if (buffer == nullptr) {
            cvk_error_fn("printf buffer was not created");
            return CL_OUT_OF_RESOURCES;
        }

        return cvk_printf(buffer, m_kernel->program()->printf_descriptors(),
                          m_queue->context()->get_printf_callback(),
                          m_queue->context()->get_printf_userdata());
    }

    return CL_SUCCESS;
}

cl_int cvk_command_batchable::build() {
    m_command_buffer = std::make_unique<cvk_command_buffer>(m_queue);
    if (!m_command_buffer->begin()) {
        return CL_OUT_OF_RESOURCES;
    }

    cvk_command_pool_lock_holder lock(m_queue);
    cl_int err = build(*m_command_buffer);
    if (err != CL_SUCCESS) {
        return err;
    }

    if (!m_command_buffer->end()) {
        return CL_OUT_OF_RESOURCES;
    }

    return CL_SUCCESS;
}

cl_int cvk_command_batchable::build(cvk_command_buffer& command_buffer) {
    CVK_ASSERT(m_command_buffer == nullptr ||
               (*m_command_buffer == command_buffer));
    // Create query pool
    VkQueryPoolCreateInfo query_pool_create_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        nullptr,
        0,                            // flags
        VK_QUERY_TYPE_TIMESTAMP,      // queryType
        NUM_POOL_QUERIES_PER_COMMAND, // queryCount
        0,                            // pipelineStatistics
    };

    bool profiling = m_queue->has_property(CL_QUEUE_PROFILING_ENABLE);

    if (profiling && m_queue->profiling_on_device() &&
        m_query_pool == VK_NULL_HANDLE) {
        auto vkdev = m_queue->device()->vulkan_device();
        auto res = vkCreateQueryPool(vkdev, &query_pool_create_info, nullptr,
                                     &m_query_pool);
        if (res != VK_SUCCESS) {
            return CL_OUT_OF_RESOURCES;
        }
    }

    // Sample timestamp if profiling
    if (profiling && m_queue->profiling_on_device() && first_recording()) {
        vkCmdResetQueryPool(command_buffer, m_query_pool, 0,
                            NUM_POOL_QUERIES_PER_COMMAND);
        vkCmdWriteTimestamp(command_buffer,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool,
                            POOL_QUERY_CMD_START);
    }

    auto err = build_batchable_inner(command_buffer);
    if (err != CL_SUCCESS) {
        return err;
    }
    if (auto* trace = command_buffer.dispatch_trace()) trace->command();

    // Sample timestamp if profiling
    if (profiling && m_queue->profiling_on_device() && last_recording()) {
        vkCmdWriteTimestamp(command_buffer,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_query_pool,
                            POOL_QUERY_CMD_END);
    }

    return CL_SUCCESS;
}

cl_int cvk_command_batchable::get_timestamp_query_results(cl_ulong* start,
                                                          cl_ulong* end) {
    uint64_t timestamps[NUM_POOL_QUERIES_PER_COMMAND];
    auto dev = m_queue->device();
    auto res = vkGetQueryPoolResults(
        dev->vulkan_device(), m_query_pool, 0, NUM_POOL_QUERIES_PER_COMMAND,
        sizeof(timestamps), timestamps, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (res != VK_SUCCESS) {
        cvk_error_fn("vkGetQueryPoolResults failed %d %s", res,
                     vulkan_error_string(res));
        return CL_OUT_OF_RESOURCES;
    }

    auto ts_start_raw = timestamps[POOL_QUERY_CMD_START];
    auto ts_end_raw = timestamps[POOL_QUERY_CMD_END];

    *start = dev->timestamp_to_ns(ts_start_raw);
    *end = dev->timestamp_to_ns(ts_end_raw);
    if (config.dispatch_trace) {
        cvk_info("DEVICE_TIMESTAMP_QUERY command=%p event=%p start_raw=%llu end_raw=%llu start_ns=%llu end_ns=%llu",
                 (void*)this, (void*)event(),
                 (unsigned long long)ts_start_raw, (unsigned long long)ts_end_raw,
                 (unsigned long long)*start, (unsigned long long)*end);
    }

    return CL_COMPLETE;
}

cl_int cvk_command_batchable::do_action() {
    CVK_ASSERT(m_command_buffer);

    const bool success = m_command_buffer->submit_and_wait();
    if (m_batch_cost)
        m_batch_cost->observe(m_command_buffer->elapsed_ns(),
            uint64_t(config.max_batch_duration_us()) * 1000, success);
    if (!success) {
        return CL_OUT_OF_RESOURCES;
    }

    return do_post_action();
}

void cvk_command_batch::observe_duration(bool api_success) {
    const uint64_t budget = m_duration_window.budget_ns;
    if (!budget) return;
    const uint64_t elapsed = m_command_buffer->elapsed_ns();
    const auto first = m_commands.front()->batch_cost();
    bool homogeneous = first != nullptr;
    for (const auto& command : m_commands)
        homogeneous &= command->batch_cost() == first;
    // Reject the whole over-budget sample before apportioning it. A reset can
    // falsely return API success near a watchdog boundary; it must not train.
    const bool eligible = api_success && elapsed && elapsed < budget;
    const uint64_t sample = cvk_batch_cost_sample(elapsed, budget, api_success,
        m_commands.size(), homogeneous);
    for (const auto& command : m_commands)
        if (command->batch_cost()) command->batch_cost()->observe(sample, budget, api_success);
    if (config.dispatch_trace) {
        const auto* trace = m_command_buffer->dispatch_trace();
        cvk_info("BATCH_DURATION id=%llu cl_queue=%p commands=%llu elapsed_ns=%llu budget_ns=%llu api_success=%u eligible=%u homogeneous=%u scope=whole-vulkan-queue",
            (unsigned long long)(trace ? trace->id : 0),
            (void*)&*m_queue, (unsigned long long)m_commands.size(),
            (unsigned long long)elapsed, (unsigned long long)budget,
            unsigned(api_success), unsigned(eligible), unsigned(homogeneous));
        cvk_log_flush();
    }
}

cl_int cvk_command_batch::do_action() {

    cvk_info("executing batch of %lu commands", m_commands.size());

    const bool success = m_command_buffer->submit_and_wait();
    observe_duration(success);
    if (!success) {
        return CL_OUT_OF_RESOURCES;
    }

    m_queue->batch_completed();

    return CL_COMPLETE;
}

cl_int cvk_command_buffer_host_copy::do_action() {
    bool success = false;

    switch (m_type) {
    case CL_COMMAND_WRITE_IMAGE:
    case CL_COMMAND_WRITE_BUFFER:
        success = m_buffer->copy_from(m_ptr, m_offset, m_size);
        break;
    case CL_COMMAND_READ_IMAGE:
    case CL_COMMAND_READ_BUFFER:
        success = m_buffer->copy_to(m_ptr, m_offset, m_size);
        break;
    default:
        CVK_ASSERT(false);
        break;
    }

    return success ? CL_COMPLETE : CL_OUT_OF_RESOURCES;
}

struct rectangle {
public:
    void set_params(const std::array<size_t, 3>& origin, size_t slicep,
                    size_t rowp, size_t elem_size) {
        m_origin = origin;
        m_slice_pitch = slicep;
        m_row_pitch = rowp;
        m_elem_size = elem_size;
    }

    size_t get_row_offset(size_t slice, size_t row) {
        return m_slice_pitch * (m_origin[2] + slice) +
               m_row_pitch * (m_origin[1] + row) + m_origin[0] * m_elem_size;
    }

private:
    std::array<size_t, 3> m_origin;
    size_t m_slice_pitch;
    size_t m_row_pitch;
    size_t m_elem_size;
};

struct memobj_map_holder {
    enum class access
    {
        read_write,
        read_only,
        write_only,
    };
    memobj_map_holder(cvk_mem* memobj, enum access access)
        : m_mem(memobj), m_mapped(false), m_access(access) {
        CVK_ASSERT(memobj != nullptr);
    }
    ~memobj_map_holder() {
        if (m_mapped) {
            if (m_access == access::read_only) {
                m_mem->unmap_read_only();
            } else {
                m_mem->unmap();
            }
        }
    }

    bool CHECK_RETURN map() {
        if (m_access == access::write_only) {
            m_mapped = m_mem->map_write_only();
        } else {
            m_mapped = m_mem->map();
        }
        return m_mapped;
    }

private:
    cvk_mem* m_mem;
    bool m_mapped;
    enum access m_access;
};

void cvk_rectangle_copier::do_copy(direction dir, void* src_base,
                                   void* dst_base) {
    rectangle ra, rb;

    ra.set_params(m_a_origin, m_a_slice_pitch, m_a_row_pitch, m_elem_size);
    rb.set_params(m_b_origin, m_b_slice_pitch, m_b_row_pitch, m_elem_size);

    rectangle *rsrc, *rdst;
    if (dir == direction::A_TO_B) {
        rsrc = &ra;
        rdst = &rb;
    } else {
        CVK_ASSERT(dir == direction::B_TO_A);
        rsrc = &rb;
        rdst = &ra;
    }

    for (size_t slice = 0; slice < m_region[2]; slice++) {
        // cvk_debug_fn("slice = %zu", slice);
        for (size_t row = 0; row < m_region[1]; row++) {
            // cvk_debug_fn("row = %zu (size = %zu)", row, m_region[0]);
            auto dst =
                pointer_offset(dst_base, rdst->get_row_offset(slice, row));
            auto src =
                pointer_offset(src_base, rsrc->get_row_offset(slice, row));
            memcpy(dst, src, m_region[0] * m_elem_size);
        }
    }
}

cl_int cvk_command_copy_host_buffer_rect::do_action() {
    memobj_map_holder map_holder{m_buffer,
                                 memobj_map_holder::access::read_write};

    if (!map_holder.map()) {
        return CL_OUT_OF_RESOURCES;
    }

    cvk_rectangle_copier::direction dir;
    void *src_base, *dst_base;

    switch (m_type) {
    case CL_COMMAND_READ_BUFFER_RECT:
    case CL_COMMAND_READ_IMAGE:
        dst_base = m_hostptr;
        src_base = m_buffer->host_va();
        dir = cvk_rectangle_copier::direction::A_TO_B;
        break;
    case CL_COMMAND_WRITE_BUFFER_RECT:
    case CL_COMMAND_WRITE_IMAGE:
        dst_base = m_buffer->host_va();
        src_base = m_hostptr;
        dir = cvk_rectangle_copier::direction::B_TO_A;
        break;
    default:
        return CL_INVALID_OPERATION;
    }

    m_copier.do_copy(dir, src_base, dst_base);

    return CL_COMPLETE;
}

cl_int cvk_command_copy_buffer_rect::do_action() {
    memobj_map_holder src_map_holder{m_src_buffer,
                                     memobj_map_holder::access::read_only};
    memobj_map_holder dst_map_holder{m_dst_buffer,
                                     memobj_map_holder::access::write_only};

    if (!src_map_holder.map()) {
        return CL_OUT_OF_RESOURCES;
    }

    if (!dst_map_holder.map()) {
        return CL_OUT_OF_RESOURCES;
    }

    auto dir = cvk_rectangle_copier::direction::A_TO_B;
    auto src_base = m_src_buffer->host_va();
    auto dst_base = m_dst_buffer->host_va();

    m_copier.do_copy(dir, src_base, dst_base);

    return CL_COMPLETE;
}

cl_int cvk_command_copy_buffer::do_action() {
    bool success =
        m_src_buffer->copy_to(m_dst_buffer, m_src_offset, m_dst_offset, m_size);

    return success ? CL_COMPLETE : CL_OUT_OF_RESOURCES;
}

namespace {
template <typename T>
void memset_multi(void* dst, void* pattern_ptr, size_t size) {
    T pattern = *static_cast<T*>(pattern_ptr);
    auto end = pointer_offset(dst, size);
    while (dst < end) {
        *static_cast<T*>(dst) = pattern;
        dst = pointer_offset(dst, sizeof(pattern));
    }
}
} // namespace

cl_int cvk_command_fill_buffer::do_action() {
    memobj_map_holder map_holder{m_buffer,
                                 memobj_map_holder::access::write_only};

    if (!map_holder.map()) {
        return CL_OUT_OF_RESOURCES;
    }

    auto begin = pointer_offset(m_buffer->host_va(), m_offset);
    auto end = pointer_offset(begin, m_size);

    if (m_pattern_size == 1) {
        int pattern = *reinterpret_cast<uint8_t*>(m_pattern.data());
        memset(begin, pattern, m_size);
    } else if (m_pattern_size == 2) {
        memset_multi<uint16_t>(begin, m_pattern.data(), m_size);
    } else if (m_pattern_size == 4) {
        memset_multi<uint32_t>(begin, m_pattern.data(), m_size);
    } else if (m_pattern_size == 8) {
        memset_multi<uint64_t>(begin, m_pattern.data(), m_size);
    } else {
        auto address = begin;
        while (address < end) {
            memcpy(address, m_pattern.data(), m_pattern_size);
            address = pointer_offset(address, m_pattern_size);
        }
    }

    return CL_COMPLETE;
}

cl_int cvk_command_map_buffer::build(void** map_ptr) {

    if (!m_buffer->find_or_create_mapping(m_mapping, m_offset, m_size, m_flags,
                                          m_image)) {
        return CL_OUT_OF_RESOURCES;
    }

    m_mapping_needs_releasing_on_destruction = true;
    *map_ptr = m_mapping.buffer->map_ptr(m_offset);

    return CL_SUCCESS;
}

cl_int cvk_command_map_buffer::do_action() {
    bool success = true;

    if (!m_buffer->insert_mapping(m_mapping)) {
        return false;
    }

    if (m_buffer->has_flags(CL_MEM_USE_HOST_PTR)) {
        auto dst = m_mapping.buffer->host_ptr();
        dst = pointer_offset(dst, m_offset);
        success = m_buffer->copy_to(dst, m_offset, m_size);
    }

    if (success) {
        m_mapping_needs_releasing_on_destruction = false;
    }

    return success ? CL_COMPLETE : CL_OUT_OF_RESOURCES;
}

cl_int cvk_command_unmap_buffer::do_action() {
    bool success = true;

    auto mapping = m_buffer->remove_mapping(m_mapped_ptr);

    if (m_buffer->has_flags(CL_MEM_USE_HOST_PTR)) {
        auto src = m_buffer->host_ptr();
        src = pointer_offset(src, mapping.offset);
        success = mapping.buffer->copy_from(src, mapping.offset, mapping.size);
    }

    return success ? CL_COMPLETE : CL_OUT_OF_RESOURCES;
}

cl_int cvk_command_unmap_image::do_action() {
    // TODO flush caches on non-coherent memory
    m_image->remove_mapping(m_mapped_ptr);

    return CL_COMPLETE;
}

VkImageSubresourceLayers prepare_subresource(const cvk_image* image,
                                             const std::array<size_t, 3>& origin,
                                             const std::array<size_t, 3>& region) {
    uint32_t baseArrayLayer = 0;
    uint32_t layerCount = 1;

    switch (image->type()) {
    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        baseArrayLayer = origin[1];
        layerCount = region[1];
        break;
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
        baseArrayLayer = origin[2];
        layerCount = region[2];
        break;
    }

    VkImageSubresourceLayers ret = {VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
                                    0,                         // mipLevel
                                    baseArrayLayer, layerCount};

    return ret;
}

VkOffset3D prepare_offset(const cvk_image* image, const std::array<size_t, 3>& origin) {

    auto x = static_cast<int32_t>(origin[0]);
    auto y = static_cast<int32_t>(origin[1]);
    auto z = static_cast<int32_t>(origin[2]);

    switch (image->type()) {
    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        y = 0;
        z = 0;
        break;
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
        z = 0;
        break;
    }

    VkOffset3D offset = {x, y, z};

    cvk_debug_fn("offset: %d, %d, %d", offset.x, offset.y, offset.z);

    return offset;
}

VkExtent3D prepare_extent(const cvk_image* image, const std::array<size_t, 3>& region) {
    uint32_t extentHeight = region[1];
    uint32_t extentDepth = region[2];

    switch (image->type()) {
    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        extentHeight = 1;
        extentDepth = 1;
        break;
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
        extentDepth = 1;
        break;
    }

    VkExtent3D extent = {static_cast<uint32_t>(region[0]), extentHeight,
                         extentDepth};

    cvk_debug_fn("extent: %u, %u, %u", extent.width, extent.height,
                 extent.depth);

    return extent;
}

VkBufferImageCopy prepare_buffer_image_copy(const cvk_image* image,
                                            size_t bufferOffset,
                                            const std::array<size_t, 3>& origin,
                                            const std::array<size_t, 3>& region) {

    VkImageSubresourceLayers subResource = prepare_subresource(image, origin, region);

    VkOffset3D offset = prepare_offset(image, origin);

    VkExtent3D extent = prepare_extent(image, region);

    // Tightly pack the data in the destination buffer
    VkBufferImageCopy ret = {
        bufferOffset, // bufferOffset
        0,            // bufferRowLength
        0,            // bufferImageHeight
        subResource,  // imageSubresource
        offset,       // imageOffset
        extent,       // imageExtent
    };
    return ret;
}

void cvk_command_buffer_image_copy::build_inner_image_to_buffer(
    cvk_command_buffer& cmdbuf, const VkBufferImageCopy& region) {
    VkImageSubresourceRange subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
        0,                         // baseMipLevel
        VK_REMAINING_MIP_LEVELS,   // levelCount
        0,                         // baseArrayLayer
        VK_REMAINING_ARRAY_LAYERS, // layerCount
    };

    VkImageMemoryBarrier imageBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_MEMORY_WRITE_BIT,  // srcAccessMask
        VK_ACCESS_TRANSFER_READ_BIT, // dstAccessMask
        VK_IMAGE_LAYOUT_GENERAL,     // oldLayout
        VK_IMAGE_LAYOUT_GENERAL,     // newLayout
        0,                           // srcQueueFamilyIndex
        0,                           // dstQueueFamilyIndex
        m_image->vulkan_image(),     // image
        subresourceRange,
    };

    vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,              // dependencyFlags
                         0,              // memoryBarrierCount
                         nullptr,        // pMemoryBarriers
                         0,              // bufferMemoryBarrierCount
                         nullptr,        // pBufferMemoryBarriers
                         1,              // imageMemoryBarrierCount
                         &imageBarrier); // pImageMemoryBarriers

    vkCmdCopyImageToBuffer(cmdbuf, m_image->vulkan_image(),
                           VK_IMAGE_LAYOUT_GENERAL, m_buffer->vulkan_buffer(),
                           1, &region);
}

void cvk_command_buffer_image_copy::build_inner_buffer_to_image(
    cvk_command_buffer& cmdbuf, const VkBufferImageCopy& region) {
    VkBufferMemoryBarrier bufferBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        0, // srcQueueFamilyIndex
        0, // dstQueueFamilyIndex
        m_buffer->vulkan_buffer(),
        0, // offset
        VK_WHOLE_SIZE};

    vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,              // dependencyFlags
                         0,              // memoryBarrierCount
                         nullptr,        // pMemoryBarriers
                         1,              // bufferMemoryBarrierCount
                         &bufferBarrier, // pBufferMemoryBarriers
                         0,              // imageMemoryBarrierCount
                         nullptr);       // pImageMemoryBarriers

    vkCmdCopyBufferToImage(cmdbuf, m_buffer->vulkan_buffer(),
                           m_image->vulkan_image(), VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
}

cl_int cvk_command_buffer_image_copy::build_batchable_inner(
    cvk_command_buffer& cmdbuf) {

    VkBufferImageCopy region =
        prepare_buffer_image_copy(m_image, m_offset, m_origin, m_region);

    switch (m_copy_type) {
    case CL_COMMAND_COPY_IMAGE_TO_BUFFER:
    case CL_COMMAND_MAP_IMAGE:
        build_inner_image_to_buffer(cmdbuf, region);
        break;
    case CL_COMMAND_COPY_BUFFER_TO_IMAGE:
    case CL_COMMAND_UNMAP_MEM_OBJECT:
        build_inner_buffer_to_image(cmdbuf, region);
        break;
    default:
        CVK_ASSERT(false);
        break;
    }

    VkMemoryBarrier memoryBarrier = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT};

    vkCmdPipelineBarrier(
        cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
        // TODO HOST only when the dest buffer is an image mapping buffer
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, // dependencyFlags
        1, // memoryBarrierCount
        &memoryBarrier,
        0,        // bufferMemoryBarrierCount
        nullptr,  // pBufferMemoryBarriers
        0,        // imageMemoryBarrierCount
        nullptr); // pImageMemoryBarriers

    return CL_SUCCESS;
}

cl_int cvk_command_image_image_copy::build_batchable_inner(
    cvk_command_buffer& cmdbuf) {

    VkImageSubresourceLayers srcSubresource =
        prepare_subresource(m_src_image, m_src_origin, m_region);

    VkOffset3D srcOffset = prepare_offset(m_src_image, m_src_origin);

    VkImageSubresourceLayers dstSubresource =
        prepare_subresource(m_dst_image, m_dst_origin, m_region);

    VkOffset3D dstOffset = prepare_offset(m_dst_image, m_dst_origin);

    VkExtent3D extent = prepare_extent(m_src_image, m_region);

    VkImageCopy region = {srcSubresource, srcOffset, dstSubresource, dstOffset,
                          extent};

    vkCmdCopyImage(cmdbuf, m_src_image->vulkan_image(), VK_IMAGE_LAYOUT_GENERAL,
                   m_dst_image->vulkan_image(), VK_IMAGE_LAYOUT_GENERAL, 1,
                   &region);

    return CL_SUCCESS;
}

cl_int cvk_command_fill_image::do_action() {
    // TODO use bigger memcpy's when possible
    size_t num_elems = m_region[2] * m_region[1] * m_region[0];
    for (size_t elem = 0; elem < num_elems; elem++) {
        auto dst = pointer_offset(m_ptr, elem * m_pattern_size);
        memcpy(dst, m_pattern.data(), m_pattern_size);
    }

    return CL_COMPLETE;
}

cl_int
cvk_command_image_init::build_batchable_inner(cvk_command_buffer& cmdbuf) {

    bool needs_copy = m_image->init_data() != nullptr;

    // Transition image layout to GENERAL or TRANSFER_DST_OPTIMAL.
    VkImageSubresourceRange subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
        0,                         // baseMipLevel
        VK_REMAINING_MIP_LEVELS,   // levelCount
        0,                         // baseArrayLayer
        VK_REMAINING_ARRAY_LAYERS, // layerCount
    };

    VkImageLayout layout = needs_copy ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                      : VK_IMAGE_LAYOUT_GENERAL;

    VkImageMemoryBarrier imageBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        nullptr,
        0,                                                      // srcAccessMask
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, // dstAccessMask
        VK_IMAGE_LAYOUT_UNDEFINED,                              // oldLayout
        layout,                                                 // newLayout
        0,                       // srcQueueFamilyIndex
        0,                       // dstQueueFamilyIndex
        m_image->vulkan_image(), // image
        subresourceRange,        // subresourceRange
    };

    vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,              // dependencyFlags
                         0,              // memoryBarrierCount
                         nullptr,        // pMemoryBarriers
                         0,              // bufferMemoryBarrierCount
                         nullptr,        // pBufferMemoryBarriers
                         1,              // imageMemoryBarrierCount
                         &imageBarrier); // pImageMemoryBarriers

    // Set up a buffer->image copy to initialize the image contents.
    if (needs_copy) {
        uint32_t row_length = m_image->row_pitch() ? m_image->row_pitch() /
                                                         m_image->element_size()
                                                   : m_image->width();
        uint32_t image_height =
            m_image->slice_pitch()
                ? m_image->slice_pitch() / row_length / m_image->element_size()
                : m_image->height();
        uint32_t layer_count = 1;
        if ((m_image->type() == CL_MEM_OBJECT_IMAGE1D_ARRAY) ||
            (m_image->type() == CL_MEM_OBJECT_IMAGE2D_ARRAY)) {
            layer_count = m_image->array_size();
        }
        VkImageSubresourceLayers subresource = {
            VK_IMAGE_ASPECT_COLOR_BIT, // aspectMask
            0,                         // mipLevel
            0,                         // baseArrayLayer
            layer_count,               // layerCount
        };
        VkExtent3D extent;

        extent.width = m_image->width();
        extent.height = m_image->height();
        extent.depth = m_image->depth();

        switch (m_image->type()) {
        case CL_MEM_OBJECT_IMAGE2D:
        case CL_MEM_OBJECT_IMAGE2D_ARRAY:
            extent.depth = 1;
            break;
        case CL_MEM_OBJECT_IMAGE1D_BUFFER:
        case CL_MEM_OBJECT_IMAGE1D:
        case CL_MEM_OBJECT_IMAGE1D_ARRAY:
            extent.height = 1;
            extent.depth = 1;
            break;
        default:
            break;
        }

        VkBufferImageCopy copy = {
            0,            // bufferOffset
            row_length,   // bufferRowLength
            image_height, // bufferImageHeight
            subresource,  // imageSubresource
            {0, 0, 0},    // imageOffset
            extent,       // imageExtent
        };
        vkCmdCopyBufferToImage(cmdbuf, m_image->init_data()->vulkan_buffer(),
                               m_image->vulkan_image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Transition image layout to GENERAL.
        VkImageMemoryBarrier imageBarrier = {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT,         // srcAccessMask
            VK_ACCESS_MEMORY_READ_BIT,            // dstAccessMask
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // oldLayout
            VK_IMAGE_LAYOUT_GENERAL,              // newLayout
            0,                                    // srcQueueFamilyIndex
            0,                                    // dstQueueFamilyIndex
            m_image->vulkan_image(),              // image
            subresourceRange,                     // subresourceRange
        };
        vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0,              // dependencyFlags
                             0,              // memoryBarrierCount
                             nullptr,        // pMemoryBarriers
                             0,              // bufferMemoryBarrierCount
                             nullptr,        // pBufferMemoryBarriers
                             1,              // imageMemoryBarrierCount
                             &imageBarrier); // pImageMemoryBarriers
    }

    return CL_SUCCESS;
}
