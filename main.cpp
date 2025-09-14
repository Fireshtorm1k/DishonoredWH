#include <array>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <tuple>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <windows.h>
#include "OverlayPoints.hpp"
#include "ProcHelper.hpp"
#include "MemoryScanner.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
struct Vec3 { float x,y,z; };
struct Mat3 { float m[3][3]; }; // row-major


static inline Vec3 sub(const Vec3& a, const Vec3& b){
    return {a.x-b.x, a.y-b.y, a.z-b.z};
}

// y = R * x  (R — world->camera, row*col)
static inline Vec3 mul(const Mat3& R, const Vec3& v){
    return {
        
        R.m[0][0]*v.x + R.m[0][1]*v.y + R.m[0][2]*v.z,
        R.m[1][0]*v.x + R.m[1][1]*v.y + R.m[1][2]*v.z,
        R.m[2][0]*v.x + R.m[2][1]*v.y + R.m[2][2]*v.z
    };
}


// right_is_negative = true for Dishonored 2 (world->camera)
static inline std::tuple<double, double, bool>
project_point_hFOV(const Vec3& X_world,
    const Vec3& C_cam,
    const Mat3& R_world_to_cam,
    int W, int H, double Fh_deg,
    bool right_is_negative = true)
{
    const Vec3 d = { X_world.x - C_cam.x, X_world.y - C_cam.y, X_world.z - C_cam.z };
    const Vec3 c = {
        R_world_to_cam.m[0][0] * d.x + R_world_to_cam.m[0][1] * d.y + R_world_to_cam.m[0][2] * d.z,
        R_world_to_cam.m[1][0] * d.x + R_world_to_cam.m[1][1] * d.y + R_world_to_cam.m[1][2] * d.z,
        R_world_to_cam.m[2][0] * d.x + R_world_to_cam.m[2][1] * d.y + R_world_to_cam.m[2][2] * d.z
    };
    if (c.x <= 0.0) return { NAN, NAN, false }; // вперед = +X_cam

    const double aspect = double(W) / double(H);
    const double Fh = Fh_deg * M_PI / 180.0;
    const double Fv = 2.0 * std::atan(std::tan(Fh * 0.5) / aspect);
    const double kx = (W * 0.5) / std::tan(Fh * 0.5);
    const double ky = (H * 0.5) / std::tan(Fv * 0.5);

    const double signX = right_is_negative ? -1.0 : 1.0;
    const double u = (W * 0.5) + signX * kx * (c.y / c.x);
    const double v = (H * 0.5) - ky * (c.z / c.x);

    const bool on = (u >= 0.0 && u <= W && v >= 0.0 && v <= H);
    return { u, v, on };
}

bool read_exact(HANDLE h, uintptr_t addr, void* dst, size_t sz) {
    SIZE_T br = 0;
    if (!ReadProcessMemory(h, reinterpret_cast<LPCVOID>(addr), dst, sz, &br))
        return false;
    return br == sz;
}

bool read_vec3(HANDLE h, uintptr_t addr, Vec3& out) {
    float tmp[3];
    if (!read_exact(h, addr, tmp, sizeof(tmp))) return false;
    out = { tmp[0], tmp[1], tmp[2] };
    return true;
}

float distance(const Vec3& a, const Vec3& b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}


bool read_cam_transform(HANDLE h, uintptr_t addr, Vec3& C, Mat3& R) {
    float buf[12]; // [0..2] pos, [3..11] rot (row-major)
    if (!read_exact(h, addr, buf, sizeof(buf))) return false;

    C = { buf[0], buf[1], buf[2] };

    // rot: 3 строки по 3 элемента
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            R.m[i][j] = buf[3 + i * 3 + j];

    return true;
}
HANDLE hProc;

std::atomic<bool> g_stop{ false };
void on_sigint(int) {
    if(hProc)
        CloseHandle(hProc);
    g_stop.store(true, std::memory_order_relaxed);
    exit(0);
}
int main(){
    static_assert(sizeof(Vec3) == 3 * sizeof(float));
    static_assert(sizeof(Mat3) == 9 * sizeof(float));
	static_assert(sizeof(void*) == 0x8); // x64 only

    int W = 2560, H = 1440;      // 2K (QHD)
    double Fh_deg = 110.0;       // Horizontal FOV

    uintptr_t camTransform = 0x7FF699A259A0;
    uintptr_t objPos = 0x1B542DA40D0+0x300;
    
    std::optional<int> pid = pid_by_name("Dishonored2");
    if (!pid.has_value()) {
        std::printf("Cannot find process");
    }
    hProc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid.value());
    std::signal(SIGINT, on_sigint);
    if (!hProc) {
        std::printf("OpenProcess failed, GetLastError=%lu\n", GetLastError());
        return 1;
    }
	uintptr_t movable_vptr = 0x7FF698ABDE18;
    uintptr_t usable_vptr = 0x7FF698ABEC38;
    uintptr_t idClass_vptr = 0x7FF69895C930;
	std::printf("Scanning for pointers...\n");
    std::vector<std::uint64_t> throwables = memscan::scan_value(hProc, movable_vptr);
    std::printf("Scanning complete\n");
    while (true)
    {
        Vec3 cam_pos{};
        Mat3 R{};   // world->camera (row-major)
        Vec3 obj_pos{};
        
        
        if (!read_cam_transform(hProc, camTransform, cam_pos, R)) {
            std::printf("read_cam_transform failed, GetLastError=%lu\n", GetLastError());
            CloseHandle(hProc);
            return -1;
        }
        std::vector<overlay_points::XY> pts;
        for (size_t i = 0; i < throwables.size(); i++)
        {
            
            uintptr_t vptr = 0;
            if (!read_exact(hProc, throwables[i], &vptr, sizeof(void*)))
            {
                std::printf("read_exact (vptr) failed, GetLastError=%lu\n", GetLastError());
                CloseHandle(hProc);
                return -1;
            }

            // Validate obj
            if (vptr == idClass_vptr)
                throwables.erase(throwables.begin() + static_cast<std::ptrdiff_t>(i));

            

            if (!read_vec3(hProc, throwables[i] + 0x300, obj_pos)) {
                std::printf("read_vec3 (obj) failed, GetLastError=%lu\n", GetLastError());
                CloseHandle(hProc);
                return -1;
            }

            if (distance(cam_pos, obj_pos) > 10)
                continue;
            auto [u, v, on] = project_point_hFOV(obj_pos, cam_pos, R, W, H, Fh_deg);
			pts.push_back({ (int)std::lround(u), (int)std::lround(v) });
        }

        
        
        /*std::cout << std::fixed << std::setprecision(6);
        std::cout << "u = " << u << ", v = " << v
            << "  (rounded: " << (long)std::lround(u)
            << ", " << (long)std::lround(v) << ")\n";
        std::cout << "on_screen = " << (on ? "true" : "false") << "\n";*/
        overlay_points::draw_on_monitor(pts.data(), pts.size(), 2560, 1440, 0);
        pts.clear();
        Sleep(10);
    }



    return 0;
}
