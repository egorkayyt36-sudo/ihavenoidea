#include "gui.h"

#include "log.h"
#include "recorder.h"
#include "speedhack.h"
#include "state.h"

#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <string>
#include <thread>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace tasdll {

namespace {

std::thread        g_thread;
std::atomic<bool>  g_running{false};
HWND               g_hwnd = nullptr;

ID3D11Device*           g_pd3dDevice         = nullptr;
ID3D11DeviceContext*    g_pd3dDeviceContext  = nullptr;
IDXGISwapChain*         g_pSwapChain         = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width  = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        featureLevels, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) {
        log_line("gui: D3D11CreateDeviceAndSwapChain failed: 0x%08lx", (unsigned long)hr);
        return false;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (!backbuffer) return false;
    g_pd3dDevice->CreateRenderTargetView(backbuffer, nullptr, &g_mainRenderTargetView);
    backbuffer->Release();
    return true;
}

void destroy_device() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain)           { g_pSwapChain->Release();           g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)    { g_pd3dDeviceContext->Release();    g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)           { g_pd3dDevice->Release();           g_pd3dDevice = nullptr; }
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wp != SIZE_MINIMIZED) {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
            g_pSwapChain->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* bb = nullptr;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
            g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRenderTargetView);
            bb->Release();
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        state().gui_visible.store(false);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void draw_ui() {
    auto& s = state();

    if (!s.gui_visible.load()) {
        if (g_hwnd && IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
    } else {
        if (g_hwnd && !IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOWNA);
    }

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    ImGui::Begin("tasdll");

    double speed_d = speedhack_current_speed();
    float  speed   = (float)speed_d;
    ImGui::Text("Speedhack");
    ImGui::Separator();
    if (ImGui::SliderFloat("speed", &speed, 0.05f, 10.0f, "%.3fx", ImGuiSliderFlags_Logarithmic)) {
        speedhack_set_speed((double)speed);
    }
    ImGui::SameLine();
    if (ImGui::Button("1x")) speedhack_set_speed(1.0);

    const float presets[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
    for (float p : presets) {
        char lbl[16]; snprintf(lbl, sizeof(lbl), "%.2gx", p);
        if (ImGui::SmallButton(lbl)) speedhack_set_speed((double)p);
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::Spacing();
    ImGui::Text("Recording");
    ImGui::Separator();

    {
        std::lock_guard<std::mutex> lk(s.cfg_mu);
        char out_buf[512];
        WideCharToMultiByte(CP_UTF8, 0, s.output_dir.c_str(), -1, out_buf, sizeof(out_buf), nullptr, nullptr);
        if (ImGui::InputText("output dir", out_buf, sizeof(out_buf))) {
            wchar_t wbuf[512];
            MultiByteToWideChar(CP_UTF8, 0, out_buf, -1, wbuf, _countof(wbuf));
            s.output_dir = wbuf;
        }
        char title_buf[256];
        WideCharToMultiByte(CP_UTF8, 0, s.window_title.c_str(), -1, title_buf, sizeof(title_buf), nullptr, nullptr);
        if (ImGui::InputText("window title", title_buf, sizeof(title_buf))) {
            wchar_t wbuf[256];
            MultiByteToWideChar(CP_UTF8, 0, title_buf, -1, wbuf, _countof(wbuf));
            s.window_title = wbuf;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("foreground")) {
            s.window_title = foreground_window_title();
        }

        bool desktop = s.use_desktop;
        if (ImGui::Checkbox("capture whole desktop", &desktop)) s.use_desktop = desktop;
        ImGui::SameLine();
        bool audio = s.capture_audio;
        if (ImGui::Checkbox("audio (dshow)", &audio)) s.capture_audio = audio;

        int fps = s.capture_fps;
        if (ImGui::InputInt("fps", &fps)) { if (fps < 1) fps = 1; if (fps > 240) fps = 240; s.capture_fps = fps; }
    }

    bool active = recorder_is_active();
    if (!active) {
        if (ImGui::Button("Start Recording (F9)")) {
            recorder_start(speedhack_current_speed());
        }
    } else {
        if (ImGui::Button("Stop Recording (F9)")) {
            recorder_stop();
        }
    }

    std::wstring last_out = recorder_last_output_path();
    std::wstring last_err = recorder_last_error();
    if (!last_out.empty()) {
        char buf[600];
        WideCharToMultiByte(CP_UTF8, 0, last_out.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        ImGui::Text("last output: %s", buf);
    }
    if (!last_err.empty()) {
        char buf[300];
        WideCharToMultiByte(CP_UTF8, 0, last_err.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "error: %s", buf);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Hotkeys: F8 toggle GUI  |  F9 toggle record  |  Ctrl+[ slower  |  Ctrl+] faster  |  Ctrl+\\ reset");
    ImGui::End();
}

void thread_main() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"tasdll_imgui_wnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName, L"tasdll",
        WS_OVERLAPPEDWINDOW,
        80, 80, 480, 420,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        log_line("gui: CreateWindow failed");
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }

    if (!create_device(g_hwnd)) {
        DestroyWindow(g_hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_running.store(true);

    MSG msg;
    while (g_running.load() && !state().exit_requested.load()) {
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        if (quit) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        draw_ui();

        ImGui::Render();
        const float clear[4] = { 0.05f, 0.05f, 0.07f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(g_hwnd);
    UnregisterClassW(L"tasdll_imgui_wnd", GetModuleHandleW(nullptr));
    g_hwnd = nullptr;
    g_running.store(false);
}

} // anonymous namespace

bool gui_start() {
    if (g_running.load() || g_thread.joinable()) return true;
    g_thread = std::thread(thread_main);
    return true;
}

void gui_stop() {
    g_running.store(false);
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread.joinable()) g_thread.join();
}

} // namespace tasdll
