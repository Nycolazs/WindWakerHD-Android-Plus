// A hash table for the renderer's short-lived caches (per submission or per state): 64-bit keys,
// open addressing in one array, cleared in O(1) by bumping an epoch. Unlike std::unordered_map it
// doesn't allocate per entry or free on clear (malloc/free cost the render thread ~3% with the maps
// these caches were cleared and refilled every submission) and a lookup reads one cache line.
// Keys should be well mixed in their low bits (callers hash them with mix()).
#pragma once
#include <cstdint>
#include <vector>

namespace gfx {

template <typename V>
class FlatTable {
public:
    static uint64_t mix(uint64_t k) {
        k ^= k >> 33;
        k *= 0xFF51AFD7ED558CCDull;
        return k ^ (k >> 33);
    }
    V* find(uint64_t key) {
        if (slots_.empty()) return nullptr;
        for (size_t i = mix(key) & mask_;; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.epoch != epoch_) return nullptr;
            if (s.key == key) return &s.value;
        }
    }
    // the entry for key, added (value-initialized) when missing
    V& operator[](uint64_t key) {
        if ((used_ + 1) * 2 > slots_.size()) grow();
        for (size_t i = mix(key) & mask_;; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.epoch != epoch_) {
                s = Slot{key, epoch_, V{}};
                used_++;
                return s.value;
            }
            if (s.key == key) return s.value;
        }
    }
    void clear() {
        used_ = 0;
        if (++epoch_ == 0) {  // wrapped: old slots could look current
            for (auto& s : slots_) s.epoch = 0;
            epoch_ = 1;
        }
    }
    size_t size() const { return used_; }

private:
    struct Slot {
        uint64_t key = 0;
        uint32_t epoch = 0;  // current when == epoch_
        V value{};
    };
    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        const uint32_t oldEpoch = epoch_;
        slots_.resize(old.empty() ? 64 : old.size() * 2);
        mask_ = slots_.size() - 1;
        epoch_ = 1;
        used_ = 0;
        for (auto& s : old)
            if (s.epoch == oldEpoch) (*this)[s.key] = s.value;
    }
    std::vector<Slot> slots_;
    size_t mask_ = 0, used_ = 0;
    uint32_t epoch_ = 1;
};

}  // namespace gfx

namespace gfx {

// A small cache in front of a big map: 2 slots per set, 64-bit keys, one cache line per lookup.
// A miss costs nothing but the map lookup it saves elsewhere; clear() when the map drops entries.
template <typename V, unsigned kSets>
class HotCache {
    static_assert((kSets & (kSets - 1)) == 0, "a power of two");

public:
    V* find(uint64_t key) {
        Slot* s = set(key);
        if (s[0].used && s[0].key == key) return &s[0].value;
        if (s[1].used && s[1].key == key) return &s[1].value;
        return nullptr;
    }
    void put(uint64_t key, const V& v) {  // the newest in slot 0, the one before it moves to slot 1
        Slot* s = set(key);
        if (!(s[0].used && s[0].key == key)) s[1] = s[0];
        s[0] = {key, v, true};
    }
    void clear() {
        for (auto& s : slots_) s.used = false;
    }

private:
    struct Slot {
        uint64_t key = 0;
        V value{};
        bool used = false;
    };
    Slot* set(uint64_t key) { return &slots_[(FlatTable<V>::mix(key) & (kSets - 1)) * 2]; }
    Slot slots_[kSets * 2];
};

}  // namespace gfx
