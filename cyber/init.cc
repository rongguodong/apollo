/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
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
 *****************************************************************************/

#include "cyber/init.h"

#include <libgen.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

#include "cyber/binary.h"
#include "cyber/common/global_data.h"
#include "cyber/cyber.h"
#include "cyber/data/data_dispatcher.h"
#include "cyber/logger/async_logger.h"
#include "cyber/proto/clock.pb.h"
#include "cyber/scheduler/scheduler.h"
#include "cyber/service_discovery/topology_manager.h"
#include "cyber/sysmo/sysmo.h"
#include "cyber/task/task.h"
#include "cyber/timer/timing_wheel.h"
#include "cyber/transport/transport.h"

namespace apollo {
namespace cyber {

using apollo::cyber::scheduler::Scheduler;
using apollo::cyber::service_discovery::TopologyManager;

namespace {

bool g_atexit_registered = false;
std::mutex g_mutex;
logger::AsyncLogger* async_logger = nullptr;

std::unique_ptr<apollo::cyber::Node> clock_node;

void InitLogger(const char* binary_name) {
  const char* slash = strrchr(binary_name, '/');
  if (slash) {
    ::apollo::cyber::Binary::SetName(slash + 1);
  } else {
    ::apollo::cyber::Binary::SetName(binary_name);
  }

  // Init glog
  google::InitGoogleLogging(binary_name);
  google::SetLogDestination(google::ERROR, "");
  google::SetLogDestination(google::WARNING, "");
  google::SetLogDestination(google::FATAL, "");

  // Init async logger
  async_logger = new ::apollo::cyber::logger::AsyncLogger(
      google::base::GetLogger(FLAGS_minloglevel));
  google::base::SetLogger(FLAGS_minloglevel, async_logger);
  async_logger->Start();
}

void StopLogger() { delete async_logger; }

}  // namespace

void OnShutdown(int sig) {
  (void)sig;
  if (GetState() != STATE_SHUTDOWN) {
    SetState(STATE_SHUTTING_DOWN);
  }
}

void ExitHandle() { Clear(); }

bool Init(const char* binary_name) {
  std::lock_guard<std::mutex> lg(g_mutex);
  if (GetState() != STATE_UNINITIALIZED) {
    return false;
  }

  InitLogger(binary_name);
  auto thread = const_cast<std::thread*>(async_logger->LogThread());
  scheduler::Instance()->SetInnerThreadAttr("async_log", thread);
  SysMo::Instance();
  std::signal(SIGINT, OnShutdown);
  // Register exit handlers
  if (!g_atexit_registered) {
    if (std::atexit(ExitHandle) != 0) {
      AERROR << "Register exit handle failed";
      return false;
    }
    AINFO << "Register exit handle succ.";
    g_atexit_registered = true;
  }
  SetState(STATE_INITIALIZED);

  const char* use_sim_time = ::getenv("USE_SIM_TIME");
  if (use_sim_time != nullptr) {
    std::string use_sim_time_str(use_sim_time);
    if (use_sim_time_str == "true") {
      clock_node = apollo::cyber::CreateNode("cyber_clock_node" +
                                             std::to_string(getpid()));

      auto cb =
          [](const std::shared_ptr<const apollo::cyber::proto::Clock>& msg) {
            if (msg->has_clock()) {
              Time::SetSimTime(Time(msg->clock()));
            }
          };

      clock_node->CreateReader<apollo::cyber::proto::Clock>("/clock", cb);
    }
  }
  return true;
}

void Clear() {
  std::lock_guard<std::mutex> lg(g_mutex);
  if (GetState() == STATE_SHUTDOWN || GetState() == STATE_UNINITIALIZED) {
    return;
  }
  SysMo::CleanUp();
  TaskManager::CleanUp();
  TimingWheel::CleanUp();
  scheduler::CleanUp();
  service_discovery::TopologyManager::CleanUp();
  transport::Transport::CleanUp();
  StopLogger();
  SetState(STATE_SHUTDOWN);
}

}  // namespace cyber
}  // namespace apollo
