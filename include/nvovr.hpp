// nvovr.hpp — NVIDIA overlay shared-texture composition library.
//
// Single-header C++17. STB-style: include everywhere; in ONE translation unit
// define NVOVR_IMPLEMENTATION before the include:
//
//     #define NVOVR_IMPLEMENTATION
//     #include <nvovr.hpp>
//
// What this does
// --------------
// The NVIDIA overlay ("ShadowPlay / GeForce Experience in-game overlay")
// exposes a set of named shared file-mappings per target process, one per
// visible overlay element. Each mapping carries a small POD header plus a
// encoded DXGI shared-resource token. The overlay's compositor (living inside
// nvcontainer / driver land) decodes and opens that handle, then paints it on top
// of the target's swap chain — no code executes inside the target process.
//
// This library exposes a clean RAII interface for taking over one of those
// slots, publishing your own shared texture, and drawing arbitrary D3D11
// content (or ImGui via examples/imgui_bench.cpp) that appears composited
// over the target with zero in-process footprint on the target.
//
// Threat model / attribution notes
// --------------------------------
//  * No CreateRemoteThread / VirtualAllocEx / LoadLibrary in the target.
//  * OpenProcess uses PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE only.
//  * The published shared handle is still traceable via NtQueryObject.
//  * The named-section opener PID is visible to a kernel object callback.
//  * NV compositor pulls at target-swap-present rate. Publishing faster than
//    monitor Hz is wasted work; the mechanism itself can push >20 000 fps.
//
// Public surface (all under namespace nvovr)
// ------------------------------------------
//     SlotInfo, SessionOptions, Rect, Placement
//     Session       — attaches to a target process + slot; restores on drop
//     Compositor    — owns the D3D11 device + shared texture; per-frame publish
//     ImGuiHost     — optional glue (define NVOVR_ENABLE_IMGUI before include)
//
// Compile flags
// -------------
//   NVOVR_IMPLEMENTATION       define in exactly one TU
//   NVOVR_ENABLE_IMGUI         opt-in ImGui glue (requires imgui + backend)
//   NVOVR_EXPECTED_VERSION     override the accepted section-header version
//                              (default 0x000B0280; bump for newer drivers)
// License: MIT License (Copyright (c) 2026 Adam)
#ifndef NVOVR_HPP_INCLUDED
#define NVOVR_HPP_INCLUDED

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#ifndef NVOVR_EXPECTED_VERSION
#define NVOVR_EXPECTED_VERSION 0x000B0280u
#endif

namespace nvovr {

// -----------------------------------------------------------------------------
// Process discovery
// -----------------------------------------------------------------------------
//
// Helpers for finding a target by image basename (e.g. L"EscapeFromTarkov.exe")
// instead of hard-coding a PID. Case-insensitive.
namespace process {

// Return the first PID matching `image_basename`, or 0 if not found.
DWORD find_first(const std::wstring& image_basename);

// Return every matching PID (empty on no match).
std::vector<DWORD> find_all(const std::wstring& image_basename);

// Poll every `poll_interval` up to `timeout` for a matching process.
// Returns the PID as soon as one appears, or 0 on timeout.
DWORD wait_for(const std::wstring& image_basename,
               std::chrono::milliseconds timeout,
               std::chrono::milliseconds poll_interval = std::chrono::milliseconds{200});

}  // namespace process

// -----------------------------------------------------------------------------
// Small value types
// -----------------------------------------------------------------------------

struct Rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

// Options for placing the overlay element inside the target's swap chain.
struct Placement {
    Rect       screen{80, 80, 512, 320};  // absolute pixel coords on target
    float      scale_x = 1.0f;
    float      scale_y = 1.0f;
    float      alpha   = 1.0f;
};

// Slot index; -1 = auto-pick the first live GPU-transport slot.
constexpr int kAutoSlot = -1;

struct SessionOptions {
    int         slot                 = kAutoSlot;
    Placement   placement            = {};
    // If non-empty, require the target process image basename to case-
    // insensitively equal this value. Useful safety belt.
    std::wstring require_target_name = {};
    // Prefer the NVIDIA DXGI adapter for our D3D device (recommended).
    bool         nvidia_adapter_only = true;
    // Folded v9 resource-token key. Leave zero to derive it from the current
    // native slot token and CaptureCore diagnostic stream. Callers with an
    // authorized target-memory reader can provide the value directly.
    std::uint64_t resource_key_xor   = 0;
    bool derive_resource_key_from_log = true;
};

// One row of the pre-attach slot scan.
struct SlotInfo {
    unsigned      index          = 0;
    bool          present        = false;
    std::uint32_t version        = 0;
    std::uint32_t transport      = 0;   // 2 == GPU shared surface
    std::uint32_t flags          = 0;   // bit0 => element is visible
    std::uint32_t update_counter = 0;
    std::uint32_t render_counter = 0;
    std::uint64_t generation     = 0;
    std::uint32_t width          = 0;
    std::uint32_t height         = 0;
    float         alpha          = 0.0f;

    bool valid_version() const { return version == NVOVR_EXPECTED_VERSION; }
    bool gpu_transport()  const { return transport == 2; }
    bool visible()        const { return (flags & 1u) != 0; }
};

// Read-only diagnostics for validating the v9 resource-token transform.
// The folded key is ephemeral session state, not a reusable credential.
struct TokenDiagnostics {
    bool          available          = false;
    bool          key_from_log       = false;
    bool          round_trip_ok      = false;
    std::uint64_t folded_key         = 0;
    std::uint32_t seed               = 0;
    std::uint64_t mask               = 0;
    std::uint64_t encoded_resource   = 0;
    std::uint64_t decoded_resource   = 0;
    std::uint64_t last_shared_handle = 0;
    std::uint32_t update_counter     = 0;
    std::uint32_t render_counter     = 0;
    std::uint64_t generation         = 0;
};

// Thrown for every failure. Use .what() for the reason.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

class Compositor;

// -----------------------------------------------------------------------------
// Session — binds one process+slot, restores overlay state on drop.
// -----------------------------------------------------------------------------
//
// Not copyable, movable. Lifetime rule: outlive every Compositor attached to it.
//
// Typical use:
//     auto opts = nvovr::SessionOptions{};
//     opts.placement.screen = {80, 80, 512, 320};
//     opts.require_target_name = L"EscapeFromTarkov.exe";
//     auto sess = nvovr::Session::attach(pid, opts);
//     nvovr::Compositor comp{sess};
//     while (running) {
//         comp.render([](nvovr::Compositor::Frame& f) { /* draw D3D */ });
//     }
class Session {
public:
    // Enumerate all 8 slots for a target PID. No attach, no mutation.
    // Returned rows always have present=true/false correctly set; only the
    // fields backed by an existing mapping are meaningful when present.
    static std::vector<SlotInfo> scan(DWORD pid);

    // Bind to the target. Throws Error on any handshake failure. Restores the
    // slot's original fields when the returned Session is destroyed.
    static Session attach(DWORD pid, const SessionOptions& opts = {});

    // Convenience: look up the first process matching image_basename and
    // attach to it. If wait_timeout > 0, poll for it to appear first.
    // Sets opts.require_target_name to image_basename if none supplied.
    static Session attach_by_name(
        const std::wstring& image_basename,
        SessionOptions opts = {},
        std::chrono::milliseconds wait_timeout = std::chrono::milliseconds{0});

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Accessors.
    DWORD              pid()          const noexcept;
    HANDLE             process()      const noexcept;
    unsigned           slot()         const noexcept;
    std::wstring       target_image() const;
    const Placement&   placement()    const noexcept;
    SlotInfo           current()      const;
    TokenDiagnostics   token_diagnostics() const;

    // Publish a shared surface into this slot. `handle` must remain valid for
    // as long as the publish is in effect. Placement can change per-publish.
    void publish(HANDLE shared_handle, std::uint32_t width, std::uint32_t height,
                 std::uint32_t pitch, const Placement& placement);

    // Bump only the update counter + optional placement/alpha, reusing the
    // last published shared handle. Cheap per-frame poke.
    void repost(const Placement* placement = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class Compositor;
    explicit Session(std::unique_ptr<Impl> impl) noexcept;
};

// -----------------------------------------------------------------------------
// Compositor — one D3D11 device + one BGRA8 shared texture + RTV.
// -----------------------------------------------------------------------------
//
// Instantiate one per Session. Compositor keeps the Session alive by ref;
// user must not destroy the Session first.
class Compositor {
public:
    struct Frame {
        ID3D11Device*           device;
        ID3D11DeviceContext*    context;
        ID3D11Texture2D*        texture;
        ID3D11RenderTargetView* rtv;
        std::uint32_t           width;
        std::uint32_t           height;
        // Filled by begin_frame with the current clear color; user may modify.
        float                   clear_color[4];
    };

    // Creates a shared texture at Session.placement().screen.{w,h}.
    // Recreate the Compositor to change the texture size.
    explicit Compositor(Session& session);
    ~Compositor();
    Compositor(const Compositor&)            = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Begin/end pattern.
    Frame& begin_frame();
    void   present();  // Flush + counter bump + FlushViewOfFile.

    // Single-shot lambda pattern:
    template <typename F>
    void render(F&& fn) {
        auto& f = begin_frame();
        static_cast<F&&>(fn)(f);
        present();
    }

    ID3D11Device*        device()  const noexcept;
    ID3D11DeviceContext* context() const noexcept;
    // Legacy DXGI shared handle for the underlying BGRA8 texture. Do not close.
    HANDLE               shared_handle() const noexcept;

    std::uint32_t width()  const noexcept;
    std::uint32_t height() const noexcept;

    // Change the clear color used by the next begin_frame. Alpha respected.
    void set_clear_color(float r, float g, float b, float a);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// Optional ImGui glue. Enable with:
//     #define NVOVR_ENABLE_IMGUI
// Requires imgui + imgui_impl_dx11 to be linked in the same TU.
// -----------------------------------------------------------------------------
#ifdef NVOVR_ENABLE_IMGUI
class ImGuiHost {
public:
    explicit ImGuiHost(Compositor& comp);
    ~ImGuiHost();
    ImGuiHost(const ImGuiHost&)            = delete;
    ImGuiHost& operator=(const ImGuiHost&) = delete;

    // Call once per frame. Wraps: NewFrame, sets DisplaySize, DeltaTime.
    void new_frame(float delta_seconds);

    // Wraps: Render, OMSetRenderTargets, viewport, clear, RenderDrawData,
    // Compositor::present().
    void render();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif  // NVOVR_ENABLE_IMGUI

}  // namespace nvovr


// =============================================================================
// IMPLEMENTATION
// =============================================================================
#ifdef NVOVR_IMPLEMENTATION

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

#include <tlhelp32.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace nvovr {
namespace detail {

// The published section is named per slot:
//   {1BBC83F3-72CB-453B-AD96-E1028AFFB5E9}_v9_<0..7>
// The associated mutex appends "_0" to the section name.
inline constexpr wchar_t kSectionStem[] =
    L"{1BBC83F3-72CB-453B-AD96-E1028AFFB5E9}_v9";
inline constexpr std::size_t kLogicalSize     = 0x280;
inline constexpr std::uint32_t kSlotCount     = 8;
inline constexpr std::uint32_t kMutexTimeout  = 2000;  // ms

// Field offsets inside one slot's mapping (reversed empirically, driver
// 11.02.80; bump NVOVR_EXPECTED_VERSION only after revalidating the layout).
inline constexpr std::size_t kOffVersion        = 0x000;
inline constexpr std::size_t kOffUpdateCounter  = 0x008;
inline constexpr std::size_t kOffTransport      = 0x00C;
inline constexpr std::size_t kOffGeneration     = 0x010;
inline constexpr std::size_t kOffSharedResource = 0x018;
inline constexpr std::size_t kOffFormat         = 0x020;
inline constexpr std::size_t kOffWidth          = 0x024;
inline constexpr std::size_t kOffHeight         = 0x028;
inline constexpr std::size_t kOffPitch          = 0x02C;
inline constexpr std::size_t kOffAbsX           = 0x048;
inline constexpr std::size_t kOffAbsY           = 0x04C;
inline constexpr std::size_t kOffScaleX         = 0x050;
inline constexpr std::size_t kOffScaleY         = 0x054;
inline constexpr std::size_t kOffFlags          = 0x058;
inline constexpr std::size_t kOffRenderCounter  = 0x124;
inline constexpr std::size_t kOffRectCount      = 0x130;
inline constexpr std::size_t kOffFirstRect      = 0x134;
inline constexpr std::size_t kOffAlpha          = 0x274;
inline constexpr std::size_t kOffTokenSeed      = 0x278;

// D3DFMT_A8R8G8B8; matches DXGI_FORMAT_B8G8R8A8_UNORM on the wire.
inline constexpr std::uint32_t kFormatBgra8 = 21;

// -----------------------------------------------------------------------------
// Handle wrappers.
// -----------------------------------------------------------------------------
struct HandleGuard {
    HANDLE h = nullptr;
    HandleGuard() = default;
    explicit HandleGuard(HANDLE handle) noexcept : h(handle) {}
    HandleGuard(HandleGuard&& o) noexcept : h(std::exchange(o.h, nullptr)) {}
    HandleGuard& operator=(HandleGuard&& o) noexcept {
        if (this != &o) { reset(); h = std::exchange(o.h, nullptr); }
        return *this;
    }
    HandleGuard(const HandleGuard&)            = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    ~HandleGuard() { reset(); }
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    HANDLE release() noexcept { return std::exchange(h, nullptr); }
    explicit operator bool() const noexcept { return h && h != INVALID_HANDLE_VALUE; }
};

struct MapView {
    void* p = nullptr;
    MapView() = default;
    explicit MapView(void* ptr) noexcept : p(ptr) {}
    MapView(MapView&& o) noexcept : p(std::exchange(o.p, nullptr)) {}
    MapView& operator=(MapView&& o) noexcept {
        if (this != &o) { reset(); p = std::exchange(o.p, nullptr); }
        return *this;
    }
    MapView(const MapView&)            = delete;
    MapView& operator=(const MapView&) = delete;
    ~MapView() { reset(); }
    void reset() { if (p) UnmapViewOfFile(p); p = nullptr; }
};

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr(const ComPtr&)            = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p(std::exchange(o.p, nullptr)) {}
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { if (p) p->Release(); p = std::exchange(o.p, nullptr); }
        return *this;
    }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T*  get() const noexcept { return p; }
    T*  operator->() const noexcept { return p; }
    explicit operator bool() const noexcept { return p != nullptr; }
};

struct MutexLock {
    HANDLE h = nullptr;
    bool held = false;
    explicit MutexLock(HANDLE handle) : h(handle) {
        const DWORD r = WaitForSingleObject(h, kMutexTimeout);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED)
            throw Error("nvovr: timed out acquiring overlay mutex");
        held = true;
    }
    ~MutexLock() { if (held) ReleaseMutex(h); }
    MutexLock(const MutexLock&)            = delete;
    MutexLock& operator=(const MutexLock&) = delete;
};

// -----------------------------------------------------------------------------
// Byte helpers.
// -----------------------------------------------------------------------------
template <typename T>
inline T mem_read(const std::byte* b, std::size_t off) noexcept {
    T v{}; std::memcpy(&v, b + off, sizeof(T)); return v;
}
template <typename T>
inline void mem_write(std::byte* b, std::size_t off, const T& v) noexcept {
    std::memcpy(b + off, &v, sizeof(T));
}

// v9 resource-token transform used by nvspcap64 11.0.9.251. The four
// per-session key qwords are folded with XOR before this function is called.
inline std::uint64_t resource_token_mask(std::uint64_t key_xor,
                                         std::uint32_t seed) noexcept {
    constexpr std::uint64_t c1 = 0xFF51AFD7ED558CCDull;
    constexpr std::uint64_t cx = 0xED77E7F1C90AA277ull;
    constexpr std::uint64_t c2 = 0xC4CEB9FE1A85EC53ull;
    const std::uint64_t repeated = static_cast<std::uint64_t>(seed) |
        (static_cast<std::uint64_t>(seed) << 32);
    std::uint64_t value = key_xor ^ repeated;
    value = (value ^ (value >> 33)) * c1;
    value = (value ^ (value >> 33)) * cx;
    value = (value ^ (value >> 33)) * c2;
    return value ^ (value >> 33);
}

inline std::uint64_t undo_xor_shift33(std::uint64_t value) noexcept {
    // The second shifted term would be >>66 and therefore zero.
    return value ^ (value >> 33);
}

inline std::uint64_t recover_resource_key(std::uint64_t mask,
                                          std::uint32_t seed) noexcept {
    constexpr std::uint64_t inv_c1 = 0x4F74430C22A54005ull;
    constexpr std::uint64_t inv_cx = 0xD361F90F40FDD747ull;
    constexpr std::uint64_t inv_c2 = 0x9CB4B2F8129337DBull;
    std::uint64_t value = undo_xor_shift33(mask);
    value = undo_xor_shift33(value * inv_c2);
    value = undo_xor_shift33(value * inv_cx);
    value = undo_xor_shift33(value * inv_c1);
    const std::uint64_t repeated = static_cast<std::uint64_t>(seed) |
        (static_cast<std::uint64_t>(seed) << 32);
    return value ^ repeated;
}

inline std::optional<std::uint64_t> latest_decoded_resource(DWORD pid,
                                                            unsigned slot) {
    wchar_t program_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramData", program_data, static_cast<DWORD>(std::size(program_data)));
    if (!length || length >= std::size(program_data)) return std::nullopt;
    const auto log_path = std::filesystem::path(program_data) /
        L"NVIDIA Corporation" / L"ShadowPlay" / L"CaptureCore.log";
    std::ifstream input(log_path, std::ios::binary);
    if (!input) return std::nullopt;

    const std::string pid_marker = ":" + std::to_string(pid) + ":";
    const std::string slot_marker = "slot[" + std::to_string(slot) + "]";
    constexpr char resource_marker[] = "hResource[0x";
    std::optional<std::uint64_t> latest;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find(pid_marker) == std::string::npos ||
            line.find(slot_marker) == std::string::npos)
            continue;
        const auto at = line.find(resource_marker);
        if (at == std::string::npos) continue;
        const auto begin = at + sizeof(resource_marker) - 1;
        const auto end = line.find_first_not_of("0123456789abcdefABCDEF", begin);
        const auto digits = line.substr(begin, end - begin);
        if (digits.empty() || digits.size() > 16) continue;
        try {
            const std::uint64_t value = std::stoull(digits, nullptr, 16);
            if (value) latest = value;
        } catch (const std::exception&) {
        }
    }
    return latest;
}

// -----------------------------------------------------------------------------
// Utilities.
// -----------------------------------------------------------------------------
inline std::wstring section_name(unsigned slot) {
    return std::wstring(kSectionStem) + L"_" + std::to_wstring(slot);
}

inline std::wstring process_image(HANDLE process) {
    std::vector<wchar_t> path(32768);
    DWORD len = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &len))
        throw Error("nvovr: QueryFullProcessImageNameW failed");
    return std::wstring(path.data(), len);
}

inline SlotInfo probe_slot(unsigned slot) {
    SlotInfo si{};
    si.index = slot;
    const auto name = section_name(slot);
    HANDLE mh = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mh) return si;
    HandleGuard mapping{mh};
    void* v = MapViewOfFile(mapping.h, FILE_MAP_READ, 0, 0, 0);
    if (!v) return si;
    MapView view{v};
    auto* b = static_cast<const std::byte*>(v);
    si.present        = true;
    si.version        = mem_read<std::uint32_t>(b, kOffVersion);
    si.transport      = mem_read<std::uint32_t>(b, kOffTransport);
    si.flags          = mem_read<std::uint32_t>(b, kOffFlags);
    si.update_counter = mem_read<std::uint32_t>(b, kOffUpdateCounter);
    si.render_counter = mem_read<std::uint32_t>(b, kOffRenderCounter);
    si.generation     = mem_read<std::uint64_t>(b, kOffGeneration);
    si.width          = mem_read<std::uint32_t>(b, kOffWidth);
    si.height         = mem_read<std::uint32_t>(b, kOffHeight);
    si.alpha          = mem_read<float        >(b, kOffAlpha);
    return si;
}

// Snapshot of every field this library rewrites; restored on Session drop.
struct SlotSnapshot {
    std::uint32_t update{};
    std::uint64_t generation{};
    std::uint64_t resource{};
    std::uint32_t format{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t pitch{};
    std::uint32_t abs_x{};
    std::uint32_t abs_y{};
    float         scale_x{};
    float         scale_y{};
    std::uint32_t flags{};
    std::uint32_t render{};
    std::uint32_t rects{};
    std::array<std::uint32_t, 10> first_rect{};
    float         alpha{};
};

inline SlotSnapshot capture(const std::byte* b) {
    return { mem_read<std::uint32_t>(b, kOffUpdateCounter),
             mem_read<std::uint64_t>(b, kOffGeneration),
             mem_read<std::uint64_t>(b, kOffSharedResource),
             mem_read<std::uint32_t>(b, kOffFormat),
             mem_read<std::uint32_t>(b, kOffWidth),
             mem_read<std::uint32_t>(b, kOffHeight),
             mem_read<std::uint32_t>(b, kOffPitch),
             mem_read<std::uint32_t>(b, kOffAbsX),
             mem_read<std::uint32_t>(b, kOffAbsY),
             mem_read<float        >(b, kOffScaleX),
             mem_read<float        >(b, kOffScaleY),
             mem_read<std::uint32_t>(b, kOffFlags),
             mem_read<std::uint32_t>(b, kOffRenderCounter),
             mem_read<std::uint32_t>(b, kOffRectCount),
             mem_read<std::array<std::uint32_t, 10>>(b, kOffFirstRect),
             mem_read<float        >(b, kOffAlpha) };
}

inline void restore(std::byte* b, const SlotSnapshot& s, std::uint32_t update) {
    mem_write(b, kOffGeneration,     s.generation);
    mem_write(b, kOffSharedResource, s.resource);
    mem_write(b, kOffFormat,         s.format);
    mem_write(b, kOffWidth,          s.width);
    mem_write(b, kOffHeight,         s.height);
    mem_write(b, kOffPitch,          s.pitch);
    mem_write(b, kOffAbsX,           s.abs_x);
    mem_write(b, kOffAbsY,           s.abs_y);
    mem_write(b, kOffScaleX,         s.scale_x);
    mem_write(b, kOffScaleY,         s.scale_y);
    mem_write(b, kOffFlags,          s.flags);
    mem_write(b, kOffRenderCounter,  s.render);
    mem_write(b, kOffRectCount,      s.rects);
    mem_write(b, kOffFirstRect,      s.first_rect);
    mem_write(b, kOffAlpha,          s.alpha);
    MemoryBarrier();
    mem_write(b, kOffUpdateCounter,  update);
}

}  // namespace detail

// -----------------------------------------------------------------------------
// nvovr::process implementations
// -----------------------------------------------------------------------------
namespace process {

DWORD find_first(const std::wstring& image_basename) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD hit = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, image_basename.c_str()) == 0) {
                hit = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return hit;
}

std::vector<DWORD> find_all(const std::wstring& image_basename) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, image_basename.c_str()) == 0)
                out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

DWORD wait_for(const std::wstring& image_basename,
               std::chrono::milliseconds timeout,
               std::chrono::milliseconds poll_interval) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    for (;;) {
        DWORD pid = find_first(image_basename);
        if (pid) return pid;
        if (clock::now() >= deadline) return 0;
        std::this_thread::sleep_for(poll_interval);
    }
}

}  // namespace process

// -----------------------------------------------------------------------------
// Session::Impl
// -----------------------------------------------------------------------------
struct Session::Impl {
    DWORD                 pid           = 0;
    unsigned              slot          = 0;
    detail::HandleGuard   process;
    detail::HandleGuard   mapping;
    detail::MapView       view;
    detail::HandleGuard   mutex;
    std::byte*            bytes         = nullptr;
    detail::SlotSnapshot  original      = {};
    bool                  restored      = false;
    Placement             placement     = {};
    std::uint64_t         resource_key_xor = 0;
    bool                  resource_key_from_log = false;

    // Publish state (cached so repost() is cheap).
    std::uint64_t         gen_cursor    = 0;
    std::uint32_t         render_cursor = 0;
    std::uint64_t         last_handle   = 0;
    std::uint32_t         last_width    = 0;
    std::uint32_t         last_height   = 0;
    std::uint32_t         last_pitch    = 0;

    void restore_now() {
        if (restored || !bytes) return;
        try {
            detail::MutexLock lk{mutex.h};
            std::uint32_t cur = detail::mem_read<std::uint32_t>(bytes, detail::kOffUpdateCounter);
            std::uint32_t next = (cur == UINT32_MAX) ? original.update : cur + 1;
            detail::restore(bytes, original, next);
            FlushViewOfFile(bytes, detail::kLogicalSize);
        } catch (...) { /* swallow — dtor path */ }
        restored = true;
    }
};

Session::Session(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Session::Session(Session&&) noexcept            = default;
Session& Session::operator=(Session&&) noexcept = default;

Session::~Session() {
    if (impl_) impl_->restore_now();
}

std::vector<SlotInfo> Session::scan(DWORD pid) {
    // pid parameter is accepted for symmetry with future per-target sections;
    // the current NVIDIA layout uses process-scoped named sections opened by
    // name, so no PID handshake is required here.
    (void)pid;
    std::vector<SlotInfo> rows;
    rows.reserve(detail::kSlotCount);
    for (unsigned i = 0; i < detail::kSlotCount; ++i)
        rows.push_back(detail::probe_slot(i));
    return rows;
}

Session Session::attach(DWORD pid, const SessionOptions& opts) {
    if (!pid) throw Error("nvovr: pid == 0");

    auto impl = std::make_unique<Impl>();
    impl->pid       = pid;
    impl->placement = opts.placement;
    impl->resource_key_xor = opts.resource_key_xor;

    impl->process = detail::HandleGuard{OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
    if (!impl->process) throw Error("nvovr: OpenProcess(query-only) failed");

    if (!opts.require_target_name.empty()) {
        const auto image = detail::process_image(impl->process.h);
        const auto base  = std::filesystem::path(image).filename().wstring();
        if (_wcsicmp(base.c_str(), opts.require_target_name.c_str()) != 0)
            throw Error("nvovr: target image name mismatch");
    }

    // Pick slot.
    int chosen = opts.slot;
    if (chosen == kAutoSlot) {
        int best = -1;
        std::uint64_t best_score = 0;
        for (unsigned i = 0; i < detail::kSlotCount; ++i) {
            auto si = detail::probe_slot(i);
            if (!si.present || !si.valid_version() || !si.gpu_transport()) continue;
            const std::uint64_t score = (static_cast<std::uint64_t>(si.visible()) << 40)
                                        | si.render_counter;
            if (best < 0 || score > best_score) {
                best_score = score;
                best = static_cast<int>(i);
            }
        }
        if (best < 0) throw Error("nvovr: no live GPU-transport slot found");
        chosen = best;
    }
    if (chosen < 0 || chosen >= static_cast<int>(detail::kSlotCount))
        throw Error("nvovr: invalid slot index");
    impl->slot = static_cast<unsigned>(chosen);

    // Open section + mutex.
    const auto sec = detail::section_name(impl->slot);
    const DWORD access = FILE_MAP_READ | FILE_MAP_WRITE;
    impl->mapping = detail::HandleGuard{OpenFileMappingW(access, FALSE, sec.c_str())};
    if (!impl->mapping) throw Error("nvovr: OpenFileMappingW failed");
    void* v = MapViewOfFile(impl->mapping.h, access, 0, 0, 0);
    if (!v) throw Error("nvovr: MapViewOfFile failed");
    impl->view  = detail::MapView{v};
    impl->bytes = static_cast<std::byte*>(v);

    const std::uint32_t ver = detail::mem_read<std::uint32_t>(impl->bytes, detail::kOffVersion);
    if (ver != NVOVR_EXPECTED_VERSION)
        throw Error("nvovr: section version mismatch (rebuild with NVOVR_EXPECTED_VERSION)");
    const std::uint32_t tr = detail::mem_read<std::uint32_t>(impl->bytes, detail::kOffTransport);
    if (tr != 2)
        throw Error("nvovr: slot not using GPU shared-surface transport");

    impl->mutex = detail::HandleGuard{OpenMutexW(
        SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (sec + L"_0").c_str())};
    if (!impl->mutex) throw Error("nvovr: OpenMutexW failed");

    // Snapshot original fields for later restore.
    {
        detail::MutexLock lk{impl->mutex.h};
        impl->original = detail::capture(impl->bytes);
        if (impl->original.update == UINT32_MAX ||
            impl->original.render == UINT32_MAX ||
            impl->original.generation == UINT64_MAX)
            throw Error("nvovr: counter about to roll over — refusing to attach");
        impl->gen_cursor    = impl->original.generation;
        impl->render_cursor = impl->original.render & 0xFFFFF000u;
        if (!impl->resource_key_xor && opts.derive_resource_key_from_log) {
            const auto decoded = detail::latest_decoded_resource(pid, impl->slot);
            const auto seed = detail::mem_read<std::uint32_t>(
                impl->bytes, detail::kOffTokenSeed);
            if (decoded && impl->original.resource && seed) {
                const auto mask = impl->original.resource ^ *decoded;
                impl->resource_key_xor = detail::recover_resource_key(mask, seed);
                impl->resource_key_from_log = true;
            }
        }
        if (!impl->resource_key_xor)
            throw Error("nvovr: v9 resource-token key unavailable");
    }

    return Session{std::move(impl)};
}

Session Session::attach_by_name(const std::wstring& image_basename,
                                SessionOptions opts,
                                std::chrono::milliseconds wait_timeout) {
    if (image_basename.empty())
        throw Error("nvovr: attach_by_name got empty image basename");
    if (opts.require_target_name.empty()) opts.require_target_name = image_basename;

    DWORD pid = wait_timeout.count() > 0
        ? process::wait_for(image_basename, wait_timeout)
        : process::find_first(image_basename);
    if (!pid) throw Error("nvovr: target process not found");
    return attach(pid, opts);
}

DWORD              Session::pid()          const noexcept { return impl_->pid; }
HANDLE             Session::process()      const noexcept { return impl_->process.h; }
unsigned           Session::slot()         const noexcept { return impl_->slot; }
std::wstring       Session::target_image() const           { return detail::process_image(impl_->process.h); }
const Placement&   Session::placement()    const noexcept { return impl_->placement; }

SlotInfo Session::current() const {
    return detail::probe_slot(impl_->slot);
}

TokenDiagnostics Session::token_diagnostics() const {
    TokenDiagnostics out{};
    if (!impl_ || !impl_->bytes || !impl_->resource_key_xor) return out;

    detail::MutexLock lk{impl_->mutex.h};
    out.available = true;
    out.key_from_log = impl_->resource_key_from_log;
    out.folded_key = impl_->resource_key_xor;
    out.seed = detail::mem_read<std::uint32_t>(impl_->bytes, detail::kOffTokenSeed);
    out.encoded_resource = detail::mem_read<std::uint64_t>(
        impl_->bytes, detail::kOffSharedResource);
    out.update_counter = detail::mem_read<std::uint32_t>(
        impl_->bytes, detail::kOffUpdateCounter);
    out.render_counter = detail::mem_read<std::uint32_t>(
        impl_->bytes, detail::kOffRenderCounter);
    out.generation = detail::mem_read<std::uint64_t>(
        impl_->bytes, detail::kOffGeneration);
    out.last_shared_handle = impl_->last_handle;
    if (out.seed) {
        out.mask = detail::resource_token_mask(out.folded_key, out.seed);
        out.decoded_resource = out.encoded_resource ^ out.mask;
        out.round_trip_ok = out.last_shared_handle != 0 &&
            out.decoded_resource == out.last_shared_handle;
    }
    return out;
}

void Session::publish(HANDLE shared_handle, std::uint32_t width, std::uint32_t height,
                      std::uint32_t pitch, const Placement& p) {
    if (!shared_handle) throw Error("nvovr: publish() got null shared handle");
    detail::MutexLock lk{impl_->mutex.h};

    ++impl_->gen_cursor;
    if (impl_->render_cursor > UINT32_MAX - 0x1000u)
        throw Error("nvovr: render generation about to roll over");
    impl_->render_cursor += 0x1000u;

    auto* b = impl_->bytes;
    detail::mem_write(b, detail::kOffGeneration, impl_->gen_cursor);
    const std::uint32_t token_seed = detail::mem_read<std::uint32_t>(
        b, detail::kOffTokenSeed);
    if (!token_seed) throw Error("nvovr: v9 resource-token seed unavailable");
    const std::uint64_t encoded_resource =
        reinterpret_cast<std::uint64_t>(shared_handle) ^
        detail::resource_token_mask(impl_->resource_key_xor, token_seed);
    detail::mem_write(b, detail::kOffSharedResource, encoded_resource);
    detail::mem_write(b, detail::kOffFormat, detail::kFormatBgra8);
    detail::mem_write(b, detail::kOffWidth,  width);
    detail::mem_write(b, detail::kOffHeight, height);
    detail::mem_write(b, detail::kOffPitch,  pitch);
    detail::mem_write(b, detail::kOffAbsX,   p.screen.x);
    detail::mem_write(b, detail::kOffAbsY,   p.screen.y);
    detail::mem_write(b, detail::kOffScaleX, p.scale_x);
    detail::mem_write(b, detail::kOffScaleY, p.scale_y);
    detail::mem_write(b, detail::kOffFlags,  impl_->original.flags | 1u);

    const std::array<std::uint32_t, 10> rect_full = {
        0, 0, 0, width, height,
        0, 0, 0, width, height };
    detail::mem_write(b, detail::kOffRectCount, std::uint32_t{1});
    detail::mem_write(b, detail::kOffFirstRect, rect_full);

    detail::mem_write(b, detail::kOffRenderCounter, impl_->render_cursor | 3u);
    detail::mem_write(b, detail::kOffAlpha, p.alpha);

    std::uint32_t u = detail::mem_read<std::uint32_t>(b, detail::kOffUpdateCounter);
    if (u == UINT32_MAX) u = 0;
    MemoryBarrier();
    detail::mem_write(b, detail::kOffUpdateCounter, u + 1);

    // Mapped-section writes are coherent between processes; flushing the view
    // only requests backing-store persistence and stalls a high-rate publisher.

    impl_->placement   = p;
    impl_->last_handle = reinterpret_cast<std::uint64_t>(shared_handle);
    impl_->last_width  = width;
    impl_->last_height = height;
    impl_->last_pitch  = pitch;
}

void Session::repost(const Placement* p) {
    if (!impl_->last_handle) throw Error("nvovr: repost() before first publish()");
    const Placement pl = p ? *p : impl_->placement;
    publish(reinterpret_cast<HANDLE>(impl_->last_handle),
            impl_->last_width, impl_->last_height, impl_->last_pitch, pl);
}

// -----------------------------------------------------------------------------
// Compositor::Impl
// -----------------------------------------------------------------------------
struct Compositor::Impl {
    Session*                        session = nullptr;
    detail::ComPtr<IDXGIFactory1>   factory;
    detail::ComPtr<IDXGIAdapter1>   adapter;
    detail::ComPtr<ID3D11Device>    device;
    detail::ComPtr<ID3D11DeviceContext> context;
    detail::ComPtr<ID3D11Texture2D> texture;
    detail::ComPtr<IDXGIResource>   resource;
    detail::ComPtr<ID3D11RenderTargetView> rtv;
    HANDLE                          shared_handle = nullptr;
    std::uint32_t                   width  = 0;
    std::uint32_t                   height = 0;
    float                           clear_color[4] = {0.f, 0.f, 0.f, 0.f};
    Frame                           frame{};

    void build_device(bool nv_only) {
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(factory.put()));
        if (FAILED(hr)) throw Error("nvovr: CreateDXGIFactory1 failed");

        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* cand = nullptr;
            hr = factory->EnumAdapters1(i, &cand);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) throw Error("nvovr: EnumAdapters1 failed");
            DXGI_ADAPTER_DESC1 d{};
            cand->GetDesc1(&d);
            const bool pick = nv_only ? (d.VendorId == 0x10DE) : true;
            if (pick) { adapter.put(); adapter.p = cand; break; }
            cand->Release();
        }
        if (!adapter && nv_only) throw Error("nvovr: no NVIDIA DXGI adapter");
        if (!adapter) throw Error("nvovr: no DXGI adapter");

        D3D_FEATURE_LEVEL got{};
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, device.put(), &got, context.put());
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
                static_cast<UINT>(std::size(levels)) - 1,
                D3D11_SDK_VERSION, device.put(), &got, context.put());
        }
        if (FAILED(hr)) throw Error("nvovr: D3D11CreateDevice failed");
    }

    void build_texture() {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, texture.put());
        if (FAILED(hr)) throw Error("nvovr: CreateTexture2D(shared) failed");

        hr = texture->QueryInterface(__uuidof(IDXGIResource),
            reinterpret_cast<void**>(resource.put()));
        if (FAILED(hr)) throw Error("nvovr: IDXGIResource query failed");
        hr = resource->GetSharedHandle(&shared_handle);
        if (FAILED(hr) || !shared_handle) throw Error("nvovr: GetSharedHandle failed");

        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device->CreateRenderTargetView(texture.get(), &rvd, rtv.put());
        if (FAILED(hr)) throw Error("nvovr: CreateRenderTargetView failed");
    }
};

Compositor::Compositor(Session& session) : impl_(std::make_unique<Impl>()) {
    impl_->session = &session;
    impl_->width   = session.placement().screen.w;
    impl_->height  = session.placement().screen.h;
    if (impl_->width < 8 || impl_->height < 8)
        throw Error("nvovr: placement size must be >= 8x8");

    impl_->build_device(true);
    impl_->build_texture();
}

Compositor::~Compositor() = default;

Compositor::Frame& Compositor::begin_frame() {
    auto& f = impl_->frame;
    f.device       = impl_->device.get();
    f.context      = impl_->context.get();
    f.texture      = impl_->texture.get();
    f.rtv          = impl_->rtv.get();
    f.width        = impl_->width;
    f.height       = impl_->height;
    std::memcpy(f.clear_color, impl_->clear_color, sizeof(f.clear_color));

    ID3D11RenderTargetView* rtvs[] = { impl_->rtv.get() };
    impl_->context->OMSetRenderTargets(1, rtvs, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width  = static_cast<float>(impl_->width);
    vp.Height = static_cast<float>(impl_->height);
    vp.MaxDepth = 1.0f;
    impl_->context->RSSetViewports(1, &vp);
    impl_->context->ClearRenderTargetView(impl_->rtv.get(), f.clear_color);
    return f;
}

void Compositor::present() {
    impl_->context->Flush();
    const std::uint32_t pitch = impl_->width * 4;
    impl_->session->publish(impl_->shared_handle, impl_->width, impl_->height,
                            pitch, impl_->session->placement());
}

ID3D11Device*        Compositor::device()        const noexcept { return impl_->device.get(); }
ID3D11DeviceContext* Compositor::context()       const noexcept { return impl_->context.get(); }
HANDLE               Compositor::shared_handle() const noexcept { return impl_->shared_handle; }
std::uint32_t        Compositor::width()         const noexcept { return impl_->width; }
std::uint32_t        Compositor::height()        const noexcept { return impl_->height; }

void Compositor::set_clear_color(float r, float g, float b, float a) {
    impl_->clear_color[0] = r; impl_->clear_color[1] = g;
    impl_->clear_color[2] = b; impl_->clear_color[3] = a;
}

// -----------------------------------------------------------------------------
// Optional ImGui glue.
// -----------------------------------------------------------------------------
#ifdef NVOVR_ENABLE_IMGUI
struct ImGuiHost::Impl {
    Compositor* comp = nullptr;
    ImGuiContext* ctx = nullptr;
    bool          own_ctx = false;
};

ImGuiHost::ImGuiHost(Compositor& comp) : impl_(std::make_unique<Impl>()) {
    impl_->comp = &comp;
    if (!ImGui::GetCurrentContext()) {
        impl_->ctx = ImGui::CreateContext();
        impl_->own_ctx = true;
    } else {
        impl_->ctx = ImGui::GetCurrentContext();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(comp.width()),
                            static_cast<float>(comp.height()));
    ImGui::StyleColorsDark();
    if (!ImGui_ImplDX11_Init(comp.device(), comp.context()))
        throw Error("nvovr: ImGui_ImplDX11_Init failed");
}

ImGuiHost::~ImGuiHost() {
    ImGui_ImplDX11_Shutdown();
    if (impl_->own_ctx) ImGui::DestroyContext(impl_->ctx);
}

void ImGuiHost::new_frame(float dt) {
    ImGui_ImplDX11_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(impl_->comp->width()),
                            static_cast<float>(impl_->comp->height()));
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    ImGui::NewFrame();
}

void ImGuiHost::render() {
    ImGui::Render();
    auto& f = impl_->comp->begin_frame();
    (void)f;
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    impl_->comp->present();
}
#endif  // NVOVR_ENABLE_IMGUI

}  // namespace nvovr

#endif  // NVOVR_IMPLEMENTATION
#endif  // NVOVR_HPP_INCLUDED
