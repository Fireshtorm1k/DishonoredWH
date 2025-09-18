#include "ObjectScanner.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

struct Region {
    std::uint8_t* base;
    std::size_t   size;
};
static inline std::size_t page_size() {
    static std::size_t s = [] {
        SYSTEM_INFO si{}; GetSystemInfo(&si); return (std::size_t)si.dwPageSize;
        }();
    return s;
}
static inline bool is_readable(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    const DWORD p = protect & 0xFF; // базовые флаги
    switch (p) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

static std::vector<Region> enumerate_readable_regions() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    const auto minA = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto maxA = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    std::vector<Region> out;
    std::uintptr_t addr = minA;

    while (addr < maxA) {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
        if (got == 0) break;

        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto size = static_cast<std::size_t>(mbi.RegionSize);

        if (mbi.State == MEM_COMMIT && is_readable(mbi.Protect) && size != 0) {
            out.push_back(Region{ reinterpret_cast<std::uint8_t*>(mbi.BaseAddress), size });
        }
        // переход к следующему региону
        const std::uintptr_t next = base + size;
        if (next <= addr) break; // защита от зацикливания при некорректных данных
        addr = next;
    }
    return out;
}

static inline std::uintptr_t align_up(std::uintptr_t x, std::size_t a) { return (x + (a - 1)) & ~(std::uintptr_t)(a - 1); }
static inline std::uintptr_t align_down(std::uintptr_t x, std::size_t a) { return x & ~(std::uintptr_t)(a - 1); }

// Проверка наличия AVX2 у CPU и поддержки ОС сохранения YMM‑состояния
static bool cpu_has_avx2() {
    int info[4] = { 0,0,0,0 };
    __cpuid(info, 1);
    const bool avx = (info[2] & (1 << 28)) != 0;
    const bool osx = (info[2] & (1 << 27)) != 0;
    if (!(avx && osx)) return false;

    // XCR0: биты 1 (XMM) и 2 (YMM) должны быть установлены
    unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;

    int info7[4] = { 0,0,0,0 };
    __cpuidex(info7, 7, 0);
    const bool avx2 = (info7[1] & (1 << 5)) != 0;
    return avx2;
}

static inline void scan_block_scalar_aligned(std::uintptr_t p, std::uintptr_t pend,
    std::uint64_t needle,
    std::vector<std::uintptr_t>& out)
{
    for (; p + 8 <= pend; p += 8) {
        if (*reinterpret_cast<const std::uint64_t*>(p) == needle) {
            out.push_back(p);
        }
    }
}

#ifdef __AVX2__
#include <immintrin.h>
static inline void scan_block_avx2_aligned(std::uintptr_t p, std::uintptr_t pend,
    std::uint64_t needle,
    std::vector<std::uintptr_t>& out)
{
    const __m256i pat = _mm256_set1_epi64x((long long)needle);

    // основной цикл по 32 байта
    for (; p + 32 <= pend; p += 32) {
        // PREFETCH безопасен, но чтобы не волноваться — подстрахуемся по границе страницы
        if ((p & (page_size() - 1)) == 0) {
            _mm_prefetch(reinterpret_cast<const char*>(p + 256), _MM_HINT_T0);
        }

        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const __m256i eq = _mm256_cmpeq_epi64(v, pat);
        const int mask = _mm256_movemask_pd(_mm256_castsi256_pd(eq));
        if (mask) {
            if (mask & 0x1) out.push_back(p + 0);
            if (mask & 0x2) out.push_back(p + 8);
            if (mask & 0x4) out.push_back(p + 16);
            if (mask & 0x8) out.push_back(p + 24);
        }
    }
    // хвост
    for (; p + 8 <= pend; p += 8) {
        if (*reinterpret_cast<const std::uint64_t*>(p) == needle) {
            out.push_back(p);
        }
    }
}
#endif

// Медленный, но «непадающий» проход по странице: 8-байтовые чтения под SEH
static inline void scan_block_scalar_safe(std::uintptr_t p, std::uintptr_t pend,
    std::uint64_t needle,
    std::vector<std::uintptr_t>& out)
{
    for (; p + 8 <= pend; p += 8) {
        bool ok = false;
        std::uint64_t v = 0;
        __try {
            v = *reinterpret_cast<const std::uint64_t*>(p);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (ok && v == needle) out.push_back(p);
    }
}

// Страничный скан: на каждую страницу — одна крупная попытка; при исключении fallback к безопасному проходу
static void scan_region_aligned_robust(const Region& r, std::uint64_t needle,
    std::vector<std::uintptr_t>& out, bool use_avx2)
{
    const std::uintptr_t beg = reinterpret_cast<std::uintptr_t>(r.base);
    const std::uintptr_t end = beg + r.size;

    std::uintptr_t cur = align_up(beg, 8);
    const std::uintptr_t stop = align_down(end, 8);
    if (cur >= stop) return;

    const std::size_t ps = page_size();

    while (cur < stop) {
        const std::uintptr_t page_end = std::min(stop, align_up(cur + 1, ps));

        // Быстрая попытка: целиком страница
        bool page_ok = true;
        __try {
#ifdef __AVX2__
            if (use_avx2) scan_block_avx2_aligned(cur, page_end, needle, out);
            else
#endif
                scan_block_scalar_aligned(cur, page_end, needle, out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_ok = false;
        }

        if (!page_ok) {
            // Страница оказалась с сюрпризами: медленный «безопасный» проход
            scan_block_scalar_safe(cur, page_end, needle, out);
        }

        cur = page_end;
    }
}

// Невыровненный (по каждому байту), тоже устойчивый
static void scan_region_unaligned_robust(const Region& r, std::uint64_t needle,
    std::vector<std::uintptr_t>& out)
{
    const std::uintptr_t beg = reinterpret_cast<std::uintptr_t>(r.base);
    const std::uintptr_t end = beg + r.size;

    std::uintptr_t cur = beg;
    const std::size_t ps = page_size();

    while (cur < end) {
        const std::uintptr_t page_end = std::min(end, align_up(cur + 1, ps));

        bool page_ok = true;
        __try {
            for (std::uintptr_t p = cur; p + 8 <= page_end; ++p) {
                if (*reinterpret_cast<const std::uint64_t*>(p) == needle) {
                    out.push_back(p);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_ok = false;
        }
        if (!page_ok) {
            // медленный безопасный проход по байтам
            for (std::uintptr_t p = cur; p + 8 <= page_end; ++p) {
                bool ok = false;
                std::uint64_t v = 0;
                __try {
                    v = *reinterpret_cast<const std::uint64_t*>(p);
                    ok = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                if (ok && v == needle) out.push_back(p);
            }
        }
        cur = page_end;
    }
}


// Скалярный выровненный скан (fallback без AVX2)
static void scan_region_scalar_aligned(const Region& r, std::uint64_t needle,
    std::vector<std::uintptr_t>& out)
{
    const std::uintptr_t beg = reinterpret_cast<std::uintptr_t>(r.base);
    const std::uintptr_t end = beg + r.size;
    std::uintptr_t p = align_up(beg, 8);
    const std::uintptr_t stop = align_down(end, 8);

    for (; p + 8 <= stop; p += 8) {
        if (*reinterpret_cast<const std::uint64_t*>(p) == needle) {
            out.push_back(p);
        }
    }
}

// Невыровненный скан: проверяем каждый байт (медленнее ~в 7–8×)
static void scan_region_scalar_unaligned(const Region& r, std::uint64_t needle,
    std::vector<std::uintptr_t>& out)
{
    const std::uintptr_t beg = reinterpret_cast<std::uintptr_t>(r.base);
    const std::uintptr_t end = beg + r.size;

    // Читаем 8‑байтовыми unaligned‑загрузками, не переходя границу региона.
    for (std::uintptr_t p = beg; p + 8 <= end; ++p) {
        if (*reinterpret_cast<const std::uint64_t*>(p) == needle) {
            out.push_back(p);
        }
    }
}

struct ScanOptions {
    bool unaligned = false;                 
    unsigned threads = std::thread::hardware_concurrency();
};

std::vector<std::uintptr_t>
scan_self_for_pointer(std::uint64_t needle, const ScanOptions& opt = {})
{
    static_assert(sizeof(void*) == 8, "Требуется x64.");

    auto regions = enumerate_readable_regions();
    if (regions.empty()) return {};

    const bool use_avx2 = cpu_has_avx2() && !opt.unaligned;
    unsigned nt = opt.threads ? opt.threads : 1;
    nt = std::min<unsigned>(nt, regions.size());
    if (nt == 0) nt = 1;

    std::atomic<std::size_t> idx{ 0 };
    std::vector<std::vector<std::uintptr_t>> buckets(nt);

    auto worker = [&](unsigned tid) {
        auto& bucket = buckets[tid];
        bucket.reserve(1 << 12); // небольшой запас, чтобы меньше реаллокаций
        for (;;) {
            const std::size_t i = idx.fetch_add(1, std::memory_order_relaxed);
            if (i >= regions.size()) break;

            const Region& rg = regions[i];

            if (opt.unaligned) {
                scan_region_unaligned_robust(rg, needle, bucket);
            }
            else {
#ifdef __AVX2__
                const bool use_avx2 = cpu_has_avx2();
#else
                const bool use_avx2 = false;
#endif
                scan_region_aligned_robust(rg, needle, bucket, use_avx2);
            }

        }
        };

    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (unsigned t = 0; t < nt; ++t) pool.emplace_back(worker, t);
    for (auto& th : pool) th.join();

    // Слить результаты
    std::size_t total = 0;
    for (auto& b : buckets) total += b.size();
    std::vector<std::uintptr_t> out;
    out.reserve(total);
    for (auto& b : buckets) {
        out.insert(out.end(), b.begin(), b.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ------------------------- демонстрация -------------------------

static std::uint64_t parse_u64(const char* s) {
    // Поддержка "0x..." и без префикса
    std::uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        std::sscanf(s, "%llx", &v);
    }
    else {
        // Пытаемся как hex, если есть буквы A-F, иначе как десятичное
        bool is_hex = false;
        for (const char* p = s; *p; ++p)
            if ((*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) { is_hex = true; break; }
        if (is_hex) std::sscanf(s, "%llx", &v);
        else        std::sscanf(s, "%llu", &v);
    }
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("Usage: fast_self_pointer_scan.exe <value> [unaligned]\n"
            "Example: fast_self_pointer_scan.exe 0x7FFB95631AF6\n"
            "         fast_self_pointer_scan.exe 0x7FFB95631AF6 unaligned");
        return 0;
    }

    const std::uint64_t needle = parse_u64(argv[1]);
    ScanOptions opt{};
    if (argc >= 3) opt.unaligned = true;

    auto hits = scan_self_for_pointer(needle, opt);

    std::printf("Found %zu matches for 0x%016llX\n", hits.size(),
        static_cast<unsigned long long>(needle));
    for (std::uintptr_t p : hits) {
        std::printf("  %p\n", reinterpret_cast<void*>(p));
    }
    return 0;
}


ObjectScanner::ObjectScanner()
{
}

ObjectScanner::~ObjectScanner()
{
}

uintptr_t ObjectScanner::getCameraTransform()
{
    HMODULE base = GetModuleHandle(NULL);
    return ((uintptr_t)base + 0x2BC59A0);
}

std::vector<uintptr_t> ObjectScanner::scanForType(ClassType typeForScan)
{
    HMODULE base = GetModuleHandle(NULL);
    uintptr_t classVptr = (uintptr_t)base + typeForScan;
    return scan_self_for_pointer(classVptr);
}
