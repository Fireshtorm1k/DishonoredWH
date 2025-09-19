#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "ObjectScanner.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <cmath>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- глобалки ---
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
Present_t oPresent = nullptr;
bool g_ShowMenu = true;
HWND g_hWnd = nullptr;
WNDPROC g_OrigWndProc = nullptr;
ID3D11Device* g_Device = nullptr;
ID3D11DeviceContext* g_Context = nullptr;
ID3D11RenderTargetView* g_RTV = nullptr;
bool g_Init = false;

static ObjectScanner scanner;



extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct Vec3 {
    float x, y, z;
};
struct Mat3 { float m[3][3]; };


inline float Distance(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Projector {
    int W, H;
    double halfW, halfH;
    double kx, ky;
    double signX;
    Projector(int w, int h, float fov, bool right_is_negative = true)
        : W(w), H(h), halfW(w * 0.5), halfH(h * 0.5), signX(right_is_negative ? -1.0 : 1.0)
    {
        const float aspect = double(W) / double(H);
        const float Fh = fov * M_PI / 180.0;
        const float Fv = 2.0 * std::atan(std::tan(Fh * 0.5) / aspect);
        kx = halfW / std::tan(Fh * 0.5);
        ky = halfH / std::tan(Fv * 0.5);
    }

    // Возвращает false, если точка за спиной/вне экрана
    inline bool project(const Vec3& X_world, const Vec3& C_cam, const Mat3& R,
        float& u, float &v) const noexcept
    {
        const Vec3 d = { X_world.x - C_cam.x, X_world.y - C_cam.y, X_world.z - C_cam.z };
        const float cx = R.m[0][0] * d.x + R.m[0][1] * d.y + R.m[0][2] * d.z;
        if (cx <= 0.0) return false;
        const float cy = R.m[1][0] * d.x + R.m[1][1] * d.y + R.m[1][2] * d.z;
        const float cz = R.m[2][0] * d.x + R.m[2][1] * d.y + R.m[2][2] * d.z;
        u = halfW + signX * kx * (cy / cx);
        v = halfH - ky * (cz / cx);
        return (u >= 0.0 && u <= W && v >= 0.0 && v <= H);
    }
};


LRESULT CALLBACK WndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    return CallWindowProc(g_OrigWndProc, hWnd, msg, wParam, lParam);
}

inline float Normalize(float x, float min, float max)
{
    if (max == min) return 0.0f; // защита от деления на ноль
    return (x - min) / (max - min);
}



ImVec4 LerpColor(const ImVec4& c1, const ImVec4& c2, float t)
{
    return ImVec4(
        c1.x + (c2.x - c1.x) * t,  // R
        c1.y + (c2.y - c1.y) * t,  // G
        c1.z + (c2.z - c1.z) * t,  // B
        c1.w + (c2.w - c1.w) * t   // A
    );
}


static std::vector<uintptr_t> objectAddrs;
static Projector projector(2560, 1440, 110, true);
uintptr_t camTransform = scanner.getCameraTransform();
Vec3* camPos = reinterpret_cast<Vec3*>(camTransform);
Mat3* camRot = reinterpret_cast<Mat3*>(camTransform + sizeof(Vec3));

static uintptr_t deadEntityVptr = (uintptr_t)GetModuleHandle(NULL) + 0x1afc930;

// --- наш Present ---
HRESULT __stdcall HookPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
{
    if (!g_Init) {
        swap->GetDevice(__uuidof(ID3D11Device), (void**)&g_Device);
        g_Device->GetImmediateContext(&g_Context);

        DXGI_SWAP_CHAIN_DESC desc;
        swap->GetDesc(&desc);
        g_hWnd = desc.OutputWindow;

        ID3D11Texture2D* backBuf = nullptr;
        swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
        g_Device->CreateRenderTargetView(backBuf, nullptr, &g_RTV);
        backBuf->Release();

        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_hWnd);
        ImGui_ImplDX11_Init(g_Device, g_Context);
        g_OrigWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    
        g_Init = true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // обработка клавиш
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        g_ShowMenu = !g_ShowMenu;
        ImGui::GetIO().MouseDrawCursor = g_ShowMenu;
    }

    static float colNear[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    static float colFar[4] =  { 1.0f, 0.0f, 0.0f, 0.25f };
    static float maxDistance = 10;
	static float dotsSize = 3.0f;
    static bool checkForFlags = false;
    // отрисовка меню
    if (g_ShowMenu) {
        ImGui::Begin("Overlay Menu");
        if (ImGui::Button("Reload Cache")) {
			objectAddrs =  scanner.scanForType(ClassType::Pickup);
        }
        if (ImGui::Button("Clean List")) {
            objectAddrs.clear();
        }
        ImGui::ColorPicker4("Dots NearColor", colNear);
        ImGui::ColorPicker4("Dots FarColor", colFar);
		ImGui::SliderFloat("Max Distance", &maxDistance, 10.0f, 1000.0f);
		ImGui::SliderFloat("Dots Size", &dotsSize, 1.0f, 30.0f);
        ImGui::Checkbox("Check for flags (WIP)", &checkForFlags);
        ImGui::End();
    }
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    for (const auto& pt : objectAddrs) {
        Vec3* objPos = reinterpret_cast<Vec3*>(pt + 0x300);
        std::erase_if(objectAddrs, [](uintptr_t pt) {
            Vec3* objPos = reinterpret_cast<Vec3*>(pt + 0x300);
            return *(uintptr_t*)pt == deadEntityVptr;
            });

        if (checkForFlags && (*(uintptr_t**)(pt + 0x8) == nullptr))
            continue;
            
		float dist = Distance(*objPos, *camPos);
        if(dist > maxDistance)
			continue;
        float x, y;
            if (projector.project(*objPos, *camPos, *camRot,x,y))
            {
				float norm = Normalize(dist, 0.0f, maxDistance);

                ImVec4 col = LerpColor(ImVec4(colNear[0], colNear[1], colNear[2], colNear[3]),ImVec4(colFar[0], colFar[1], colFar[2], colFar[3]), norm);
                drawList->AddCircleFilled(ImVec2(x, y), dotsSize, ImGui::ColorConvertFloat4ToU32(col));
            }
    }
    ImGui::Render();
    g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());


    return oPresent(swap, sync, flags);
}

void HookSwapChain()
{
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L,
                      GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
                      "DummyWindow", NULL };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, "Dummy", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;

    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &swap, &dev, &fl, &ctx);

    void** vtable = *reinterpret_cast<void***>(swap);

    DWORD oldProt;
    VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    oPresent = (Present_t)vtable[8];
    vtable[8] = (void*)&HookPresent;
    VirtualProtect(&vtable[8], sizeof(void*), oldProt, &oldProt);

    swap->Release();
    dev->Release();
    ctx->Release();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
}

// --- поток старта ---
DWORD WINAPI ThreadProc(LPVOID)
{
    HookSwapChain();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    }
    return TRUE;
}
