/**
 * SPDX-FileCopyrightText: Copyright (c) 2022-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "internal/runtime/worker_manager.hpp"

#include "internal/control_plane/state/root_state.hpp"
#include "internal/runtime/data_plane_manager.hpp"
#include "internal/runtime/partition_runtime.hpp"
#include "internal/runtime/resource_manager_base.hpp"
#include "internal/runtime/segments_manager.hpp"

#include "mrc/exceptions/runtime_error.hpp"
#include "mrc/types.hpp"
#include "mrc/utils/string_utils.hpp"

#include <glog/logging.h>
#include <rxcpp/rx.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <sstream>

namespace mrc::runtime {

WorkerManager::WorkerManager(PartitionRuntime& runtime, InstanceID worker_id) :
  PartitionResourceManager(runtime,
                           worker_id,
                           MRC_CONCAT_STR("WorkerManager[" << runtime.partition_id() << "/" << worker_id << "]")),
  m_partition_id(runtime.partition_id()),
  m_worker_id(worker_id)
{}

WorkerManager::~WorkerManager()
{
    PartitionResourceManager::call_in_destructor();
}

DataPlaneManager& WorkerManager::data_plane() const
{
    CHECK(m_data_plane_manager) << "The WorkerManager must be started before using the data_plane()";

    return *m_data_plane_manager;
}

control_plane::state::Worker WorkerManager::filter_resource(const control_plane::state::ControlPlaneState& state) const
{
    if (!state.workers().contains(this->id()))
    {
        throw exceptions::MrcRuntimeError(MRC_CONCAT_STR("Could not find Worker with ID: " << this->id()));
    }
    return state.workers().at(this->id());
}

bool WorkerManager::on_created_requested(control_plane::state::Worker& instance, bool needs_local_update)
{
    if (needs_local_update)
    {
        // Create the data plane manager
        m_data_plane_manager = std::make_shared<DataPlaneManager>(*this, m_partition_id);

        this->child_service_start(m_data_plane_manager, true);

        // Create the segment manager
        m_segments_manager = std::make_shared<SegmentsManager>(*this, m_partition_id);

        this->child_service_start(m_segments_manager, true);
    }

    return true;
}

void WorkerManager::on_completed_requested(control_plane::state::Worker& instance)
{
    // // Activate our worker
    // protos::RegisterWorkersResponse resp;
    // resp.set_machine_id(0);
    // resp.add_instance_ids(m_worker_id);

    // // Need to activate our worker
    // this->runtime().control_plane().await_unary<protos::Ack>(protos::ClientUnaryActivateStream, std::move(resp));
}

void WorkerManager::on_running_state_updated(control_plane::state::Worker& instance)
{
    m_data_plane_manager->sync_state(instance);

    // Returns true if all segments have been removed (and none starting)
    if (m_segments_manager->sync_state(instance))
    {
        // See if our stop condition is met. Wait until a pipeline mapping has been applied
        if (!instance.executor().mapped_pipeline_definitions().empty() && instance.assigned_segments().empty() &&
            this->get_local_actual_status() < control_plane::state::ResourceActualStatus::Completed)
        {
            // If all manifolds and segments have been removed, we can mark ourselves as completed
            this->mark_completed();
        }
    }
}

void WorkerManager::on_stopped_requested(control_plane::state::Worker& instance)
{
    this->service_stop();
}

}  // namespace mrc::runtime
