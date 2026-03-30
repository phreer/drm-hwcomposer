# DRM Lease for Container Display Access

## Problem Statement

A Docker container running under Android needs to drive a physical display independently.
The DRM master is held by drm-hwcomposer (HWC). Since only one DRM master can exist per
device, the container cannot open `/dev/dri/card0` and become master.

## Solution: DRM Lease

The DRM lease API (Linux 4.15+, `drmModeCreateLease`) allows the DRM master (HWC) to
delegate a subset of KMS resources — one connector + one CRTC + its planes — to another
process. The lessee receives a new fd that behaves as a limited DRM master for those
resources only.

**Key properties:**
- HWC (lessor) retains master and continues driving non-leased displays normally
- The lessee gets full atomic KMS capability on its leased resources
- The kernel prevents the lessor from using leased resources
- Multiple leases can be active simultaneously (one per display)
- Lease terminates when: lessee closes its fd, lessor calls RevokeLease, or lessee exits

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Android Host                            │
│                                                              │
│  ┌────────────────────────────────────────────────────┐     │
│  │          drm-hwcomposer (DRM Master)                │     │
│  │                                                      │     │
│  │  DrmDevice ──fd──▶ /dev/dri/card0                   │     │
│  │    ├── Pipeline A (connector-1, crtc-0)  ◀── SF     │     │
│  │    ├── [connector-2: LEASED, skipped]               │     │
│  │    └── [connector-3: LEASED, skipped]               │     │
│  │                                                      │     │
│  │  DrmLeaseManager                                    │     │
│  │    ├── Lease 0: connector-2 → lease_fd_0            │     │
│  │    └── Lease 1: connector-3 → lease_fd_1            │     │
│  └──────────┬───────────────────────┬──────────────────┘     │
│             │ unix socket            │ unix socket             │
│  ┌──────────▼──────┐      ┌─────────▼──────────┐            │
│  │  Container A     │      │  Container B        │            │
│  │  (lease_fd_0)    │      │  (lease_fd_1)       │            │
│  │  → connector-2   │      │  → connector-3      │            │
│  └─────────────────┘      └────────────────────┘            │
└─────────────────────────────────────────────────────────────┘
```

## Configuration

Single Android property (parsed at HWC init):

```
# Format: connector_id:socket_path[,connector_id:socket_path,...]
vendor.hwc.drm.lease=52:/dev/socket/drm_lease_0,78:/dev/socket/drm_lease_1
```

- **`connector_id`**: The DRM connector ID (from `modetest -c` or `drm_info`)
- **`socket_path`**: Unix domain socket path; HWC creates it, container connects to it

The lease fd is delivered via `SCM_RIGHTS` (unix socket fd passing). Container connects to
the socket, receives the fd, then uses it for independent KMS atomic modesetting.

## Files Changed

| File | Action | Purpose |
|------|--------|---------|
| `drm/DrmLeaseManager.h` | **New** | Lease manager class declaration |
| `drm/DrmLeaseManager.cpp` | **New** | Lease manager implementation |
| `drm/DrmDevice.h` | **Modify** | Add `CreateLease()` / `RevokeLease()` method declarations |
| `drm/DrmDevice.cpp` | **Modify** | Implement lease ioctl wrappers |
| `drm/ResourceManager.h` | **Modify** | Add `DrmLeaseManager` member, expose accessor |
| `drm/ResourceManager.cpp` | **Modify** | Init lease manager; skip leased connectors in update loop |
| `utils/properties.h` | **Modify** | Add `GetDrmLease()` declaration |
| `utils/properties.cpp` | **Modify** | Implement `GetDrmLease()` |
| `Android.bp` | **Modify** | Add `DrmLeaseManager.cpp` to `drm_hwcomposer_common` filegroup |
| `drm/meson.build` | **Modify** | Add `DrmLeaseManager.cpp` to `src_common` |

## Detailed Design

### DrmDevice — Lease Ioctl Wrappers

```cpp
// In DrmDevice.h
struct DrmLease {
  int lease_fd;       // New DRM master fd for the lessee
  uint32_t lessee_id; // Kernel-assigned lessee ID (for revocation)
};

auto CreateLease(const std::vector<uint32_t> &object_ids, int flags)
    -> std::optional<DrmLease>;
auto RevokeLease(uint32_t lessee_id) -> int;
```

`CreateLease()` wraps `drmModeCreateLease()`. Returns `std::nullopt` on failure.
`RevokeLease()` wraps `drmModeRevokeLease()`. Returns 0 or negative errno.

### DrmLeaseManager

```cpp
class DrmLeaseManager {
 public:
  struct LeaseConfig {
    uint32_t connector_id;
    std::string socket_path;
  };

  struct ActiveLease {
    uint32_t lessee_id;
    uint32_t connector_id;
    UniqueFd2 lease_fd;
    std::string socket_path;
  };

  explicit DrmLeaseManager(ResourceManager &res_mgr);

  // Parse property, store configs (no ioctl yet)
  auto Init() -> int;

  // Create kernel leases; call after DrmDevices are fully initialized,
  // before UpdateFrontendDisplays()
  auto CreateLeases() -> int;

  // Query: should UpdateFrontendDisplays() skip this connector?
  auto IsConnectorLeased(uint32_t connector_id) const -> bool;

  // Revoke all active leases (called from ResourceManager destructor)
  void RevokeAll();
};
```

**Resource resolution** (`ResolveLeaseResources`):
1. Find `DrmConnector` by ID
2. Iterate encoders; pick first that the connector supports
3. Iterate CRTCs; pick first that the encoder supports
4. Collect ALL planes (primary + overlay + cursor) whose `possible_crtcs` includes the CRTC
5. Return flat `std::vector<uint32_t>`: `{connector_id, crtc_id, plane_id, ...}`

This runs at init, before any `PipelineBindable` binding. The kernel lease atomically
removes those resources from the lessor's visible set.

**Socket server thread**:
- For each active lease, bind a UNIX stream socket at `socket_path`
- Accept one client connection at a time
- Send `lease_fd` via `sendmsg()` + `SCM_RIGHTS`
- Log delivery; re-listen for reconnection (supports container restart)

### ResourceManager Integration

```cpp
// ResourceManager.h — new member
std::unique_ptr<DrmLeaseManager> lease_manager_;

// ResourceManager::Init() — after DrmDevice init, before UpdateFrontendDisplays():
lease_manager_ = std::make_unique<DrmLeaseManager>(*this);
lease_manager_->Init();
lease_manager_->CreateLeases();

// ResourceManager::UpdateFrontendDisplays() — connector loop, first line:
if (lease_manager_ && lease_manager_->IsConnectorLeased(conn->GetId()))
  continue;
```

Leased connectors are never offered to `DrmHwc` / SurfaceFlinger — they are invisible to
the Android display stack from the very first enumeration.

## Boot Sequence

```
ResourceManager::Init()
  ├── DrmDevice::Init() for each /dev/dri/card*
  │     └── drmSetMaster() — HWC acquires DRM master
  ├── DrmLeaseManager::Init()           # parse vendor.hwc.drm.lease property
  ├── DrmLeaseManager::CreateLeases()   # drmModeCreateLease() for each config
  │     └── Start socket server thread  # waits for container connections
  └── UpdateFrontendDisplays()          # leased connectors skipped here
        └── SurfaceFlinger sees only non-leased displays

Container starts:
  └── connect(/dev/socket/drm_lease_N)  # block until HWC is ready
        └── recv SCM_RIGHTS             # get lease_fd
              └── use lease_fd for KMS atomic modesetting

Shutdown:
  ResourceManager::~ResourceManager()
    └── DrmLeaseManager::RevokeAll()    # drmModeRevokeLease() for each lessee_id
          └── Container lease_fd becomes invalid
```

## Known Limitations

1. **Shared hardware resources**: PLLs, memory bandwidth, and clock resources are not
   tracked by the lease mechanism. Two masters doing independent modesets on different
   displays sharing the same PLL may fail unpredictably. Use `DRM_MODE_ATOMIC_TEST_ONLY`
   inside the container before committing.

2. **No sub-leasing**: The container (lessee) cannot further subdivide its resources.
   Only the DRM master (HWC) can create leases.

3. **No lease modification**: To change leased resources, the lease must be revoked and
   re-created. This is a kernel limitation.

4. **Connector IDs are stable per boot but may change across reboots** on some drivers.
   If this is a concern, add a resolution-by-connector-type-and-index alternative.

5. **Container requires a DRM compositor**: The container's display stack must be able to
   do KMS atomic modesetting directly (e.g., a wlroots-based compositor, custom KMS app,
   or a minimal Android HWC instance using the lease fd as its DRM device fd).

## Container-Side Usage

The container receives a DRM master fd and uses it for normal KMS operations:

```c
// Connect to socket and receive lease fd
int sock = socket(AF_UNIX, SOCK_STREAM, 0);
connect(sock, (struct sockaddr *)&addr, sizeof(addr));
int lease_fd = recv_fd(sock);  // SCM_RIGHTS helper

// Use lease_fd exactly like a normal DRM device fd
drmModeRes *res = drmModeGetResources(lease_fd);
// ... set mode, create framebuffers, do atomic commits ...
drmModeAtomicCommit(lease_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
```

The lease fd behaves as a DRM master for exactly the leased resources. The container can
query which objects it has access to with `drmModeGetLease(lease_fd, ...)`.
