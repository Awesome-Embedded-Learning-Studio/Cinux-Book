/**
 * @file static_hash_map.hpp
 * @brief Fixed-capacity open-addressing hash map with linear probing.
 */

#ifndef CINUX_STATIC_HASH_MAP_HPP
#define CINUX_STATIC_HASH_MAP_HPP

#include <cinux/optional.hpp>
#include <cinux/string_view.hpp>
#include <cstddef>
#include <cstdint>

namespace cinux::lib {

// ============================================================
// Default hash functor specializations
// ============================================================

template <typename T>
struct Hash;

template <>
struct Hash<uint32_t> {
    constexpr size_t operator()(uint32_t v) const { return static_cast<size_t>(v * 2654435761u); }
};

template <>
struct Hash<int> {
    constexpr size_t operator()(int v) const { return Hash<uint32_t>{}(static_cast<uint32_t>(v)); }
};

template <>
struct Hash<uint64_t> {
    constexpr size_t operator()(uint64_t v) const {
        return static_cast<size_t>(v * 14695981039346656037ULL);
    }
};

template <>
struct Hash<StringView> {
    constexpr size_t operator()(StringView sv) const {
        // FNV-1a
        size_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < sv.size(); ++i) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(sv[i]));
            h *= 1099511628211ULL;
        }
        return h;
    }
};

// ============================================================
// StaticHashMap
// ============================================================

/**
 * @brief Fixed-capacity open-addressing hash map with linear probing.
 *
 * @tparam K     Key type.
 * @tparam V     Value type.
 * @tparam N     Number of slots (capacity).
 * @tparam HashT Hash functor.
 *
 * Supports tombstone deletion. No heap allocation.
 */
template <typename K, typename V, size_t N, typename HashT = Hash<K>>
class StaticHashMap {
public:
    constexpr StaticHashMap() = default;

    static constexpr size_t capacity() { return N; }

    constexpr size_t size() const { return size_; }
    constexpr bool   empty() const { return size_ == 0; }
    constexpr bool   full() const { return size_ >= N; }

    // ============================================================
    // Insert
    // ============================================================

    /** @brief Insert a key-value pair. Returns false if key exists or table is full. */
    constexpr bool insert(const K& key, const V& value) {
        size_t idx = find_slot_for_insert(key);
        if (idx == N) {
            return false;  // full
        }

        if (slots_[idx].occupied && !slots_[idx].tombstone) {
            return false;  // key already exists
        }

        slots_[idx].key       = key;
        slots_[idx].value     = value;
        slots_[idx].occupied  = true;
        slots_[idx].tombstone = false;
        ++size_;
        return true;
    }

    /** @brief Insert or overwrite existing key. Returns false only if full and key not present. */
    constexpr bool insert_or_assign(const K& key, const V& value) {
        size_t idx = find_slot_for_insert(key);
        if (idx == N) {
            return false;
        }

        if (slots_[idx].occupied && !slots_[idx].tombstone) {
            slots_[idx].value = value;
            return true;
        }

        slots_[idx].key       = key;
        slots_[idx].value     = value;
        slots_[idx].occupied  = true;
        slots_[idx].tombstone = false;
        ++size_;
        return true;
    }

    // ============================================================
    // Lookup
    // ============================================================

    constexpr Optional<V> find(const K& key) const {
        size_t idx = find_slot_for_lookup(key);
        if (idx == N) {
            return {};
        }
        return slots_[idx].value;
    }

    constexpr bool contains(const K& key) const { return find_slot_for_lookup(key) != N; }

    // ============================================================
    // Remove
    // ============================================================

    constexpr bool remove(const K& key) {
        size_t idx = find_slot_for_lookup(key);
        if (idx == N) {
            return false;
        }
        slots_[idx].tombstone = true;
        --size_;
        return true;
    }

    // ============================================================
    // Direct access
    // ============================================================

    constexpr V& operator[](const K& key) {
        size_t idx = find_slot_for_lookup(key);
        if (idx != N) {
            return slots_[idx].value;
        }

        // Not found — insert default
        idx                   = find_slot_for_insert(key);
        slots_[idx].key       = key;
        slots_[idx].value     = V{};
        slots_[idx].occupied  = true;
        slots_[idx].tombstone = false;
        ++size_;
        return slots_[idx].value;
    }

    constexpr void clear() {
        for (size_t i = 0; i < N; ++i) {
            slots_[i] = Slot{};
        }
        size_ = 0;
    }

private:
    struct Slot {
        K    key{};
        V    value{};
        bool occupied  = false;
        bool tombstone = false;
    };

    Slot   slots_[N]{};
    size_t size_ = 0;

    constexpr size_t hash_index(const K& key) const { return HashT{}(key) % N; }

    // Find slot where key lives (for lookup/remove)
    constexpr size_t find_slot_for_lookup(const K& key) const {
        size_t start = hash_index(key);
        for (size_t i = 0; i < N; ++i) {
            size_t idx = (start + i) % N;
            if (!slots_[idx].occupied) {
                return N;
            }
            if (!slots_[idx].tombstone && slots_[idx].key == key) {
                return idx;
            }
        }
        return N;
    }

    // Find slot for insertion: prefers tombstone, then empty, stops at existing key
    constexpr size_t find_slot_for_insert(const K& key) {
        size_t start           = hash_index(key);
        size_t first_tombstone = N;

        for (size_t i = 0; i < N; ++i) {
            size_t idx = (start + i) % N;
            if (!slots_[idx].occupied) {
                // Return tombstone if we saw one, else this empty slot
                return (first_tombstone != N) ? first_tombstone : idx;
            }
            if (slots_[idx].tombstone) {
                if (first_tombstone == N) {
                    first_tombstone = idx;
                }
            } else if (slots_[idx].key == key) {
                return idx;  // existing key
            }
        }
        // Table is full of live entries
        return (first_tombstone != N) ? first_tombstone : N;
    }
};

}  // namespace cinux::lib

#endif  // CINUX_STATIC_HASH_MAP_HPP
