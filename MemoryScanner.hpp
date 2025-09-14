#pragma once
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstddef>
#include <vector>
#include <type_traits>
#include <algorithm>
#include <cstring>

namespace memscan {

// --- утилита: проверка, можно ли читать регион по Protect ---
inline bool is_readable_page(DWORD prot) {
    if (prot == PAGE_NOACCESS) return false;
    if (prot & PAGE_GUARD)     return false;
    const DWORD mask =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (prot & mask) != 0;
}

// --- утилита: поиск всех вхождений needle в буфере hay ---
inline void find_all_in_buffer(const std::uint8_t* hay, std::size_t hlen,
                               const std::uint8_t* needle, std::size_t nlen,
                               std::uint64_t absolute_start,
                               std::vector<std::uint64_t>& out)
{
    if (nlen == 0 || hlen < nlen) return;
    for (std::size_t i = 0; i + nlen <= hlen; ++i) {
        if (std::memcmp(hay + i, needle, nlen) == 0) {
            out.push_back(absolute_start + i);
        }
    }
}

// Низкоуровневая: поиск произвольного байтового паттерна
inline std::vector<std::uint64_t>
scan_pattern(HANDLE process,
             const void* pattern, std::size_t pattern_size,
             std::size_t chunk_size = (1u << 20)) // 1 MiB
{
    std::vector<std::uint64_t> hits;
    if (!process || !pattern || pattern_size == 0) return hits;
    if (chunk_size < pattern_size) chunk_size = pattern_size;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto* minAddr = static_cast<std::uint8_t*>(si.lpMinimumApplicationAddress);
    auto* maxAddr = static_cast<std::uint8_t*>(si.lpMaximumApplicationAddress);
    const SIZE_T page = si.dwPageSize ? si.dwPageSize : 0x1000;

    std::vector<std::uint8_t> buf;
    std::vector<std::uint8_t> carry; // для совпадений на границах чанков/регионов
    carry.reserve(pattern_size ? (pattern_size - 1) : 0);

    MEMORY_BASIC_INFORMATION mbi{};
    std::uint8_t* addr = minAddr;
    std::uint8_t* prev_region_end = nullptr;
    bool prev_region_was_readable = false;

    while (addr < maxAddr) {
        SIZE_T q = VirtualQueryEx(process, addr, &mbi, sizeof(mbi));
        if (!q) {
            // шагнём на страницу, чтобы не зациклиться
            addr += page;
            continue;
        }

        auto* region_base = static_cast<std::uint8_t*>(mbi.BaseAddress);
        auto  region_size = static_cast<SIZE_T>(mbi.RegionSize);
        auto* region_end  = region_base + region_size;

        const bool readable = (mbi.State == MEM_COMMIT) && is_readable_page(mbi.Protect);

        // Сохраняем "непрерывность" переноса только если регионы смежные и оба читаемые
        if (!(prev_region_was_readable && readable && prev_region_end == region_base)) {
            carry.clear();
        }

        if (readable) {
            SIZE_T offset = 0;
            while (offset < region_size) {
                SIZE_T to_read = std::min<SIZE_T>(chunk_size, region_size - offset);

                // Подготовим буфер: [carry | fresh_read]
                buf.resize(carry.size() + to_read);

                SIZE_T actually_read = 0;
                BOOL ok = ReadProcessMemory(process,
                                            region_base + offset,
                                            buf.data() + carry.size(),
                                            to_read, &actually_read);

                if (!ok && actually_read == 0) {
                    // не удалось прочитать — пропустим страницу
                    offset += page;
                    carry.clear();
                    continue;
                }

                buf.resize(carry.size() + actually_read);

                // Добавим перенос в начало
                if (!carry.empty()) {
                    std::memmove(buf.data(), carry.data(), carry.size());
                }

                // Ищем совпадения
                const std::uint64_t absolute_start =
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(region_base) + offset) - carry.size();

                find_all_in_buffer(buf.data(), buf.size(),
                                   static_cast<const std::uint8_t*>(pattern), pattern_size,
                                   absolute_start, hits);

                // Обновляем перенос: последние (pattern_size-1) байт из прочитанного блока
                const std::size_t keep = std::min<std::size_t>(pattern_size - 1, buf.size());
                carry.assign(buf.end() - keep, buf.end());

                offset += actually_read;
            }
        }

        prev_region_end = region_end;
        prev_region_was_readable = readable;
        addr = region_end;
    }

    return hits;
}

// Высокоуровневая: поиск по значению T (точное байтовое равенство)
template <typename T>
inline std::vector<std::uint64_t>
scan_value(HANDLE process, const T& value,
           std::size_t chunk_size = (1u << 20))
{
    static_assert(std::is_trivially_copyable<T>::value,
                  "scan_value<T>: T must be trivially copyable");
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&value);
    return scan_pattern(process, p, sizeof(T), chunk_size);
}

// Удобство: поиск по байтовой последовательности
inline std::vector<std::uint64_t>
scan_bytes(HANDLE process, const std::vector<std::uint8_t>& bytes,
           std::size_t chunk_size = (1u << 20))
{
    if (bytes.empty()) return {};
    return scan_pattern(process, bytes.data(), bytes.size(), chunk_size);
}

// Удобство: поиск ASCII-строки (без завершающего '\0')
inline std::vector<std::uint64_t>
scan_ascii(HANDLE process, const char* s, bool include_null = false,
           std::size_t chunk_size = (1u << 20))
{
    if (!s) return {};
    const std::size_t n = std::strlen(s) + (include_null ? 1u : 0u);
    return n ? scan_pattern(process, s, n, chunk_size) : std::vector<std::uint64_t>{};
}

struct ScanOpts {
    std::size_t chunk_size = (16u << 20); // 16 MiB
    bool only_private = true;
    DWORD allow_protect = PAGE_READWRITE | PAGE_WRITECOPY; // типично для кучи
    std::size_t align = alignof(void*); // 8 на x64
};

inline bool prot_matches(DWORD prot, DWORD allow) {
    if (prot & PAGE_GUARD)   return false;
    if (prot == PAGE_NOACCESS) return false;
    return (prot & allow) != 0;
}

inline bool type_matches(DWORD type, bool only_private) {
    if (!only_private) return true;
    return type == MEM_PRIVATE;
}

// Находит все вхождения pattern, шагая с заданным выравниванием (align),
// и начиная с позиции, согласованной с выравниванием реального адреса.
inline void find_all_in_buffer_aligned(const std::uint8_t* hay, std::size_t hlen,
    const std::uint8_t* needle, std::size_t nlen,
    std::size_t align, std::uint64_t absolute_start,
    std::vector<std::uint64_t>& out)
{
    if (nlen == 0 || hlen < nlen || align == 0) return;

    // Вычислим стартовый индекс так, чтобы (absolute_start + i) % align == 0
    std::size_t rem = static_cast<std::size_t>(absolute_start % align);
    std::size_t i = (rem == 0) ? 0 : (align - rem);

    // Специализация для 8‑байтовых паттернов (частый случай vptr)
    if (nlen == 8 && align % 8 == 0) {
        const std::uint64_t pat = *reinterpret_cast<const std::uint64_t*>(needle);
        for (; i + 8 <= hlen; i += align) {
            std::uint64_t v = *reinterpret_cast<const std::uint64_t*>(hay + i);
            if (v == pat) out.push_back(absolute_start + i);
        }
        return;
    }

    // Общий путь
    for (; i + nlen <= hlen; i += align) {
        if (std::memcmp(hay + i, needle, nlen) == 0) {
            out.push_back(absolute_start + i);
        }
    }
}

inline std::vector<std::uint64_t>
scan_pattern_aligned(HANDLE process,
    const void* pattern, std::size_t pattern_size,
    const ScanOpts& opts = {})
{
    std::vector<std::uint64_t> hits;
    if (!process || !pattern || pattern_size == 0) return hits;
    const std::size_t chunk = std::max<std::size_t>(opts.chunk_size, pattern_size);

    SYSTEM_INFO si{}; GetSystemInfo(&si);
    auto* minAddr = static_cast<std::uint8_t*>(si.lpMinimumApplicationAddress);
    auto* maxAddr = static_cast<std::uint8_t*>(si.lpMaximumApplicationAddress);
    const SIZE_T page = si.dwPageSize ? si.dwPageSize : 0x1000;

    std::vector<std::uint8_t> buf;
    std::vector<std::uint8_t> carry; carry.reserve(pattern_size ? (pattern_size - 1) : 0);

    MEMORY_BASIC_INFORMATION mbi{};
    std::uint8_t* addr = minAddr;
    std::uint8_t* prev_region_end = nullptr;
    bool prev_readable = false;

    while (addr < maxAddr) {
        SIZE_T q = VirtualQueryEx(process, addr, &mbi, sizeof(mbi));
        if (!q) { addr += page; continue; }

        auto* region_base = static_cast<std::uint8_t*>(mbi.BaseAddress);
        auto  region_size = static_cast<SIZE_T>(mbi.RegionSize);
        auto* region_end = region_base + region_size;

        const bool readable =
            (mbi.State == MEM_COMMIT) &&
            prot_matches(mbi.Protect, opts.allow_protect) &&
            type_matches(mbi.Type, opts.only_private);

        if (!(prev_readable && readable && prev_region_end == region_base)) carry.clear();

        if (readable) {
            SIZE_T offset = 0;
            while (offset < region_size) {
                SIZE_T to_read = std::min<SIZE_T>(chunk, region_size - offset);
                buf.resize(carry.size() + to_read);

                SIZE_T br = 0;
                BOOL ok = ReadProcessMemory(process, region_base + offset,
                    buf.data() + carry.size(),
                    to_read, &br);
                if (!ok && br == 0) {
                    offset += page; carry.clear(); continue;
                }
                buf.resize(carry.size() + br);

                if (!carry.empty())
                    std::memmove(buf.data(), carry.data(), carry.size());

                const std::uint64_t absolute_start =
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(region_base) + offset) - carry.size();

                find_all_in_buffer_aligned(buf.data(), buf.size(),
                    static_cast<const std::uint8_t*>(pattern), pattern_size,
                    std::max<std::size_t>(1, opts.align),
                    absolute_start, hits);

                const std::size_t keep = std::min<std::size_t>(pattern_size - 1, buf.size());
                carry.assign(buf.end() - keep, buf.end());
                offset += br;
            }
        }

        prev_region_end = region_end;
        prev_readable = readable;
        addr = region_end;
    }

    return hits;
}

template <typename T>
inline std::vector<std::uint64_t>
scan_value_aligned(HANDLE process, const T& value, const ScanOpts& opts = {})
{
    static_assert(std::is_trivially_copyable<T>::value, "scan_value_aligned<T>: T must be trivially copyable");
    return scan_pattern_aligned(process, &value, sizeof(T), opts);
}

} // namespace memscan
