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

#define LOG_TAG "drmhwc"

#include "DrmLeaseManager.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <xf86drm.h>

#include <cerrno>
#include <cstring>
#include <sstream>

#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmPlane.h"
#include "drm/ResourceManager.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android {

DrmLeaseManager::DrmLeaseManager(ResourceManager& res_mgr) : res_mgr_(res_mgr) {
}

DrmLeaseManager::~DrmLeaseManager() {
  RevokeAll();
}

auto DrmLeaseManager::Init() -> int {
  char prop[PROPERTY_VALUE_MAX];
  // Try the persistent property first, fallback to the non-persistent one.
  property_get("persist.vendor.hwc.drm.lease", prop, "");
  if (prop[0] == '\0') {
    property_get("vendor.hwc.drm.lease", prop, "");
  }

  if (prop[0] == '\0') {
    ALOGD("No DRM leases configured (persist.vendor.hwc.drm.lease not set)");
    return 0;
  }

  // Parse comma-separated "connector:socket_path" entries.
  std::istringstream stream(prop);
  std::string entry;
  while (std::getline(stream, entry, ',')) {
    auto sep = entry.find(':');
    if (sep == std::string::npos || sep == 0 || sep + 1 == entry.size()) {
      ALOGE("Invalid lease entry (expected connector_name:socket_path): '%s'",
            entry.c_str());
      return -EINVAL;
    }

    std::string conn_str = entry.substr(0, sep);
    std::string socket_path = entry.substr(sep + 1);

    LeaseConfig cfg;
    cfg.socket_path = socket_path;
    cfg.connector_name = conn_str;

    ALOGI("DRM lease config: connector name '%s' → socket %s",
          cfg.connector_name.c_str(), cfg.socket_path.c_str());

    lease_configs_.push_back(std::move(cfg));
  }

  // Resolve connector names to IDs now so that IsConnectorLeased() can filter
  // connectors during UpdateFrontendDisplays(), which runs before
  // CreateLeases().  CreateLeases() still needs UpdateFrontendDisplays() to
  // have run first so that CanBind() reflects already-reserved CRTCs.
  for (const auto& cfg : lease_configs_) {
    for (auto& drm : res_mgr_.GetDrmDevices()) {
      for (const auto& conn : drm->GetConnectors()) {
        if (conn->GetName() == cfg.connector_name) {
          leased_connector_ids_.insert(conn->GetId());
          break;
        }
      }
    }
  }

  return 0;
}

auto DrmLeaseManager::IsConnectorLeased(uint32_t connector_id) const -> bool {
  return leased_connector_ids_.count(connector_id) != 0;
}

auto DrmLeaseManager::ResolveLeaseResources(DrmDevice& dev,
                                            uint32_t connector_id)
    -> std::optional<std::vector<uint32_t>> {
  // 1. Find the connector.
  DrmConnector* target_conn = nullptr;
  for (const auto& conn : dev.GetConnectors()) {
    if (conn->GetId() == connector_id) {
      target_conn = conn.get();
      break;
    }
  }
  if (target_conn == nullptr) {
    ALOGE("Lease: connector %u not found in DRM device", connector_id);
    return {};
  }

  // 2. Find a CRTC using the same strategy as
  // DrmDisplayPipeline::CreatePipeline:
  //    first try the connector's current encoder + its current CRTC (avoids
  //    disturbing an already-active display), then fall back to any free CRTC
  //    that passes CanBind() (i.e. not already claimed by another HWC
  //    pipeline). This must be called after UpdateFrontendDisplays() so that
  //    CanBind() reflects CRTCs already reserved by other connectors.
  DrmCrtc* target_crtc = nullptr;

  auto try_encoder = [&](DrmEncoder& enc) -> bool {
    // Prefer the CRTC the encoder is currently driving in hardware.
    auto* current_crtc = dev.FindCrtcById(enc.GetCurrentCrtcId());
    if (current_crtc != nullptr && current_crtc->CanBind(connector_id)) {
      target_crtc = current_crtc;
      return true;
    }
    // Fall back: any CRTC compatible with this encoder that is not yet claimed.
    for (const auto& crtc : dev.GetCrtcs()) {
      if (enc.SupportsCrtc(*crtc) && crtc->CanBind(connector_id)) {
        target_crtc = crtc.get();
        return true;
      }
    }
    return false;
  };

  // Try the connector's currently-active encoder first.
  auto* current_enc = dev.FindEncoderById(target_conn->GetCurrentEncoderId());
  if (current_enc != nullptr && target_conn->SupportsEncoder(*current_enc)) {
    try_encoder(*current_enc);
  }
  // If that didn't work, iterate all compatible encoders.
  if (target_crtc == nullptr) {
    for (const auto& enc : dev.GetEncoders()) {
      if (target_conn->SupportsEncoder(*enc) && try_encoder(*enc)) {
        break;
      }
    }
  }

  if (target_crtc == nullptr) {
    ALOGE("Lease: no free CRTC found for connector %u", connector_id);
    return {};
  }

  ALOGI("Lease: connector %u → crtc %u", connector_id, target_crtc->GetId());

  // 3. Collect all planes that support this CRTC (primary + overlay + cursor).
  std::vector<uint32_t> objects = {connector_id, target_crtc->GetId()};
  for (const auto& plane : dev.GetPlanes()) {
    if (plane->IsCrtcSupported(*target_crtc)) {
      objects.push_back(plane->GetId());
      ALOGD("Lease: adding plane %u", plane->GetId());
    }
  }

  return objects;
}

auto DrmLeaseManager::CreateLeases() -> int {
  if (lease_configs_.empty()) {
    return 0;
  }

  for (const auto& cfg : lease_configs_) {
    // Find the DRM device that owns this connector.
    DrmDevice* owner_dev = nullptr;
    uint32_t connector_id = 0;
    for (auto& drm : res_mgr_.GetDrmDevices()) {
      for (const auto& conn : drm->GetConnectors()) {
        if (conn->GetName() == cfg.connector_name) {
          owner_dev = drm.get();
          connector_id = conn->GetId();
          break;
        }
      }
      if (owner_dev != nullptr) {
        break;
      }
    }

    if (owner_dev == nullptr) {
      ALOGE("Lease: connector %s not found on any DRM device",
            cfg.connector_name.c_str());
      continue;
    }

    auto objects = ResolveLeaseResources(*owner_dev, connector_id);
    if (!objects) {
      ALOGE("Lease: failed to resolve resources for connector %s",
            cfg.connector_name.c_str());
      continue;
    }

    uint32_t lessee_id = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int lease_fd = drmModeCreateLease(*owner_dev->GetFd(), objects->data(),
                                      static_cast<int>(objects->size()),
                                      O_CLOEXEC, &lessee_id);
    if (lease_fd < 0) {
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      ALOGE("Lease: drmModeCreateLease failed for connector %s: %s",
            cfg.connector_name.c_str(), strerror(errno));
      continue;
    }

    ALOGI("Lease: created lease %u for connector %s (id=%u, fd=%d)", lessee_id,
          cfg.connector_name.c_str(), connector_id, lease_fd);

    ActiveLease lease;
    lease.lessee_id = lessee_id;
    lease.connector_id = connector_id;
    lease.lease_fd = UniqueFd2(lease_fd);
    lease.socket_path = cfg.socket_path;
    active_leases_.push_back(std::move(lease));
  }

  if (!active_leases_.empty()) {
    stop_thread_ = false;
    socket_thread_ = std::thread(&DrmLeaseManager::SocketServerThread, this);
  }

  return 0;
}

// static
auto DrmLeaseManager::SendFdOverSocket(int sock_fd, int fd_to_send) -> bool {
  // Send a single byte of data alongside the fd via SCM_RIGHTS.
  char data = '\0';
  struct iovec iov = {.iov_base = &data, .iov_len = sizeof(data)};

  alignas(struct cmsghdr) char ctrl_buf[CMSG_SPACE(sizeof(int))];
  memset(ctrl_buf, 0, sizeof(ctrl_buf));

  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl_buf;
  msg.msg_controllen = sizeof(ctrl_buf);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

  return sendmsg(sock_fd, &msg, 0) >= 0;
}

void DrmLeaseManager::SocketServerThread() {
  // For each active lease, create a listening unix socket, accept connections
  // in round-robin, and deliver the lease fd.  Re-listen after disconnection
  // so container restarts are handled gracefully.
  struct SocketEntry {
    int listen_fd;  // listening socket (-1 if setup failed)
    size_t lease_idx;
  };

  std::vector<SocketEntry> entries;
  entries.reserve(active_leases_.size());

  for (size_t i = 0; i < active_leases_.size(); ++i) {
    const auto& lease = active_leases_[i];

    UniqueFd2 listen_fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listen_fd) {
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      ALOGE("Lease socket: socket() failed for %s: %s",
            lease.socket_path.c_str(), strerror(errno));
      entries.push_back({-1, i});
      continue;
    }

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, lease.socket_path.c_str(),
            sizeof(addr.sun_path) - 1);

    // Remove stale socket file.
    unlink(lease.socket_path.c_str());

    if (bind(listen_fd.Get(), reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      ALOGE("Lease socket: bind() failed for %s: %s", lease.socket_path.c_str(),
            strerror(errno));
      entries.push_back({-1, i});
      continue;
    }

    // Allow the container user to connect.
    chmod(lease.socket_path.c_str(), 0660);

    if (listen(listen_fd.Get(), 1) < 0) {
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      ALOGE("Lease socket: listen() failed for %s: %s",
            lease.socket_path.c_str(), strerror(errno));
      entries.push_back({-1, i});
      continue;
    }

    ALOGI("Lease socket: listening on %s for connector %u",
          lease.socket_path.c_str(), lease.connector_id);
    entries.push_back({listen_fd.Release(), i});
  }

  // Use select() to wait for any socket to become readable, then accept and
  // send the lease fd.  Loop until stop_thread_ is set.
  while (!stop_thread_) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    int max_fd = -1;

    for (auto& entry : entries) {
      if (entry.listen_fd >= 0) {
        FD_SET(entry.listen_fd, &read_fds);
        max_fd = std::max(max_fd, entry.listen_fd);
      }
    }

    if (max_fd < 0) {
      break;  // All sockets failed to initialise.
    }

    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      // NOLINTNEXTLINE(concurrency-mt-unsafe)
      ALOGE("Lease socket: select() error: %s", strerror(errno));
      break;
    }

    for (auto& entry : entries) {
      if (entry.listen_fd < 0 || !FD_ISSET(entry.listen_fd, &read_fds)) {
        continue;
      }

      int client_fd = accept(entry.listen_fd, nullptr, nullptr);
      if (client_fd < 0) {
        continue;
      }

      const auto& lease = active_leases_[entry.lease_idx];
      ALOGI("Lease socket: client connected on %s (connector %u)",
            lease.socket_path.c_str(), lease.connector_id);

      if (SendFdOverSocket(client_fd, lease.lease_fd.Get())) {
        ALOGI("Lease socket: lease fd delivered for connector %u",
              lease.connector_id);
      } else {
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        ALOGE("Lease socket: failed to send fd for connector %u: %s",
              lease.connector_id, strerror(errno));
      }

      // Keep the connection open; close it on next iteration or when the
      // container closes it.  We do not track client_fd here intentionally:
      // the container holds the lease fd, not this connection.
      close(client_fd);
    }
  }

  // Cleanup listening sockets.
  for (auto& entry : entries) {
    if (entry.listen_fd >= 0) {
      close(entry.listen_fd);
      unlink(active_leases_[entry.lease_idx].socket_path.c_str());
    }
  }
}

void DrmLeaseManager::RevokeAll() {
  stop_thread_ = true;
  if (socket_thread_.joinable()) {
    socket_thread_.join();
  }

  for (auto& lease : active_leases_) {
    ALOGI("Lease: revoking lease %u (connector %u)", lease.lessee_id,
          lease.connector_id);

    // Find the owning DrmDevice to call RevokeLease on the master fd.
    for (auto& drm : res_mgr_.GetDrmDevices()) {
      for (const auto& conn : drm->GetConnectors()) {
        if (conn->GetId() == lease.connector_id) {
          drmModeRevokeLease(*drm->GetFd(), lease.lessee_id);
          break;
        }
      }
    }
  }

  active_leases_.clear();
}

}  // namespace android
