// overlay_points.hpp
#pragma once
// Header-only оверлей из точек, поверх всех окон (Windows).
// Линкуйте: user32, gdi32.
//
// API:
//   overlay_points::draw(const XY* pts, size_t n);                     // экранные координаты (origin=Primary (0,0))
//   overlay_points::draw(const XY* pts, size_t n, int resX, int resY); // точки в "кадре" resX×resY, масштаб в Primary
//   overlay_points::draw_on_monitor(const XY* pts, size_t n,
//                                   int resX, int resY, int monitorIndex); // масштаб в монитор index (0 — Primary)
//   overlay_points::hide();                                            // убрать все точки
//
// Настраиваемые макросы (переопределить до инклуда):
//   OVERLAY_DOT_SIZE_PX (по умолчанию 11), OVERLAY_DOT_ARGB (по умолчанию 0xFFFF3B30)

#include <cstdint>
#include <cstddef>

namespace overlay_points {

    struct XY { int x; int y; };

#ifndef OVERLAY_DOT_SIZE_PX
#define OVERLAY_DOT_SIZE_PX 11
#endif

#ifndef OVERLAY_DOT_ARGB
#define OVERLAY_DOT_ARGB 0xFFFF3B30u
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <vector>
#include <algorithm>

    namespace detail {

        constexpr int  kDotSize = OVERLAY_DOT_SIZE_PX;
        constexpr int  kDotR = kDotSize / 2;

        constexpr UINT kMsgSetListAbs = WM_APP + 10;
        constexpr UINT kMsgHide = WM_APP + 2;
        constexpr UINT kMsgQuit = WM_APP + 3;

        struct GdiHandleCloser {
            void operator()(HBITMAP h) const noexcept { if (h) DeleteObject(h); }
            void operator()(HDC h)     const noexcept { if (h) DeleteDC(h); }
        };
        template <class T, class D> class unique_win {
        public:
            unique_win() noexcept : h_(nullptr) {}
            explicit unique_win(T h) noexcept : h_(h) {}
            ~unique_win() { reset(); }
            T get() const noexcept { return h_; }
            T release() noexcept { T t = h_; h_ = nullptr; return t; }
            void reset(T h = nullptr) noexcept { if (h_) D{}(h_); h_ = h; }
            operator bool() const noexcept { return h_ != nullptr; }
        private: T h_;
        };
        using unique_hdc = unique_win<HDC, GdiHandleCloser>;
        using unique_hbmp = unique_win<HBITMAP, GdiHandleCloser>;

        struct Ctx {
            unique_hdc  memDC;
            unique_hbmp dib;
        };

        struct MonRec { RECT rc; bool primary; };

        inline BOOL CALLBACK enum_proc_(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
            auto v = reinterpret_cast<std::vector<MonRec>*>(lp);
            MONITORINFO mi; mi.cbSize = sizeof(mi);
            if (GetMonitorInfo(hMon, &mi))
                v->push_back({ mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
            return TRUE;
        }

        inline RECT get_monitor_rect_(int index) {
            std::vector<MonRec> mons;
            EnumDisplayMonitors(nullptr, nullptr, &enum_proc_, reinterpret_cast<LPARAM>(&mons));
            if (mons.empty()) {
                RECT r{ 0,0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
                return r;
            }
            std::stable_sort(mons.begin(), mons.end(),
                [](const MonRec& a, const MonRec& b) { return a.primary && !b.primary; });
            if (index < 0 || index >= static_cast<int>(mons.size())) index = 0;
            return mons[index].rc;
        }

        inline int clampi_(int v, int lo, int hi) {
            if (v < lo) return lo;
            if (v > hi) return hi;
            return v;
        }

        inline void build_bitmap(Ctx& ctx) {
            BITMAPV5HEADER bi{}; // top-down, 32bpp, ARGB
            bi.bV5Size = sizeof(BITMAPV5HEADER);
            bi.bV5Width = kDotSize;
            bi.bV5Height = -kDotSize;
            bi.bV5Planes = 1;
            bi.bV5BitCount = 32;
            bi.bV5Compression = BI_BITFIELDS;
            bi.bV5RedMask = 0x00FF0000;
            bi.bV5GreenMask = 0x0000FF00;
            bi.bV5BlueMask = 0x000000FF;
            bi.bV5AlphaMask = 0xFF000000;

            HDC screen = GetDC(nullptr);
            ctx.memDC.reset(CreateCompatibleDC(screen));
            ReleaseDC(nullptr, screen);

            void* bits = nullptr;
            ctx.dib.reset(CreateDIBSection(ctx.memDC.get(), reinterpret_cast<BITMAPINFO*>(&bi),
                DIB_RGB_COLORS, &bits, nullptr, 0));
            SelectObject(ctx.memDC.get(), ctx.dib.get());

            const uint32_t color = OVERLAY_DOT_ARGB;
            auto* p = static_cast<uint32_t*>(bits);
            for (int j = 0; j < kDotSize; ++j) {
                for (int i = 0; i < kDotSize; ++i) {
                    const int dx = i - kDotR;
                    const int dy = j - kDotR;
                    p[j * kDotSize + i] = (dx * dx + dy * dy <= kDotR * kDotR) ? color : 0u;
                }
            }
        }

        inline BOOL set_per_monitor_dpi_awareness() {
            using SetThreadDpiAwarenessContext_t = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            if (!user32) return FALSE;
            auto fn = reinterpret_cast<SetThreadDpiAwarenessContext_t>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
            if (!fn) return FALSE;
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            const DPI_AWARENESS_CONTEXT PMA_V2 = (DPI_AWARENESS_CONTEXT)-4;
#else
            const DPI_AWARENESS_CONTEXT PMA_V2 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
#endif
            return fn(PMA_V2) != nullptr;
        }

        // Пакет координат для межпоточной передачи (один блок памяти)
        struct PointsPkt {
            UINT32 n;
            POINT  pts[1]; // фактический массив из n элементов
        };

        class OverlayThread {
        public:
            static OverlayThread& instance() {
                static OverlayThread inst;
                return inst;
            }

            bool set_points_abs(const POINT* pts, size_t n) {
                ensure_started_();
                HWND w = ctrl_.load(std::memory_order_acquire);
                if (!w) return false;

                if (n > kMaxPoints) n = kMaxPoints;
                size_t sz = offsetof(PointsPkt, pts) + n * sizeof(POINT);
                auto* pkt = static_cast<PointsPkt*>(HeapAlloc(GetProcessHeap(), 0, sz));
                if (!pkt) return false;
                pkt->n = static_cast<UINT32>(n);
                if (n) memcpy(pkt->pts, pts, n * sizeof(POINT));

                // передаем владение блоком в UI-поток; он вызовет HeapFree
                return !!PostMessageW(w, kMsgSetListAbs, reinterpret_cast<WPARAM>(pkt), 0);
            }

            bool hide() {
                HWND w = ctrl_.load(std::memory_order_acquire);
                if (!w) return false;
                return !!PostMessageW(w, kMsgHide, 0, 0);
            }

            ~OverlayThread() {
                HWND w = ctrl_.load(std::memory_order_acquire);
                if (w) {
                    PostMessageW(w, kMsgQuit, 0, 0);
                    if (thread_) {
                        WaitForSingleObject(thread_, 2000);
                        CloseHandle(thread_);
                    }
                }
                if (ready_) CloseHandle(ready_);
            }

            OverlayThread(const OverlayThread&) = delete;
            OverlayThread& operator=(const OverlayThread&) = delete;

        private:
            OverlayThread() = default;

            // --- окно-контроллер ---
            static LRESULT CALLBACK CtrlWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                auto self = reinterpret_cast<OverlayThread*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
                if (!self && msg != WM_NCCREATE) return DefWindowProcW(hWnd, msg, wParam, lParam);

                switch (msg) {
                case WM_NCCREATE: {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
                    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                    return TRUE;
                }
                case kMsgHide: {
                    self->destroy_all_dot_windows_();
                    return 0;
                }
                case kMsgSetListAbs: {
                    auto* pkt = reinterpret_cast<PointsPkt*>(wParam);
                    self->apply_points_(*pkt);      // создать/удалить/переместить
                    HeapFree(GetProcessHeap(), 0, pkt);
                    return 0;
                }
                case kMsgQuit: {
                    self->destroy_all_dot_windows_();
                    DestroyWindow(hWnd);
                    return 0;
                }
                case WM_DESTROY: {
                    PostQuitMessage(0);
                    return 0;
                }
                }
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            }

            // --- окна-точек: мышь пропускаем сквозь них ---
            static LRESULT CALLBACK DotWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
                if (msg == WM_NCHITTEST) return HTTRANSPARENT;
                return DefWindowProcW(hWnd, msg, wParam, lParam);
            }

            void ensure_started_() {
                if (ctrl_.load(std::memory_order_acquire)) return;
                if (!ready_) ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                thread_ = CreateThread(nullptr, 0, &OverlayThread::thread_proc_, this, 0, nullptr);
                if (thread_) { WaitForSingleObject(ready_, 2000); }
            }

            static DWORD WINAPI thread_proc_(LPVOID param) {
                auto self = static_cast<OverlayThread*>(param);

                set_per_monitor_dpi_awareness();
                build_bitmap(self->ctx_);

                const wchar_t kCtrlCls[] = L"overlay_points_ctrl_cls";
                const wchar_t kDotCls[] = L"overlay_points_dot_cls";

                // Регистрируем классы
                {
                    WNDCLASSEXW wc{};
                    wc.cbSize = sizeof(wc);
                    wc.lpfnWndProc = &CtrlWndProc;
                    wc.hInstance = GetModuleHandleW(nullptr);
                    wc.lpszClassName = kCtrlCls;
                    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                    RegisterClassExW(&wc);
                }
                {
                    WNDCLASSEXW wc{};
                    wc.cbSize = sizeof(wc);
                    wc.lpfnWndProc = &DotWndProc;
                    wc.hInstance = GetModuleHandleW(nullptr);
                    wc.lpszClassName = kDotCls;
                    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                    RegisterClassExW(&wc);
                }

                // Создаем невидимое контроллер-окно (вне экрана)
                HWND ctrl = CreateWindowExW(
                    WS_EX_TOOLWINDOW, kCtrlCls, L"", WS_POPUP,
                    -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), self);
                self->ctrl_.store(ctrl, std::memory_order_release);
                SetEvent(self->ready_);

                // Цикл сообщений
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                // Уборка
                self->destroy_all_dot_windows_();
                UnregisterClassW(kDotCls, GetModuleHandleW(nullptr));
                UnregisterClassW(kCtrlCls, GetModuleHandleW(nullptr));
                return 0;
            }

            void destroy_all_dot_windows_() {
                for (HWND w : dot_windows_) {
                    if (w) DestroyWindow(w);
                }
                dot_windows_.clear();
            }

            // Применяем новый список точек: окна — ровно по числу точек.
            void apply_points_(const PointsPkt& pkt) {
                const UINT32 n = pkt.n;

                // 1) Подогнать количество окон
                if (dot_windows_.size() < n) {
                    // добираем недостающие
                    const wchar_t kDotCls[] = L"overlay_points_dot_cls";
                    for (size_t i = dot_windows_.size(); i < n; ++i) {
                        HWND w = CreateWindowExW(
                            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kDotCls, L"", WS_POPUP,
                            -32000, -32000, kDotSize, kDotSize,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                        dot_windows_.push_back(w);
                    }
                }
                else if (dot_windows_.size() > n) {
                    // удаляем лишние, «забываем» старые
                    for (size_t i = n; i < dot_windows_.size(); ++i) {
                        if (dot_windows_[i]) DestroyWindow(dot_windows_[i]);
                    }
                    dot_windows_.resize(n);
                }

                // 2) Разместить/показать каждую точку
                SIZE  sz = { kDotSize, kDotSize };
                POINT src = { 0, 0 };
                BLENDFUNCTION bf{};
                bf.BlendOp = AC_SRC_OVER;
                bf.SourceConstantAlpha = 255;
                bf.AlphaFormat = AC_SRC_ALPHA;

                for (UINT32 i = 0; i < n; ++i) {
                    HWND w = dot_windows_[i];
                    if (!w) continue;
                    POINT dst = { pkt.pts[i].x - kDotR, pkt.pts[i].y - kDotR };

                    SetWindowPos(w, HWND_TOPMOST, dst.x, dst.y, sz.cx, sz.cy,
                        SWP_NOACTIVATE | SWP_SHOWWINDOW);

                    UpdateLayeredWindow(w, nullptr, &dst, &sz, ctx_.memDC.get(), &src,
                        0, &bf, ULW_ALPHA);
                }
            }

            static constexpr size_t kMaxPoints = 8192; // защитный предел

            std::atomic<HWND> ctrl_{ nullptr };
            HANDLE thread_ = nullptr;
            HANDLE ready_ = nullptr;

            Ctx ctx_;
            std::vector<HWND> dot_windows_;
        };

        // --- утилиты масштабирования в прямоугольник ---
        inline POINT scale_one_into_rect_(int x, int y, int rx, int ry, const RECT& r) {
            POINT p{ r.left, r.top };
            if (rx <= 0 || ry <= 0) return p;
            const int w = r.right - r.left;
            const int h = r.bottom - r.top;
            long long sxN = 1LL * x * w + rx / 2;
            long long syN = 1LL * y * h + ry / 2;
            p.x = r.left + static_cast<int>(sxN / rx);
            p.y = r.top + static_cast<int>(syN / ry);
            p.x = clampi_(p.x, r.left, r.right - 1);
            p.y = clampi_(p.y, r.top, r.bottom - 1);
            return p;
        }

        inline bool to_points_abs_(const XY* pts, size_t n, int rx, int ry, const RECT& r,
            std::vector<POINT>& out) {
            out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                out[i] = scale_one_into_rect_(pts[i].x, pts[i].y, rx, ry, r);
            }
            return true;
        }

    } // namespace detail

    // ---------------- Публичный API ----------------

    // 1) Абсолютные экранные координаты (origin — левый верх Primary).
    inline bool draw(const XY* pts, size_t n) {
        if (!pts && n) return false;
        // конвертируем XY -> POINT без копии координат
        std::vector<POINT> tmp; tmp.resize(n);
        for (size_t i = 0; i < n; ++i) { tmp[i].x = pts[i].x; tmp[i].y = pts[i].y; }
        return detail::OverlayThread::instance().set_points_abs(tmp.data(), n);
    }

    // 2) Масштабирование из "кадра" resX×resY в Primary.
    inline bool draw(const XY* pts, size_t n, int resX, int resY) {
        if (!pts && n) return false;
        RECT r = detail::get_monitor_rect_(0);
        std::vector<POINT> absPts;
        detail::to_points_abs_(pts, n, resX, resY, r, absPts);
        return detail::OverlayThread::instance().set_points_abs(absPts.data(), absPts.size());
    }

    // 3) Масштабирование в конкретный монитор по индексу (0 — Primary).
    inline bool draw_on_monitor(const XY* pts, size_t n, int resX, int resY, int monitorIndex) {
        if (!pts && n) return false;
        RECT r = detail::get_monitor_rect_(monitorIndex);
        std::vector<POINT> absPts;
        detail::to_points_abs_(pts, n, resX, resY, r, absPts);
        return detail::OverlayThread::instance().set_points_abs(absPts.data(), absPts.size());
    }

    inline bool hide() { return detail::OverlayThread::instance().hide(); }

#else // !_WIN32

    inline bool draw(const XY*, size_t) { return false; }
    inline bool draw(const XY*, size_t, int, int) { return false; }
    inline bool draw_on_monitor(const XY*, size_t, int, int, int) { return false; }
    inline bool hide() { return false; }

#endif // _WIN32

} // namespace overlay_points
