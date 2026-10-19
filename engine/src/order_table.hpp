// An open-addressing hash table from order reference number to a 32-bit value (the
// order's slot in a pool). std::unordered_map allocated one heap node
// per order and chased a pointer per lookup; this keeps keys and values in two flat
// arrays, probes linearly, and erases by backward shift so there are no tombstones.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fathom {

class OrderTable {
public:
    static constexpr size_t npos = SIZE_MAX;
    static constexpr uint32_t kEmpty = UINT32_MAX;

    // `capacity` is rounded up to a power of two; the table grows when 70% full.
    explicit OrderTable(size_t capacity) { Rebuild(std::bit_ceil(capacity)); }

    size_t size() const { return size_; }
    size_t capacity() const { return keys_.size(); }

    // Slot holding `key`, or npos.
    size_t Find(uint64_t key) const {
        for (size_t i = Home(key);; i = (i + 1) & mask_) {
            if (vals_[i] == kEmpty) return npos;
            if (keys_[i] == key) return i;
        }
    }

    // Inserts and returns true, or returns false if `key` is already present.
    bool Insert(uint64_t key, uint32_t value) {
        if ((size_ + 1) * 10 > capacity() * 7) Grow();
        for (size_t i = Home(key);; i = (i + 1) & mask_) {
            if (vals_[i] == kEmpty) {
                keys_[i] = key;
                vals_[i] = value;
                ++size_;
                return true;
            }
            if (keys_[i] == key) return false;
        }
    }

    // Removes the entry in `slot`, sliding later entries of its probe run back so a
    // Find for any of them still reaches them without a tombstone.
    void Erase(size_t slot) {
        size_t hole = slot;
        for (size_t j = (hole + 1) & mask_; vals_[j] != kEmpty; j = (j + 1) & mask_) {
            const size_t home = Home(keys_[j]);
            // The entry at j may fill the hole only if its home is not in (hole, j].
            const bool home_after_hole =
                hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
            if (!home_after_hole) {
                keys_[hole] = keys_[j];
                vals_[hole] = vals_[j];
                hole = j;
            }
        }
        vals_[hole] = kEmpty;
        --size_;
    }

    // Direct slot access, for iterating live entries and reading the found value.
    uint32_t value(size_t slot) const { return vals_[slot]; }
    uint64_t key(size_t slot) const { return keys_[slot]; }

private:
    // Fibonacci hashing: the multiply spreads sequential reference numbers across the
    // table, and the shift keeps the top bits, which the multiply mixes best.
    size_t Home(uint64_t key) const { return (key * 0x9E3779B97F4A7C15ull) >> shift_; }

    void Rebuild(size_t capacity) {
        keys_.assign(capacity, 0);
        vals_.assign(capacity, kEmpty);
        mask_ = capacity - 1;
        shift_ = 64 - std::countr_zero(capacity);
    }

    void Grow() {
        std::vector<uint64_t> keys = std::move(keys_);
        std::vector<uint32_t> vals = std::move(vals_);
        Rebuild(keys.size() * 2);
        size_ = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (vals[i] != kEmpty) Insert(keys[i], vals[i]);
        }
    }

    std::vector<uint64_t> keys_;
    std::vector<uint32_t> vals_;
    size_t mask_ = 0;
    unsigned shift_ = 64;
    size_t size_ = 0;
};

}  // namespace fathom
