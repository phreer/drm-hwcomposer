# AGENTS.md — drm-hwcomposer

> Universal HW Composer for Android using Linux DRM/KMS.
> Apache 2.0 licensed. Hosted on GitLab freedesktop.org.

## Build Systems

Three build systems coexist:

### Android (AOSP) Build — Primary

```bash
m android.hardware.composer.hwc3-service.drm   # HWC3 service binary
m com.android.hardware.graphics.composer.drm_hwcomposer  # APEX package
```

### Meson — Cross-compilation (aospless)

```bash
make -C ~/aospless install    # Full cross-build + install for arm64
make -C ~/aospless all        # Build only
```

### Docker Dev Workflow (top-level Makefile)

```bash
make prepare       # Build Docker image and start container
make shell         # Open shell in Docker container
make ci            # Full presubmit: native build + meson cross-build + style check
make ci_fast       # Quick meson cross-build only (arm64)
make ci_cleanup    # Clean build artifacts in Docker
make bd            # Build and deploy to connected adb device (HWCLOG=1 for logcat)
```

### Native Build + Clang-Tidy (inside Docker/CI container)

```bash
make -f .ci/Makefile           # Build + tidy (all)
make -f .ci/Makefile build     # Native build only
make -f .ci/Makefile tidy      # Clang-tidy only
```

Uses `clang++-19` / `clang-tidy-19`. Flags: `-Wall -Wextra -Werror`, `gnu++17`, no RTTI.

## Tests

**No automated unit or integration tests.** Quality is enforced via:
1. **Build verification** — CI cross-compiles for arm64
2. **Static analysis** — clang-tidy on every source file (three strictness tiers)
3. **Style checks** — commit message format, clang-format, bpfmt

Manual on-device tools exist (`hwc-drm-uevent-print`, `hwcservice_test`) but are not automated.

## Style & Formatting Checks

```bash
git diff -U0 HEAD | clang-format-diff-19 -p 1 -style=file   # Check formatting
find -name "*.bp" -exec bpfmt -d -s {} \;                   # Check Blueprint files
```

### Commit Message Requirements (enforced by CI)

- Subject must start with `drm_hwcomposer:` or `Revert`
- Both author and committer must have `Signed-off-by:` tags

## Code Style

Based on **Google C++ Style** with project-specific overrides.
C++17 (`gnu++17`), no RTTI, no exceptions.

### Naming Conventions (enforced by `.clang-tidy`)

| Element               | Convention          | Example                            |
|-----------------------|---------------------|------------------------------------|
| Classes / Structs     | `CamelCase`         | `DrmPlane`, `HwcDisplay`           |
| Functions / Methods   | `CamelCase`         | `AtomicSetState()`, `Init()`       |
| Variables / Params    | `lower_case`        | `plane_id`, `client_start`         |
| Private members       | `lower_case_`       | `pipeline_`, `fd_`                 |
| Public struct fields  | `lower_case`        | `width`, `format`, `fb`            |
| Constants / constexpr | `kCamelCase`        | `kEmptyFd`, `kAlphaOpaque`         |
| Enum values           | `kCamelCase`        | `kNone`, `kConnected`              |
| Macros                | `UPPER_CASE`        | `LOG_TAG`, `GUARDED_BY`            |
| Namespaces            | `lower_case`        | `android`, `hwcomposer`            |
| Type aliases          | `CamelCase`         | `SharedFd`, `UniqueFd`             |
| File names (class)    | `CamelCase`         | `DrmPlane.h`, `DrmPlane.cpp`       |
| File names (utility)  | `lowercase`         | `fd.h`, `log.h`, `properties.cpp`  |

### Header Guards

Use `#pragma once` for all new files.

### Include Ordering

Manually ordered (`IncludeBlocks: Preserve`). Follow this convention:

```cpp
#define LOG_TAG "drmhwc"          // 1. LOG_TAG if needed (before all includes)

#include "OwnHeader.h"            // 2. Own header (.cpp files)

#include <sys/stat.h>             // 3. System / C headers
#include <algorithm>              // 4. C++ standard library

#include <cutils/native_handle.h> // 5. Android / external framework headers

#include "drm/DrmDevice.h"        // 6. Project headers (relative paths, "quotes")
#include "utils/log.h"
```

Separate groups with blank lines. `<>` for system/external, `""` for project-local.

### Error Handling

No exceptions. Use these patterns in order of preference:

1. **`std::optional<T>`** — fallible getters returning values
2. **Negative errno** (`return -EINVAL`) — DRM/kernel layer functions
3. **`HWC2::Error`** enum — HWC2 API layer
4. **`nullptr` / empty `unique_ptr`** — factory methods that can fail
5. **`bool`** — simple success/failure
6. **Custom enums** (`ConfigError`) — domain-specific error codes

### Common Patterns

**Factory methods** — static `CreateInstance()` returning `unique_ptr`:
```cpp
static auto CreateInstance(DrmDevice &dev, uint32_t plane_id)
    -> std::unique_ptr<DrmPlane>;
```

**Trailing return types** — preferred for complex returns:
```cpp
auto GetOrderedConnectors() -> std::vector<DrmConnector *>;
```

**Smart pointers / RAII** — `unique_ptr`, `shared_ptr`, custom RAII wrappers
(`DrmModePlaneUnique`, `UniqueFd2`, `SharedFd`). Never use manual resource management.

**Thread safety annotations** — Clang annotations from `utils/thread_annotations.h`:
```cpp
bool enabled_ GUARDED_BY(mutex_);
void DoWork() REQUIRES(mutex_);
```

**Deleted copy** — resource-managing classes delete copy operations:
```cpp
DrmPlane(const DrmPlane &) = delete;
DrmPlane &operator=(const DrmPlane &) = delete;
```

**Logging** — Android `ALOG*` macros (`ALOGE`, `ALOGW`, `ALOGI`, `ALOGD`, `ALOGV`).
Define `LOG_TAG` at top of `.cpp` files before includes.

**Suppressing clang-tidy** — use `NOLINTNEXTLINE` with the specific check name:
```cpp
// NOLINTNEXTLINE(readability-identifier-naming)
struct drm_plane_size_hint_local { ... };
```

### License Header

Every source file must begin with:
```cpp
/*
 * Copyright (C) <YEAR> The Android Open Source Project
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
```

## Project Structure

| Directory        | Purpose                                              |
|------------------|------------------------------------------------------|
| `backend/`       | Composition backends (generic, client-composited)    |
| `bufferinfo/`    | Buffer info extraction; `legacy/` has vendor impls   |
| `compositor/`    | Composition planning, layer data types               |
| `drm/`           | DRM/KMS abstraction (devices, connectors, planes)    |
| `hwc2_device/`   | HWC2 HAL implementation                              |
| `hwc3/`          | HWC3 AIDL implementation + service entry point       |
| `libhwcservice/` | Intel HWC service library (binder interfaces)        |
| `utils/`         | Fd wrappers, logging, properties, thread annotations |
| `tests/`         | Mock headers and manual on-device debugging tools    |
| `.ci/`           | CI: Dockerfile, Makefiles, scripts, container defs   |
