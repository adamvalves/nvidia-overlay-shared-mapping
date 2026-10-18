#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#define NVOVR_ENABLE_IMGUI
#define NVOVR_IMPLEMENTATION
#include "nvovr.hpp"

#include <chrono>
#include <cfloat>
#include <iostream>
#include <thread>

namespace {

struct WindowSearch {
    DWORD pid;
    HWND window;
};

BOOL CALLBACK find_window(HWND window, LPARAM value) {
    auto& search = *reinterpret_cast<WindowSearch*>(value);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);

    if (pid == search.pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        search.window = window;
        return FALSE;
    }

    return TRUE;
}

HWND find_window(DWORD pid) {
    WindowSearch search{pid, nullptr};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

void update_mouse(HWND target_window, DWORD target_pid) {
    ImGuiIO& io = ImGui::GetIO();
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);

    if (!target_window || foreground_pid != target_pid) {
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
        io.MouseDown[0] = false;
        io.MouseDown[1] = false;
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(target_window, &cursor);

    io.MousePos = ImVec2(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
}

} 

int wmain(int argc, wchar_t* argv[]) {
    try {
        const wchar_t* target_process = (argc > 1) ? argv[1] : L"notepad.exe";

        nvovr::SessionOptions options;
        options.placement.screen = {0, 0, 1280, 720};

        std::wcout << L"Attempting attachment to target: " << target_process << L"...\n";
        auto session = nvovr::Session::attach_by_name(target_process, options);
        nvovr::Compositor compositor{session};
        nvovr::ImGuiHost ui{compositor};
        HWND target_window = find_window(session.pid());
        int clicks = 0;

        compositor.set_clear_color(0.0f, 0.0f, 0.0f, 0.0f);

        std::wcout << L"Attached to " << session.target_image()
                   << L" (PID " << session.pid() << L")\n";

        auto previous = std::chrono::steady_clock::now();
        while (WaitForSingleObject(session.process(), 0) == WAIT_TIMEOUT) {
            const auto now = std::chrono::steady_clock::now();
            const float delta = std::chrono::duration<float>(now - previous).count();
            previous = now;

            ui.new_frame(delta);
            if (!IsWindow(target_window)) {
                target_window = find_window(session.pid());
            }
            update_mouse(target_window, session.pid());

            ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(360.0f, 170.0f), ImGuiCond_Once);
            ImGui::Begin("NVOverlay", nullptr, ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted("NVIDIA Overlay Shared-Memory Composition");
            ImGui::Separator();
            ImGui::TextUnformatted("Architecture Demonstration - In-Memory IPC");
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            if (ImGui::Button("Click me")) {
                ++clicks;
            }
            ImGui::SameLine();
            ImGui::Text("Clicks: %d", clicks);
            ImGui::End();

            ui.render();
            std::this_thread::yield();
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Attach failed: " << error.what() << '\n';
        return 1;
    }
}
