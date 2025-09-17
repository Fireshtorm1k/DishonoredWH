#pragma once
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

// === ImGui ===
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

namespace overlay_d3d {

    using Microsoft::WRL::ComPtr;

    struct XY { int x, y; };

    // Настройки кружка
#ifndef OD3D_DOT_RADIUS_PX
#define OD3D_DOT_RADIUS_PX 4.0f
#endif
#ifndef OD3D_DOT_COLOR_RGBA   // RGBA 0..1
#define OD3D_DOT_COLOR_RGBA 1.0f, 1.0f, 0.0f, 1.0f // жёлтая, непрозрачная
#endif

// --- Общий флаг "панель видима" + прямоугольник панели (в координатах клиентской области) ---
    static std::atomic<bool> s_ui_visible{ false };
    static RECT              s_ui_rect{ 0,0,0,0 };

    // Наше окно — полностью «сквозное», кроме области панели (когда она открыта).
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        // Передаём сообщения в бэкенд ImGui (если контекст уже создан)
        if (ImGui::GetCurrentContext())
            ImGui_ImplWin32_WndProcHandler(h, m, w, l);

        switch (m) {
        case WM_NCHITTEST: {
            // Если панель открыта и курсор внутри её прямоугольника — принимаем события
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &pt);
            RECT r = s_ui_rect;
            if (s_ui_visible &&
                pt.x >= r.left && pt.x < r.right &&
                pt.y >= r.top && pt.y < r.bottom) {
                return HTCLIENT;           // кликаем по панели
            }
            return HTTRANSPARENT;          // клики сквозь в остальной области
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // не активировать окно при клике
        case WM_SETCURSOR: {
            // Стрелка мыши над панелью, скрытый курсор вне её
            if (s_ui_visible) {
                POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
                if (p.x >= s_ui_rect.left && p.x < s_ui_rect.right &&
                    p.y >= s_ui_rect.top && p.y < s_ui_rect.bottom) {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                    return TRUE;
                }
            }
            SetCursor(nullptr);
            return TRUE;
        }
        case WM_DESTROY:       PostQuitMessage(0); return 0;
        default:               return DefWindowProcW(h, m, w, l);
        }
    }

    struct Ctx {
        // window/placement
        HWND hwnd = nullptr;
        RECT mon_rc{ 0,0,0,0 };

        // d3d/dcomp
        ComPtr<ID3D11Device>        dev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<IDXGISwapChain1>     sc;
        ComPtr<IDCompositionDevice> dcomp;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual> visual;

        int scW = 0, scH = 0;

        // ImGui
        bool   imguiReady = false;
        bool   uiVisible = false;
        bool   homePrevDown = false;
        ImVec4 dotColor = ImVec4(OD3D_DOT_COLOR_RGBA);

        // окно монитора по индексу
        static RECT monitor_rect(int index) {
            struct Rec { RECT rc; bool primary; };
            std::vector<Rec> mons;
            EnumDisplayMonitors(nullptr, nullptr,
                [](HMONITOR h, HDC, LPRECT, LPARAM p)->BOOL {
                    MONITORINFO mi{ sizeof(mi) };
                    if (GetMonitorInfoW(h, &mi))
                        ((std::vector<Rec>*)p)->push_back({ mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
                    return TRUE;
                }, (LPARAM)&mons);
            if (mons.empty()) {
                RECT r{ 0,0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
                return r;
            }
            std::stable_sort(mons.begin(), mons.end(), [](auto& a, auto& b) { return a.primary && !b.primary; });
            if (index < 0 || index >= (int)mons.size()) index = 0;
            return mons[index].rc;
        }

        bool create_window_on_monitor(int monIndex) {
            mon_rc = monitor_rect(monIndex);

            WNDCLASSEXW wc{ sizeof(wc) };
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"od3d_class";
            wc.lpfnWndProc = &WndProc;
            wc.hCursor = nullptr;
            RegisterClassExW(&wc);

            DWORD ex = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
            DWORD st = WS_POPUP;

            hwnd = CreateWindowExW(
                ex, wc.lpszClassName, L"", st,
                mon_rc.left, mon_rc.top,
                mon_rc.right - mon_rc.left, mon_rc.bottom - mon_rc.top,
                nullptr, nullptr, wc.hInstance, nullptr);

            if (!hwnd) return false;

            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, mon_rc.left, mon_rc.top,
                mon_rc.right - mon_rc.left, mon_rc.bottom - mon_rc.top,
                SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);

            return true;
        }

        bool init_d3d_imgui() {
            // --- D3D11 device/context ---
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            D3D_FEATURE_LEVEL flvls[] = {
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
            };
            D3D_FEATURE_LEVEL got{};
            if (FAILED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                flvls, _countof(flvls), D3D11_SDK_VERSION,
                &dev, &got, &ctx))) return false;

            // --- DirectComposition + swapchain ---
            ComPtr<IDXGIDevice> dxgiDev; dev.As(&dxgiDev);
            if (!dxgiDev) return false;

            if (FAILED(DCompositionCreateDevice(dxgiDev.Get(),
                __uuidof(IDCompositionDevice), (void**)&dcomp))) return false;

            if (FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target))) return false;
            if (FAILED(dcomp->CreateVisual(&visual))) return false;

            ComPtr<IDXGIAdapter> ad;
            dxgiDev->GetAdapter(&ad);
            ComPtr<IDXGIFactory2> fac;
            ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = mon_rc.right - mon_rc.left;
            desc.Height = mon_rc.bottom - mon_rc.top;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = { 1, 0 };
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

            if (FAILED(fac->CreateSwapChainForComposition(dev.Get(), &desc, nullptr, &sc))) return false;

            visual->SetContent(sc.Get());
            target->SetRoot(visual.Get());
            dcomp->Commit();

            scW = desc.Width; scH = desc.Height;

            // --- Dear ImGui ---
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            {
                ImGuiIO& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.LogFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // курсор управляем сами
            }
            ImGui::StyleColorsDark();
            if (!ImGui_ImplWin32_Init(hwnd)) return false;
            if (!ImGui_ImplDX11_Init(dev.Get(), ctx.Get())) return false;

            imguiReady = true;
            return true;
        }

        static void pump_window_messages(HWND h) {
            MSG msg;
            // Забираем только сообщения нашего окна
            while (PeekMessageW(&msg, h, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        bool present_points(const XY* pts, size_t n, int resX, int resY) {
            if (!hwnd || !sc || !imguiReady) return false;

            // Минимальная обработка сообщений окна (для работы ввода ImGui)
            pump_window_messages(hwnd);

            // --- Тоггл панели по клавише Home (глобально) ---
            const bool homeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
            if (homeDown && !homePrevDown) {
                uiVisible = !uiVisible;
            }
            homePrevDown = homeDown;
            s_ui_visible.store(uiVisible, std::memory_order_relaxed);

            // Бэкбуфер + RTV (без кеширования — безопасно для flip swapchain)
            ComPtr<ID3D11Texture2D> bb;
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
            ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) return false;

            // Очистка в прозрачный
            FLOAT clear[4] = { 0,0,0,0 };
            ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            ctx->ClearRenderTargetView(rtv.Get(), clear);

            // Viewport
            D3D11_VIEWPORT vp{};
            vp.TopLeftX = 0; vp.TopLeftY = 0;
            vp.Width = (FLOAT)scW; vp.Height = (FLOAT)scH;
            vp.MinDepth = 0; vp.MaxDepth = 1;
            ctx->RSSetViewports(1, &vp);

            // --- ImGui кадр ---
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Панель цвета (в правом верхнем углу, автосайз)
            if (uiVisible) {
                ImGui::SetNextWindowPos(ImVec2((float)scW - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                ImGuiWindowFlags wf = ImGuiWindowFlags_AlwaysAutoResize
                    | ImGuiWindowFlags_NoSavedSettings
                    | ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoNav;

                bool open = uiVisible;
                if (ImGui::Begin(u8"Overlay • Color", &open, wf)) {
                    ImGui::TextUnformatted(u8"Цвет точек");
                    ImGuiColorEditFlags cf = ImGuiColorEditFlags_HDR
                        | ImGuiColorEditFlags_AlphaBar
                        | ImGuiColorEditFlags_DisplayRGB
                        | ImGuiColorEditFlags_NoSidePreview;
                    ImGui::ColorPicker4("##dot_color", &dotColor.x, cf);

                    // Обновим актуальный прямоугольник панели для хит-теста окна
                    ImVec2 p = ImGui::GetWindowPos();
                    ImVec2 s = ImGui::GetWindowSize();
                    s_ui_rect = { (LONG)p.x, (LONG)p.y, (LONG)(p.x + s.x), (LONG)(p.y + s.y) };
                }
                ImGui::End();
                uiVisible = open;
                s_ui_visible.store(uiVisible, std::memory_order_relaxed);
            }
            else {
                // Панели нет — прямоугольник пустой (клики везде сквозь)
                s_ui_rect = RECT{ 0,0,0,0 };
            }

            // Рисуем точки
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const int MW = mon_rc.right - mon_rc.left;
            const int MH = mon_rc.bottom - mon_rc.top;
            int safeResX = (resX > 0) ? resX : MW;
            int safeResY = (resY > 0) ? resY : MH;

            const ImU32 col = ImGui::GetColorU32(dotColor);

            for (size_t i = 0; i < n; ++i) {
                float x = (float)pts[i].x * (float)MW / (float)safeResX;
                float y = (float)pts[i].y * (float)MH / (float)safeResY;
                dl->AddCircleFilled(ImVec2(x, y), (float)OD3D_DOT_RADIUS_PX, col, 0);
            }

            ImGui::Render();
            ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            // Present + Commit композиции
            sc->Present(0, 0);
            dcomp->Commit();
            return true;
        }
    };

    static Ctx& g() { static Ctx c; return c; }

    // --- Public API (без изменений) --------------------------------------------

    inline bool init_for_monitor(int monitorIndex) {
        auto& c = g();
        if (!c.create_window_on_monitor(monitorIndex)) return false;
        return c.init_d3d_imgui();
    }

    inline bool draw_points(const XY* pts, size_t n, int resX, int resY) {
        return g().present_points(pts, n, resX, resY);
    }

    inline void shutdown() {
        auto& c = g();
        if (c.imguiReady) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            c.imguiReady = false;
        }
        if (c.hwnd) { DestroyWindow(c.hwnd); c.hwnd = nullptr; }
        c.sc.Reset();
        c.dev.Reset(); c.ctx.Reset();
        c.dcomp.Reset(); c.target.Reset(); c.visual.Reset();
    }

} // namespace overlay_d3d
