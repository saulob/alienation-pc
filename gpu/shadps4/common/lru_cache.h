// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <mutex>
#include <type_traits>

#include "common/types.h"

namespace Common {

// aln: each operation is atomic: images are touched on the GPU threads while guest threads unmap
// (free) them. ForEachItemBelow runs `func` unlocked, so it may free items.
template <typename ObjectType, typename TickType>
class LeastRecentlyUsedCache {
    struct Item {
        ObjectType obj;
        TickType tick;
        Item* next{};
        Item* prev{};
        bool attached{};
    };

public:
    LeastRecentlyUsedCache() : first_item{}, last_item{} {}
    ~LeastRecentlyUsedCache() = default;

    size_t Insert(ObjectType obj, TickType tick) {
        std::scoped_lock lock{mutex};
        const auto new_id = Build();
        auto& item = item_pool[new_id];
        item.obj = obj;
        item.tick = tick;
        Attach(item);
        return new_id;
    }

    void Touch(size_t id, TickType tick) {
        std::scoped_lock lock{mutex};
        auto& item = item_pool[id];
        if (!item.attached || item.tick >= tick) {
            return;
        }
        item.tick = tick;
        if (&item == last_item) {
            return;
        }
        Detach(item);
        Attach(item);
    }

    void Free(size_t id) {
        std::scoped_lock lock{mutex};
        auto& item = item_pool[id];
        if (!item.attached) {
            return;
        }
        Detach(item);
        free_items.push_back(id);
    }

    [[nodiscard]] size_t Size() {
        std::scoped_lock lock{mutex};
        return attached_items;
    }

    template <typename Func>
    void ForEachItemBelow(TickType tick, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result_t<Func, ObjectType>, bool>;
        std::unique_lock lock{mutex};
        Item* iterator = first_item;
        while (iterator && iterator->attached) {
            if (static_cast<s64>(tick) - static_cast<s64>(iterator->tick) < 0) {
                return;
            }
            Item* const next = iterator->next;
            const ObjectType obj = iterator->obj;
            lock.unlock();
            if constexpr (RETURNS_BOOL) {
                if (func(obj)) {
                    return;
                }
            } else {
                func(obj);
            }
            lock.lock();
            iterator = next;
        }
    }

private:
    size_t Build() {
        if (free_items.empty()) {
            const size_t item_id = item_pool.size();
            item_pool.emplace_back();
            return item_id;
        }
        const size_t item_id = free_items.front();
        free_items.pop_front();
        return item_id;
    }

    void Attach(Item& item) {
        item.attached = true;
        item.next = nullptr;
        item.prev = last_item;
        if (last_item) {
            last_item->next = &item;
        } else {
            first_item = &item;
        }
        last_item = &item;
        ++attached_items;
    }

    void Detach(Item& item) {
        if (item.prev) {
            item.prev->next = item.next;
        } else {
            first_item = item.next;
        }
        if (item.next) {
            item.next->prev = item.prev;
        } else {
            last_item = item.prev;
        }
        item.prev = nullptr;
        item.next = nullptr;
        item.attached = false;
        --attached_items;
    }

    std::mutex mutex;
    std::deque<Item> item_pool;
    std::deque<size_t> free_items;
    Item* first_item{};
    Item* last_item{};
    size_t attached_items{};
};

} // namespace Common
