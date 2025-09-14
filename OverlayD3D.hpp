#pragma once
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace overlay_d3d {

using Microsoft::WRL::ComPtr;

struct XY { int x, y; };

// Настройки кружка
#ifndef OD3D_DOT_RADIUS_PX
#define OD3D_DOT_RADIUS_PX 6.0f
#endif
#ifndef OD3D_DOT_COLOR_RGBA   // обычный RGBA в 0..1 (премультиплирование сделаем в PS)
#define OD3D_DOT_COLOR_RGBA 1.0f, 1.0f, 0.0f, 1.0f // жёлтая, непрозрачная
#endif

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST:     return HTTRANSPARENT;   // клики сквозь
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // не активировать
    case WM_SETCURSOR:     return FALSE;           // НЕ управляем курсором
    case WM_DESTROY:       PostQuitMessage(0); return 0;
    default:               return DefWindowProcW(h, m, w, l);
    }
}

struct Ctx {
    // window/placement
    HWND hwnd = nullptr;
    RECT mon_rc{ 0,0,0,0 };

    // d3d/dcomp
    ComPtr<ID3D11Device>            dev;
    ComPtr<ID3D11DeviceContext>     ctx;
    ComPtr<IDXGISwapChain1>         sc;
    ComPtr<IDCompositionDevice>     dcomp;
    ComPtr<IDCompositionTarget>     target;
    ComPtr<IDCompositionVisual>     visual;

    // GPU state
    ComPtr<ID3D11BlendState>        blendPM;
    ComPtr<ID3D11InputLayout>       il;
    ComPtr<ID3D11Buffer>            vbQuad;
    ComPtr<ID3D11Buffer>            ibQuad;
    ComPtr<ID3D11Buffer>            instBuf;
    ComPtr<ID3D11VertexShader>      vs;
    ComPtr<ID3D11PixelShader>       ps;
    ComPtr<ID3D11Buffer>            cbFrame;

    int scW=0, scH=0, instCapacity=0;

    // окно монитора по индексу
    static RECT monitor_rect(int index) {
        struct Rec { RECT rc; bool primary; };
        std::vector<Rec> mons;
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR h, HDC, LPRECT, LPARAM p)->BOOL {
                MONITORINFO mi{ sizeof(mi) };
                if (GetMonitorInfoW(h, &mi))
                    ((std::vector<Rec>*)p)->push_back({ mi.rcMonitor, (mi.dwFlags&MONITORINFOF_PRIMARY)!=0 });
                return TRUE;
            }, (LPARAM)&mons);
        if (mons.empty()) {
            RECT r{ 0,0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            return r;
        }
        std::stable_sort(mons.begin(), mons.end(), [](auto&a, auto&b) { return a.primary && !b.primary; });
        if (index < 0 || index >= (int)mons.size()) index = 0;
        return mons[index].rc;
    }

    bool create_window_on_monitor(int monIndex /*, HWND owner — убрать */) {
        mon_rc = monitor_rect(monIndex);

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"od3d_class";
        wc.lpfnWndProc = &WndProc;
        wc.hCursor = nullptr;
        RegisterClassExW(&wc);

        DWORD ex = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        DWORD st = WS_POPUP;

        hwnd = CreateWindowExW(
            ex, wc.lpszClassName, L"", st,
            mon_rc.left, mon_rc.top,
            mon_rc.right - mon_rc.left, mon_rc.bottom - mon_rc.top,
            /*owner*/ nullptr, nullptr, wc.hInstance, nullptr);  // <— owner = nullptr

        if (!hwnd) return false;

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, mon_rc.left, mon_rc.top,
            mon_rc.right - mon_rc.left, mon_rc.bottom - mon_rc.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);

        return true;
    }



    bool init_d3d() {
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

        // DXGI
        ComPtr<IDXGIDevice> dxgiDev; dev.As(&dxgiDev);
        if (!dxgiDev) return false;

        if (FAILED(DCompositionCreateDevice(dxgiDev.Get(),
            __uuidof(IDCompositionDevice), (void**)&dcomp))) return false;

        if (FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target))) return false;
        if (FAILED(dcomp->CreateVisual(&visual))) return false;

        // Swap chain для композиции
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

        // Blend: premultiplied
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bd, &blendPM))) return false;

        // Шейдеры (VS/PS)
        static const char* kHlsl = R"(
cbuffer CBFrame : register(b0) {
  float2 Viewport;      // width, height в пикселях
  float2 InvViewport;   // 1/width, 1/height
  float4 DotColor;      // RGBA 0..1
}
struct VSIn {
  float2 Local : POSITION; // [-1..1] квадрат
  float2 Center: CENTER;   // центр в пикселях
  float  Radius: RADIUS;   // радиус в пикселях
};
struct VSOut {
  float4 pos   : SV_POSITION;
  float2 local : LOCAL; // передадим в PS для круга
};
VSOut VSMain(VSIn i) {
  VSOut o;
  float2 px = i.Center + i.Local * i.Radius; // позиция вершины в пикселях
  float2 ndc = float2(px.x * (2.0*InvViewport.x) - 1.0,
                      1.0 - px.y * (2.0*InvViewport.y)); // (y вниз в пикселях)
  o.pos = float4(ndc, 0.0, 1.0);
  o.local = i.Local; // для оценки расстояния до центра
  return o;
}
float4 PSMain(VSOut i) : SV_Target {
  float r = length(i.local); // 0..sqrt(2)
  // плавная грань круга возле r=1
  float alpha = saturate(1.0 - smoothstep(0.95, 1.0, r));
  // premultiplied: rgb *= alpha
  float4 c = DotColor;
  c.rgb *= alpha; c.a *= alpha;
  return c;
}
)";

        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", 0, 0, &vsb, &err))) return false;
        if (FAILED(D3DCompile(kHlsl, strlen(kHlsl), nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0", 0, 0, &psb, &err))) return false;
        if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs))) return false;
        if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps))) return false;

        // Input layout: VB0 = quad, VB1 = inst
        D3D11_INPUT_ELEMENT_DESC ilDesc[] = {
          { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA,   0 }, // Local
          { "CENTER",   0, DXGI_FORMAT_R32G32_FLOAT, 1, 0,   D3D11_INPUT_PER_INSTANCE_DATA, 1 }, // Center
          { "RADIUS",   0, DXGI_FORMAT_R32_FLOAT,    1, 8,   D3D11_INPUT_PER_INSTANCE_DATA, 1 }, // Radius
        };
        if (FAILED(dev->CreateInputLayout(ilDesc, _countof(ilDesc),
            vsb->GetBufferPointer(), vsb->GetBufferSize(), &il))) return false;

        // Quad (локальный квадрат [-1..1])
        struct V2 { float x, y; };
        V2 quad[4] = { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} };
        uint16_t idx[6] = { 0,1,2, 0,2,3 };

        D3D11_BUFFER_DESC bdv{}; bdv.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bdv.ByteWidth = sizeof(quad);
        D3D11_SUBRESOURCE_DATA ssv{ quad };
        if (FAILED(dev->CreateBuffer(&bdv, &ssv, &vbQuad))) return false;

        D3D11_BUFFER_DESC bdi{}; bdi.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bdi.ByteWidth = sizeof(idx);
        D3D11_SUBRESOURCE_DATA ssi{ idx };
        if (FAILED(dev->CreateBuffer(&bdi, &ssi, &ibQuad))) return false;

        // Instance buffer (динамический, увеличим по мере надобности)
        instCapacity = 0;

        // CBFrame
        D3D11_BUFFER_DESC cbd{}; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.ByteWidth = 64; cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cbFrame))) return false;

        return true;
    }

    bool ensure_instance_capacity(int want) {
        if (want <= instCapacity) return true;
        // округлим вверх до ближайшей степени 2
        int cap = 1; while (cap < want) cap <<= 1;
        struct Inst { float cx, cy, r, pad; };
        D3D11_BUFFER_DESC bd{};
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.ByteWidth = cap * sizeof(Inst);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> nb;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &nb))) return false;
        instBuf = nb;
        instCapacity = cap;
        return true;
    }

    bool present_points(const XY* pts, size_t n, int resX, int resY) {
        if (!hwnd || !sc) return false;

        // Мапим инстансы
        if (n > 0) {
            if (!ensure_instance_capacity((int)n)) return false;

            struct Inst { float cx, cy, r, pad; };
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(ctx->Map(instBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
            auto* out = (Inst*)map.pData;

            const int MW = mon_rc.right - mon_rc.left;
            const int MH = mon_rc.bottom - mon_rc.top;
            for (size_t i = 0; i < n; ++i) {
                float x = (float)pts[i].x * (float)MW / (float)resX;   // БЕЗ mon_rc.left
                float y = (float)pts[i].y * (float)MH / (float)resY;   // БЕЗ mon_rc.top
                out[i].cx = x;
                out[i].cy = y;
                out[i].r = OD3D_DOT_RADIUS_PX;
                out[i].pad = 0.0f;
            }
            ctx->Unmap(instBuf.Get(), 0);
        }

        // Backbuffer RTV (без кеширования — безопасно с flip swapchain)
        ComPtr<ID3D11Texture2D> bb;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) return false;


        // Настроим пайплайн
        FLOAT clr[4] = { 0,0,0,0 };  // прозрачный фон
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->ClearRenderTargetView(rtv.Get(), clr);

        D3D11_VIEWPORT vp{};
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        vp.Width = (FLOAT)scW; vp.Height = (FLOAT)scH;
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);

        // Константы кадра
        D3D11_MAPPED_SUBRESOURCE cbm{};
        if (SUCCEEDED(ctx->Map(cbFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbm))) {
            struct CB { float Viewport[2]; float InvViewport[2]; float DotColor[4]; };
            auto* c = (CB*)cbm.pData;
            c->Viewport[0] = (float)scW; c->Viewport[1] = (float)scH;
            c->InvViewport[0] = 1.0f / (float)scW; c->InvViewport[1] = 1.0f / (float)scH;
            c->DotColor[0] = OD3D_DOT_COLOR_RGBA; // RGBA
            ctx->Unmap(cbFrame.Get(), 0);
        }
        ctx->VSSetConstantBuffers(0, 1, cbFrame.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cbFrame.GetAddressOf());
        // IA + шейдеры
        UINT stride0 = sizeof(float) * 2, off0 = 0;
        ID3D11Buffer* vbs[2] = { vbQuad.Get(), instBuf.Get() };
        UINT strides[2] = { stride0, (UINT)sizeof(float)*4 };
        UINT offsets[2] = { 0, 0 };

        ctx->IASetInputLayout(il.Get());
        ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        ctx->IASetIndexBuffer(ibQuad.Get(), DXGI_FORMAT_R16_UINT, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);

        float blendF[4] = { 0,0,0,0 };
        ctx->OMSetBlendState(blendPM.Get(), blendF, 0xFFFFFFFF);

        if (n > 0) {
            ctx->DrawIndexedInstanced(6, (UINT)n, 0, 0, 0);
        }
        // Present (композитор сам синхронизирует)
        sc->Present(0, 0);
        dcomp->Commit();
        return true;
    }
};

static Ctx& g() { static Ctx c; return c; }


// В init_for_monitor добавим параметр owner:
inline bool init_for_monitor(int monitorIndex) {
    auto& c = g();
    if (!c.create_window_on_monitor(monitorIndex)) return false;
    return c.init_d3d();
}


inline bool draw_points(const XY* pts, size_t n, int resX, int resY) {
    return g().present_points(pts, n, resX, resY);
}

inline void shutdown() {
    auto& c = g();
    if (c.hwnd) DestroyWindow(c.hwnd), c.hwnd = nullptr;
    c.dev.Reset(); c.ctx.Reset(); c.sc.Reset();
    c.dcomp.Reset(); c.target.Reset(); c.visual.Reset();
    c.blendPM.Reset(); c.il.Reset(); c.vbQuad.Reset(); c.ibQuad.Reset();
    c.instBuf.Reset(); c.vs.Reset(); c.ps.Reset(); c.cbFrame.Reset();
}

} // namespace overlay_d3d
