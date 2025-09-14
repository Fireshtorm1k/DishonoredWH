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

} // namespace memscan
