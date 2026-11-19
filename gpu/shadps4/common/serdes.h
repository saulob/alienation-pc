// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

#include <concepts>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Serialization {

/// A cache entry that does not match its format; the entry is skipped, never fatal.
struct CorruptData : std::runtime_error {
    CorruptData() : std::runtime_error{"invalid or corrupted shader cache entry"} {}
};

template <typename T>
concept Container = requires(T t) {
    typename T::iterator;
    { t.begin() } -> std::same_as<typename T::iterator>;
    { t.end() } -> std::same_as<typename T::iterator>;
    { t.size() } -> std::convertible_to<std::size_t>;
};

struct Archive {
    void Alloc(size_t size) {
        container.resize(size);
    }

    void Grow(size_t size) {
        container.resize(container.size() + size);
    }

    void Merge(const Archive& ar) {
        container.insert(container.end(), ar.container.cbegin(), ar.container.cend());
        offset = container.size();
    }

    [[nodiscard]] size_t SizeBytes() const {
        return container.size();
    }

    [[nodiscard]] size_t Remaining() const {
        return container.size() - offset;
    }

    u8* CurrPtr() {
        return container.data() + offset;
    }

    void Advance(size_t size) {
        if (size > Remaining()) {
            throw CorruptData{};
        }
        offset += size;
    }

    std::vector<u8>&& TakeOff() {
        offset = 0;
        return std::move(container);
    }

    [[nodiscard]] bool IsEoS() const {
        return offset >= container.size();
    }

    Archive() = default;
    explicit Archive(std::vector<u8>&& v) : container{std::move(v)} {}

private:
    size_t offset{};
    std::vector<u8> container{};

    friend struct Writer;
    friend struct Reader;
};

struct Writer {
    template <typename T>
    void Write(const T* ptr, size_t size) {
        if (size > ar.Remaining()) {
            ar.Grow(size);
        }
        if (size > ar.Remaining()) {
            throw std::length_error{"serialization writer overrun"};
        }
        std::memcpy(ar.CurrPtr(), reinterpret_cast<const void*>(ptr), size);
        ar.offset += size;
    }

    template <typename T>
        requires(!Container<T>)
    void Write(const T& value) {
        const auto size = sizeof(value);
        Write(&value, size);
    }

    void Write(const auto& v) {
        Write(v.size());
        for (const auto& elem : v) {
            Write(elem);
        }
    }

    void Write(const std::string& s) {
        Write(s.size());
        Write(s.c_str(), s.size());
    }

    Writer() = delete;
    explicit Writer(Archive& ar_) : ar{ar_} {}

    Archive& ar;
};

/// Reads never pass the end of the buffer and never size an allocation from a length the
/// remaining bytes cannot hold; anything else throws CorruptData.
struct Reader {
    template <typename T>
    void Read(T* ptr, size_t size) {
        if (size > ar.Remaining()) {
            throw CorruptData{};
        }
        std::memcpy(reinterpret_cast<void*>(ptr), ar.CurrPtr(), size);
        ar.offset += size;
    }

    template <typename T>
        requires(!Container<T>)
    void Read(T& value) {
        const auto size = sizeof(value);
        Read(&value, size);
    }

    void Read(auto& v) {
        using Element = std::remove_cvref_t<decltype(*v.begin())>;
        constexpr size_t min_encoded_size = Container<Element> ? sizeof(size_t) : sizeof(Element);
        size_t num_elements{};
        Read(num_elements);
        if (num_elements > ar.Remaining() / min_encoded_size) {
            throw CorruptData{};
        }
        for (size_t i = 0; i < num_elements; ++i) {
            v.emplace_back();
            Read(v.back());
        }
    }

    void Read(std::string& s) {
        size_t length{};
        Read(length);
        if (length > ar.Remaining()) {
            throw CorruptData{};
        }
        s.resize(length);
        Read(s.data(), length);
    }

    /// The next `size` bytes in place, for data that is not copied into a field.
    const u8* Borrow(size_t size) {
        if (size > ar.Remaining()) {
            throw CorruptData{};
        }
        const u8* bytes = ar.CurrPtr();
        ar.offset += size;
        return bytes;
    }

    Reader() = delete;
    explicit Reader(Archive& ar_) : ar{ar_} {}

    Archive& ar;
};

} // namespace Serialization
