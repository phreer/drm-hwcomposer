/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "utils/UniqueFd2.h"

namespace android {

class DrmDevice;
class ResourceManager;

/**
 * DrmLeaseManager creates and manages DRM leases that delegate one or more
 * displays to external processes (e.g., Docker containers).
 *
 * Configuration is read from the Android property:
 *   persist.vendor.hwc.drm.lease=<connector_name>:<socket_path>[,<connector_name>:<socket_path>,...]
 *
 * Example:
 *   persist.vendor.hwc.drm.lease=DP-1:/dev/socket/drm_lease_1
 *
 * For each configured connector the manager:
 *  1. Resolves the associated CRTC and all its planes.
 *  2. Calls drmModeCreateLease() to obtain a limited DRM-master fd.
 *  3. Listens on a Unix domain socket and delivers the lease fd via SCM_RIGHTS
 *     to the first client that connects.  It re-listens after disconnection so
 *     the container can reconnect after a restart.
 *
 * Leased connectors are hidden from SurfaceFlinger: ResourceManager skips them
 * during UpdateFrontendDisplays().
 */
class DrmLeaseManager {
 public:
  struct LeaseConfig {
    std::string connector_name;
    std::string socket_path;
  };

  struct ActiveLease {
    uint32_t lessee_id;
    uint32_t connector_id;
    UniqueFd2 lease_fd;
    std::string socket_path;
  };

  explicit DrmLeaseManager(ResourceManager& res_mgr);

  DrmLeaseManager(const DrmLeaseManager&) = delete;
  DrmLeaseManager& operator=(const DrmLeaseManager&) = delete;

  ~DrmLeaseManager();

  // Parse the vendor.hwc.drm.lease property and store lease configs.
  // No DRM ioctls are issued here.
  auto Init() -> int;

  // Create kernel leases for all configured connectors.  Must be called after
  // all DrmDevices have completed Init(), before UpdateFrontendDisplays().
  auto CreateLeases() -> int;

  // Returns true when the given connector ID is reserved for a lease and
  // should be excluded from HWC pipeline management.
  auto IsConnectorLeased(uint32_t connector_id) const -> bool;

  // Reconcile active leases with the current connector topology after hotplug.
  auto ReconcileLeases() -> int;

  // Revoke all active leases and stop the socket server thread.
  void RevokeAll();

 private:
  // Resolve {connector_id, crtc_id, plane_id, ...} for the given connector.
  auto ResolveLeaseResources(DrmDevice& dev, uint32_t connector_id)
      -> std::optional<std::vector<uint32_t>>;

  void RefreshConfiguredConnectorIds();

  // Background thread: for each active lease, listen on its socket path and
  // deliver the lease fd via SCM_RIGHTS.  Reconnections are supported.
  void SocketServerThread();

  // Send a single fd over a connected unix socket using SCM_RIGHTS.
  static auto SendFdOverSocket(int sock_fd, int fd_to_send) -> bool;

  ResourceManager& res_mgr_;
  std::vector<LeaseConfig> lease_configs_;
  std::set<uint32_t> configured_connector_ids_;
  std::vector<ActiveLease> active_leases_;
  bool hide_configured_connectors_{true};

  std::thread socket_thread_;
  bool stop_thread_{};
};

}  // namespace android
