#include <array>
#include <cmath>
#include <iostream>
#include <tuple>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <optional> // ***
#include <vector>
#include <windows.h>
//#include "OverlayPoints.hpp"
#include "ProcHelper.hpp"
#include "MemoryScanner.hpp"
#include "OverlayD3D.hpp"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Vec3 { float x, y, z; };
struct Mat3 { float m[3][3]; }; // row-major

// --- простые утилиты ---
static inline Vec3 sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

// Математика проекции с предвычисленными коэффициентами
struct Projector {
    int W, H;
    double halfW, halfH;
    double kx, ky;
    double signX;
    Projector(int w, int h, double Fh_deg, bool right_is_negative = true)
        : W(w), H(h), halfW(w * 0.5), halfH(h * 0.5), signX(right_is_negative ? -1.0 : 1.0)
    {
        const double aspect = double(W) / double(H);
        const double Fh = Fh_deg * M_PI / 180.0;
        const double Fv = 2.0 * std::atan(std::tan(Fh * 0.5) / aspect);
        kx = halfW / std::tan(Fh * 0.5);
        ky = halfH / std::tan(Fv * 0.5);
    }

    // Возвращает false, если точка за спиной/вне экрана
    inline bool project(const Vec3& X_world, const Vec3& C_cam, const Mat3& R,
        double& u, double& v) const noexcept
    {
        const Vec3 d = { X_world.x - C_cam.x, X_world.y - C_cam.y, X_world.z - C_cam.z };
        const double cx = R.m[0][0] * d.x + R.m[0][1] * d.y + R.m[0][2] * d.z;
        if (cx <= 0.0) return false;
        const double cy = R.m[1][0] * d.x + R.m[1][1] * d.y + R.m[1][2] * d.z;
        const double cz = R.m[2][0] * d.x + R.m[2][1] * d.y + R.m[2][2] * d.z;
        u = halfW + signX * kx * (cy / cx);
        v = halfH - ky * (cz / cx);
        return (u >= 0.0 && u <= W && v >= 0.0 && v <= H);
    }
};

static inline bool read_exact(HANDLE h, uintptr_t addr, void* dst, size_t sz) {
    SIZE_T br = 0;
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), dst, sz, &br)) return false;
    return br == sz;
}

static inline bool read_vec3(HANDLE h, uintptr_t addr, Vec3& out) {
    float tmp[3];
    if (!read_exact(h, addr, tmp, sizeof(tmp))) return false;
    out = { tmp[0], tmp[1], tmp[2] };
    return true;
}

static inline bool read_cam_transform(HANDLE h, uintptr_t addr, Vec3& C, Mat3& R) {
    float buf[12]; // [0..2] pos, [3..11] rot (row-major)
    if (!read_exact(h, addr, buf, sizeof(buf))) return false;
    C = { buf[0], buf[1], buf[2] };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            R.m[i][j] = buf[3 + i * 3 + j];
    return true;
}

HANDLE hProc = nullptr;
std::atomic<bool> g_stop{ false };
void on_sigint(int) {
    if (hProc) CloseHandle(hProc);
    g_stop.store(true, std::memory_order_relaxed);
    ExitProcess(0);
}

// --- Структура для батч‑чтения по страницам ---
struct ObjAddr {
    std::uint64_t addr;          // адрес объекта (где лежит vptr)
    std::uint64_t page_base;     // база страницы
    std::uint32_t off_in_page;   // смещение внутри страницы
};
HWND find_main_hwnd(DWORD pid) {
    struct Ctx { DWORD pid; HWND h = nullptr; } c{ pid, nullptr };
    EnumWindows([](HWND h, LPARAM lp)->BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
        if (wpid == c.pid && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
            c.h = h; return FALSE;
        }
        return TRUE;
        }, (LPARAM)&c);
    return c.h;
}


int main() {
    static_assert(sizeof(Vec3) == 3 * sizeof(float), "Vec3 layout");
    static_assert(sizeof(Mat3) == 9 * sizeof(float), "Mat3 layout");
    static_assert(sizeof(void*) == 0x8, "x64 only");
    
    // --- Параметры рендера / камеры ---
    const int W = 2560, H = 1440;
    const double Fh_deg = 110.0;
    const Projector proj(W, H, Fh_deg, /*right_is_negative*/true);

    // --- Известные адреса (как у вас) ---
   
    const float maxDist = 20.0f;
    const float maxDist2 = maxDist * maxDist;



    // --- Открываем процесс ---
    std::optional<int> pid = pid_by_name("Dishonored2");
    
    if (!pid) { std::printf("Cannot find process\n"); return 1; }
    hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid.value());
    if (!hProc) {
        std::printf("OpenProcess failed, GetLastError=%lu\n", GetLastError());
        return 1;
    }
    uintptr_t moduleBase = GetModuleBaseAddress(pid.value(), L"Dishonored2.exe");

    const std::uint64_t camTransform = moduleBase + 0x2BC59A0;
    const std::uint64_t posOffset = 0x300ull; // смещение Vec3 в объекте

    const std::uint64_t movable_vptr = moduleBase + 0x1C5E258;

    HWND game = pid ? find_main_hwnd((DWORD)*pid) : nullptr;
    bool ok = overlay_d3d::init_for_monitor(0);
    std::printf("overlay init: %s\n", ok ? "OK" : "FAIL");
    
    std::signal(SIGINT, on_sigint);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // немножко помогает

    // --- Скан: ускоренная версия выровненным шагом и фильтрами ---
    std::printf("Scanning for vptr=0x%llX...\n", (unsigned long long)movable_vptr);
    memscan::ScanOpts so;
    so.chunk_size = (16u << 20);           // 16 MiB
    so.only_private = true;                // только MEM_PRIVATE
    so.allow_protect = PAGE_READWRITE | PAGE_WRITECOPY; // обычно кучи
    so.align = sizeof(void*);              // 8 на x64

    std::vector<std::uint64_t> found = memscan::scan_value_aligned(hProc, movable_vptr, so);
    std::printf("Found %zu candidates\n", found.size());

    if (found.empty()) {
        std::printf("No objects found. Exiting.\n");
        CloseHandle(hProc);
        return 0;
    }

    // --- Предготовим батч‑списки, отсортируем по страницам ---
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    const std::uint64_t pageSize = si.dwPageSize ? si.dwPageSize : 0x1000;
    const std::uint64_t pageMask = ~(pageSize - 1ull);

    std::vector<ObjAddr> objs; objs.reserve(found.size());
    for (std::uint64_t a : found) {
        const std::uint64_t base = a & pageMask;
        const std::uint32_t off = static_cast<std::uint32_t>(a - base);
        objs.push_back({ a, base, off });
    }
    std::sort(objs.begin(), objs.end(), [](const ObjAddr& A, const ObjAddr& B) {
        return (A.page_base < B.page_base) || (A.page_base == B.page_base && A.off_in_page < B.off_in_page);
        });

    // --- Буферы и константы для батч‑чтения ---
    // Нам для каждого адреса нужно: vptr (8 байт по offset 0) + Vec3 по offset 0x300,
    // т.е. диапазон [0, 0x30C). Если off_in_page <= pageSize - 0x30C, всё помещается в одну страницу.
    const std::uint32_t needSpan = static_cast<std::uint32_t>(posOffset + sizeof(float) * 3); // 0x30C
    std::vector<std::uint8_t> pageBuf;
    pageBuf.resize(static_cast<size_t>(pageSize) + needSpan); // читаем страницу + хвост (если следующая страница есть)

    // --- Рабочие буферы для кадра ---
    std::vector<overlay_d3d::XY> pts; pts.reserve(std::min<std::size_t>(objs.size(), 8000));
    Vec3 cam_pos{}; Mat3 R{};
    const size_t kOverlayCap = 8000; // чуть меньше внутреннего лимита

    // --- Главный цикл ---
    while (!g_stop.load(std::memory_order_relaxed)) {
        // 1) Камера
        if (!read_cam_transform(hProc, camTransform, cam_pos, R)) {
            std::printf("read_cam_transform failed, GetLastError=%lu\n", GetLastError());
            break;
        }

        pts.clear();

        // 2) Обход по страницам (сгруппированы и отсортированы)
        std::vector<ObjAddr> alive; alive.reserve(objs.size());
        size_t i = 0;
        while (i < objs.size()) {
            const std::uint64_t curBase = objs[i].page_base;

            // Прочитаем страницу + хвост
            SIZE_T br = 0;
            const SIZE_T toRead = pageBuf.size(); // pageSize + needSpan
            BOOL ok = ReadProcessMemory(hProc, reinterpret_cast<LPCVOID>(curBase), pageBuf.data(), toRead, &br);

            // Собираем все элементы этой страницы
            size_t j = i;
            while (j < objs.size() && objs[j].page_base == curBase) {
                const ObjAddr& e = objs[j];

                bool keep = true; // жив ли адрес
                // Если чтение страницы не удалось совсем — сбросим адрес
                if (!ok && br == 0) { keep = false; goto next_item; }

                // Проверим, попадает ли нужный диапазон целиком в наш буфер чтения
                // (если объект близко к концу страницы, диапазон пересекает следующую страницу).
                if (e.off_in_page + needSpan <= br) {
                    // Берём из буфера
                    const std::uint8_t* p = pageBuf.data() + e.off_in_page;

                    const std::uint64_t vptr = *reinterpret_cast<const std::uint64_t*>(p);
                    if (vptr != movable_vptr) { keep = false; goto next_item; }

                    const float* pf = reinterpret_cast<const float*>(p + posOffset);
                    const Vec3 obj_pos{ pf[0], pf[1], pf[2] };

                    // Быстрые отсечки: дистанция^2, проекция и on-screen
                    const float dx = obj_pos.x - cam_pos.x;
                    const float dy = obj_pos.y - cam_pos.y;
                    const float dz = obj_pos.z - cam_pos.z;
                    const float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 > maxDist2) goto next_item;

                    double u, v;
                    if (!proj.project(obj_pos, cam_pos, R, u, v)) goto next_item;

                    pts.push_back({ (int)std::lround(u), (int)std::lround(v) });
                    if (pts.size() >= kOverlayCap) goto next_item; // не раздуваем оверлей
                }
                else {
                    // Фоллбек: адрес попал в край страницы — дочитаем индивидуально
                    // (это редкая ветка)
                    std::uint64_t vptr = 0;
                    if (!read_exact(hProc, e.addr, &vptr, sizeof(vptr)) || vptr != movable_vptr) { keep = false; goto next_item; }

                    Vec3 obj_pos{};
                    if (!read_vec3(hProc, e.addr + posOffset, obj_pos)) { keep = false; goto next_item; }

                    const float dx = obj_pos.x - cam_pos.x;
                    const float dy = obj_pos.y - cam_pos.y;
                    const float dz = obj_pos.z - cam_pos.z;
                    const float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 > maxDist2) goto next_item;

                    double u, v;
                    if (!proj.project(obj_pos, cam_pos, R, u, v)) goto next_item;

                    pts.push_back({ (int)std::lround(u), (int)std::lround(v) });
                    if (pts.size() >= kOverlayCap) goto next_item;
                }

            next_item:
                if (keep) alive.push_back(e);
                ++j;
            }

            i = j; // следующая страница
        }

        // Сжимаем список адресов (убрали «мертвые» за один проход, без O(n) erase)
        objs.swap(alive);

        // 3) Рисуем только видимые точки, масштаб — в нужный монитор
        if (!pts.empty()) {
            overlay_d3d::draw_points(pts.data(), pts.size(), W, H);
        }

        // Лёгкий троттлинг
        //Sleep(10);
    }

    if (hProc) CloseHandle(hProc);
	overlay_d3d::shutdown();
    return 0;
}
