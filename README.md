# NVIDIA Overlay Shared-Memory Composition Framework

High-performance Direct3D 11 in-memory texture injection and IPC composition library for Windows x64.

## Overview

The NVIDIA in-game overlay subsystem (integrated within NVIDIA Share and GeForce Experience) utilizes named shared-memory file mappings to exchange graphical slots between target application processes and the desktop composition engine (`nvcontainer.exe` / `nvsphelper64.exe`). Each slot carries metadata including texture dimension, pixel format, coordinate placement, update counters, and an encoded DXGI shared-resource handle.

This framework demonstrates the reverse-engineered IPC protocol governing these shared mappings. By opening the target process's overlay section with read/write access, a companion process can publish an out-of-process Direct3D 11 shared texture directly into NVIDIA's compositor queue. The overlay engine composites the texture over the target window without injecting dynamic libraries (`DLLs`), creating top-most transparent windows, or modifying the target application's render loop.

## Technical Specifications

- **Target Architecture**: x86_64
- **Operating Systems**: Windows 10 / 11
- **Graphics Pipeline**: Direct3D 11 (`d3d11.dll`), DXGI (`dxgi.dll`), Dear ImGui
- **Inter-Process Communication**: Win32 Named File Mappings (`OpenFileMappingW` / `MapViewOfFile`)
- **Target Subsystem**: NVIDIA GeForce Experience Overlay / ShadowPlay Compositor

## Shared Section Layout & Data Structures

```text
+-----------------------+------------------+------------------------------------------+
| Section Field Offset  | Size (Bytes)     | Description                              |
+-----------------------+------------------+------------------------------------------+
| Header + 0x0000       | 4                | Protocol Version (e.g., 0x000B0280)      |
| Header + 0x0004       | 4                | Target Process ID (PID)                  |
| Header + 0x0008       | 4                | Total Slot Count Allocation              |
| Header + 0x000C       | 4                | Active Frame Update Counter              |
| Slot[N] + 0x0000      | 8                | Encoded DXGI Shared Texture Token        |
| Slot[N] + 0x0008      | 4                | Texture Width (Pixels)                   |
| Slot[N] + 0x000C      | 4                | Texture Height (Pixels)                  |
| Slot[N] + 0x0010      | 4                | DXGI Surface Format (e.g., B8G8R8A8)     |
| Slot[N] + 0x0014      | 16               | Normalized Placement Rect (Screen BBox)  |
| Slot[N] + 0x0024      | 4                | Slot Revision / Dirty Sequence Counter   |
+-----------------------+------------------+------------------------------------------+
```

## Cryptographic Token Mask & Session Key Derivation

NVIDIA encodes the 64-bit DXGI shared texture handle before publishing it to shared memory to prevent naive cross-process handle reuse:

$$\text{EncodedToken} = \text{SharedTextureHandle} \oplus \text{ResourceTokenMask}(K_{session}, \text{Seed}_{32})$$

### 1. Mask Transformation Algorithm

The mask derivation relies on a multi-stage avalanche permutation using 64-bit integer constants:

```cpp
std::uint64_t resource_token_mask(std::uint64_t key, std::uint32_t seed) {
    constexpr std::uint64_t c1 = 0xFF51AFD7ED558CCDull;
    constexpr std::uint64_t cx = 0xED77E7F1C90AA277ull;
    constexpr std::uint64_t c2 = 0xC4CEB9FE1A85EC53ull;

    const std::uint64_t repeated = static_cast<std::uint64_t>(seed) |
        (static_cast<std::uint64_t>(seed) << 32);

    std::uint64_t value = key ^ repeated;
    value = (value ^ (value >> 33)) * c1;
    value = (value ^ (value >> 33)) * cx;
    value = (value ^ (value >> 33)) * c2;
    return value ^ (value >> 33);
}
```

### 2. Session Key Recovery via Side-Channel

During diagnostic logging phases, NVIDIA's compositor outputs the decoded handle to `CaptureCore.log`. Having both the `EncodedToken` extracted from the shared memory mapping and the `DecodedHandle` from diagnostic telemetry allows direct mask recovery:

$$\text{Mask} = \text{EncodedToken} \oplus \text{DecodedHandle}$$

Because the permutation uses reversible arithmetic operations (multiplicative inverses modulo $2^{64}$ and bitwise shifts), the session key $K_{session}$ can be reconstructed deterministically.

## Execution Flow & Lifecycle

```text
[ Injector / Companion Process ]             [ NVIDIA Overlay Compositor ]
             │                                            │
             ├─► 1. Open Target Process Handle            │
             │   (PROCESS_QUERY_LIMITED_INFORMATION)      │
             │                                            │
             ├─► 2. Open Named Shared Memory Mapping ─────┤ (Section created per target PID)
             │   MapViewOfFile(FILE_MAP_ALL_ACCESS)       │
             │                                            │
             ├─► 3. Initialize D3D11 Shared Texture       │
             │   CreateTexture2D(D3D11_RESOURCE_MISC_SHARED)
             │   IDXGIResource::GetSharedHandle           │
             │                                            │
             ├─► 4. Encode Token & Write Slot Data ───────► (Overwrites Slot Buffer)
             │   Increment Slot Sequence Counter          │
             │                                            │
             ├─► 5. Render Frame (D3D11 / ImGui)          ├─► 6. Compositor Frame Present
             │   Flush Direct3D Pipeline Context          │   Paints shared texture
             │                                            │   over target swap chain
```

## Project Structure

```text
.
├── include/
│   └── nvovr.hpp                 # Single-header RAII overlay composition library
├── third_party/
│   └── imgui/                    # Dear ImGui core and Direct3D 11 backend
├── docs/
│   └── ARCHITECTURE.md           # In-depth IPC protocol and mathematical analysis
├── .github/
│   ├── workflows/build.yml       # MSBuild continuous integration workflow
│   ├── ISSUE_TEMPLATE/           # Inquiry and defect report templates
│   └── pull_request_template.md  # Formal PR submission template
├── main.cpp                      # Demonstration CLI application with ImGui host
├── nvidia-overlay-shared-mapping.sln     # Visual Studio 2022 Solution
├── nvidia-overlay-shared-mapping.vcxproj # MSBuild C++17 Project
├── .gitignore                    # Visual Studio build artifact exclusions
├── LICENSE                       # MIT License (Adam 2026)
└── README.md                     # Technical documentation
```

## Build & Toolchain

- **Compiler**: Visual Studio 2022 (MSVC v143)
- **Language Standard**: ISO C++17 (`/std:c++17`)
- **Platform**: x64 (Native 64-bit)
- **Configuration**: Release x64

```powershell
# Build application via MSBuild
msbuild nvidia-overlay-shared-mapping.sln /p:Configuration=Release /p:Platform=x64
```

## Research & Educational Disclaimer

This repository is published strictly for educational, security research, and operating system reverse-engineering purposes. It demonstrates Windows inter-process memory sharing mechanisms, Direct3D 11 shared resource lifecycles, and software architecture analysis.
