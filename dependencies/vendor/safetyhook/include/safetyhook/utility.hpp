#pragma once

#ifndef SAFETYHOOK_USE_CXXMODULES
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>
#else
import std.compat;
#endif

namespace safetyhook {
template <typename T> constexpr void store(uint8_t* address, const T& value) {
    std::copy_n(reinterpret_cast<const uint8_t*>(&value), sizeof(T), address);
}

template <typename T, typename U> constexpr T address_cast(U address) {
    if constexpr (std::is_integral_v<T> && std::is_integral_v<U>) {
        return static_cast<T>(address);
    } else {
        return reinterpret_cast<T>(address);
    }
}

bool is_executable(uint8_t* address);

/// @brief Held across every temporary protection change safetyhook makes (save the protection, change it, put it
/// back), so two of them never interleave on one page: the later one would put back the earlier one's temporary
/// protection and leave the page that way. Hold it around such changes of your own to code pages as well.
std::recursive_mutex& protection_change_mutex();

class UnprotectMemory {
public:
    UnprotectMemory() = delete;
    ~UnprotectMemory();
    UnprotectMemory(const UnprotectMemory&) = delete;
    UnprotectMemory(UnprotectMemory&& other) noexcept;
    UnprotectMemory& operator=(const UnprotectMemory&) = delete;
    UnprotectMemory& operator=(UnprotectMemory&& other) noexcept;

private:
    friend std::optional<UnprotectMemory> unprotect(uint8_t*, size_t);

    UnprotectMemory(uint8_t* address, size_t size, uint32_t original_protection,
        std::unique_lock<std::recursive_mutex> lock)
        : m_address{address}, m_size{size}, m_original_protection{original_protection}, m_lock{std::move(lock)} {}

    uint8_t* m_address{};
    size_t m_size{};
    uint32_t m_original_protection{};
    std::unique_lock<std::recursive_mutex> m_lock{};
};

[[nodiscard]] std::optional<UnprotectMemory> unprotect(uint8_t* address, size_t size);

template <typename T> constexpr T align_up(T address, size_t align) {
    const auto unaligned_address = address_cast<uintptr_t>(address);
    const auto aligned_address = (unaligned_address + align - 1) & ~(align - 1);
    return address_cast<T>(aligned_address);
}

template <typename T> constexpr T align_down(T address, size_t align) {
    const auto unaligned_address = address_cast<uintptr_t>(address);
    const auto aligned_address = unaligned_address & ~(align - 1);
    return address_cast<T>(aligned_address);
}
} // namespace safetyhook
