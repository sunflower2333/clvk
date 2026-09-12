// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// Copied metadata only. Never retain or read application buffer/image content.
struct cvk_dispatch_arguments {
    static constexpr size_t capacity = 16;
    struct argument {
        uint32_t pos = 0, kind = 0, set = 0, binding = 0, offset = 0, size = 0;
        std::array<char, 64> name{}, type_name{};
        bool name_truncated = false, type_truncated = false;
        std::array<uint8_t, 32> scalar{};
        uint32_t scalar_bytes = 0;
        bool scalar_valid = false, scalar_truncated = false;
        uintptr_t resource = 0;
        uint64_t resource_bytes = 0, flags = 0, parent_offset = 0, local_bytes = 0;
        uint32_t memory_type = 0;
        std::array<uint64_t, 6> image{}; // width,height,depth,array,row,slice
        void capture_scalar(const uint8_t* data, size_t available,
                            size_t start, size_t requested) {
            scalar_valid = start <= available && requested <= available - start &&
                (data != nullptr || requested == 0);
            scalar_bytes = scalar_valid ? uint32_t(std::min(requested, scalar.size())) : 0;
            scalar_truncated = scalar_valid && requested > scalar.size();
            if (scalar_bytes) std::memcpy(scalar.data(), data + start, scalar_bytes);
        }
        std::array<char, 65> scalar_hex() const {
            const char digits[] = "0123456789abcdef";
            std::array<char, 65> result{};
            for (uint32_t i = 0; i < scalar_bytes; ++i) {
                result[i * 2] = digits[scalar[i] >> 4];
                result[i * 2 + 1] = digits[scalar[i] & 15];
            }
            return result;
        }
    };
    template <size_t N> static bool text(std::array<char, N>& output,
                                         const std::string& value) {
        const auto size = std::min(value.size(), N - 1);
        std::memcpy(output.data(), value.data(), size); output[size] = 0;
        return size < value.size();
    }
    uint64_t total = 0;
    std::array<argument, capacity> args{};
};
