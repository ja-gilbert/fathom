// The book: every displayed order resting on Nasdaq, and the price levels they form.
//
// Five of the seven order messages (E, C, X, D, U) identify an order only by reference
// number and never repeat its side or symbol, so the engine has to remember what the
// add said. The order map keeps that (locate, side, shares, price) and applies the
// lifecycle rules:
//   E, C, X  subtract shares; the order dies when it reaches zero
//   D        removes the order outright
//   U        removes the old reference and inserts the new one with the old side/locate;
//            a replace to zero shares just kills the order
//
// On top of the order map, each stock keeps two sorted maps from price to level (total
// shares, order count), one per side. Every change to an order also adjusts its level,
// and a level disappears with its last order. The best bid is the last bid level and the
// best ask the first ask level, so the top of the book is O(1).
//
// Instead of asserting, every violation of the lifecycle rules is counted in Stats. A
// full-day replay proves the logic is right by ending with every counter at zero AND no
// orders left live after the End of Messages event; the counters alone cannot see a bug
// that fails to take shares off an order. LevelsConsistent() checks the levels against
// the orders the same way.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "itch.hpp"
#include "order_table.hpp"

namespace fathom {

struct Order {
    uint16_t locate;
    char side;  // 'B' or 'S'
    uint32_t shares;
    uint32_t price;
};

struct Level {
    uint64_t shares = 0;
    uint32_t count = 0;
    bool operator==(const Level&) const = default;
};

// One side's price levels, kept in a vector sorted worst to best: bids ascending, asks
// descending. The top of book is the last entry, and because most
// adds, cancels and executions land near the top, an insert or erase moves only the few
// entries behind it, instead of touching a tree node per level as std::map did.
class LevelMap {
public:
    using Entry = std::pair<uint32_t, Level>;
    using iterator = std::vector<Entry>::iterator;

    explicit LevelMap(bool best_is_highest) : best_is_highest_(best_is_highest) {}

    bool empty() const { return v_.empty(); }
    size_t size() const { return v_.size(); }
    // Worst to best; rbegin() walks from the top of book down.
    auto begin() const { return v_.begin(); }
    auto end() const { return v_.end(); }
    auto rbegin() const { return v_.rbegin(); }
    auto rend() const { return v_.rend(); }
    bool contains(uint32_t price) const {
        const auto it =
            std::lower_bound(v_.begin(), v_.end(), price,
                             [this](const Entry& e, uint32_t p) { return Worse(e.first, p); });
        return it != v_.end() && it->first == price;
    }

    iterator find(uint32_t price) {
        const auto it = Lower(price);
        return it != v_.end() && it->first == price ? it : v_.end();
    }
    // The level at `price`, created empty if absent.
    Level& operator[](uint32_t price) {
        auto it = Lower(price);
        if (it == v_.end() || it->first != price) it = v_.insert(it, {price, Level{}});
        return it->second;
    }
    void erase(iterator it) { v_.erase(it); }
    bool operator==(const LevelMap&) const = default;

private:
    bool Worse(uint32_t a, uint32_t b) const { return best_is_highest_ ? a < b : a > b; }
    iterator Lower(uint32_t price) {
        return std::lower_bound(v_.begin(), v_.end(), price,
                                [this](const Entry& e, uint32_t p) { return Worse(e.first, p); });
    }
    bool best_is_highest_;
    std::vector<Entry> v_;
};

// One stock's levels. Prices are ITCH Price(4): 1072600 is $107.26. The best bid and
// the best ask are both rbegin() of their side.
struct Levels {
    LevelMap bids{true};
    LevelMap asks{false};
    LevelMap& side(char s) { return s == 'B' ? bids : asks; }
};

class Book {
public:
    struct Stats {
        // E/C/X/D/U naming a reference that is not live: never added, or already removed.
        uint64_t unknown_ref = 0;
        // A/F/U inserting a reference that is still live, or a U whose new reference
        // equals its old one. Reuse of a reference that already died is not detected.
        uint64_t duplicate_add = 0;
        // E/C/X/D/U whose Stock Locate differs from the one the order was added under.
        // The update is still applied; the counter records the inconsistency.
        uint64_t locate_mismatch = 0;
        uint64_t over_execute = 0;    // E/C for more shares than remained
        uint64_t over_cancel = 0;     // X for more shares than remained
        uint64_t zero_share_add = 0;  // A/F announcing an order with no shares
        size_t peak_live = 0;         // most displayed orders resting at any one moment

        bool clean() const {
            return unknown_ref == 0 && duplicate_add == 0 && locate_mismatch == 0 &&
                   over_execute == 0 && over_cancel == 0 && zero_share_add == 0;
        }
    };

    void Add(const itch::AddOrder& a) {
        if (a.shares == 0) {
            ++stats_.zero_share_add;
            return;
        }
        Insert(a.ref, Order{a.hd.locate, a.side, a.shares, a.price});
    }

    // 'E' and 'C' take raw fields because they decode to different structs.
    void Execute(uint16_t locate, uint64_t ref, uint32_t shares) {
        Reduce(locate, ref, shares, stats_.over_execute);
    }

    void Cancel(const itch::OrderCancel& x) {
        Reduce(x.hd.locate, x.ref, x.shares, stats_.over_cancel);
    }

    void Delete(const itch::OrderDelete& d) {
        const size_t slot = Lookup(d.hd.locate, d.ref);
        if (slot != OrderTable::npos) Erase(slot);
    }

    void Replace(const itch::OrderReplace& u) {
        const size_t slot = Lookup(u.hd.locate, u.old_ref);
        if (slot == OrderTable::npos) return;
        if (u.new_ref == u.old_ref)
            ++stats_.duplicate_add;  // a replace always issues a new reference
        Order order = At(slot);
        Erase(slot);
        if (u.shares == 0) return;  // replaced to zero displayed shares: dead (spec 1.4)
        order.shares = u.shares;
        order.price = u.price;
        Insert(u.new_ref, order);
    }

    const Order* Find(uint64_t ref) const {
        const size_t slot = index_.Find(ref);
        return slot == OrderTable::npos ? nullptr : &pool_[index_.value(slot)];
    }

    // A stock's levels; empty if nothing is resting in it.
    const Levels& levels(uint16_t locate) const {
        static const Levels kEmpty;
        return locate < levels_.size() ? levels_[locate] : kEmpty;
    }

    // Rebuilds every level from the orders and compares. False means a level drifted.
    bool LevelsConsistent() const {
        std::vector<Levels> rebuilt(levels_.size());
        for (size_t slot = 0; slot < index_.capacity(); ++slot) {
            if (index_.value(slot) == OrderTable::kEmpty) continue;
            const Order& o = pool_[index_.value(slot)];
            Level& level = rebuilt[o.locate].side(o.side)[o.price];
            level.shares += o.shares;
            ++level.count;
        }
        for (size_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].bids != rebuilt[i].bids || levels_[i].asks != rebuilt[i].asks)
                return false;
        }
        return true;
    }

    size_t live() const { return index_.size(); }
    const Stats& stats() const { return stats_; }

    Book() { pool_.reserve(kExpectedLiveOrders); }

private:
    // Finds a live order for a modify message and checks the message's locate against
    // it. Returns its index slot, or npos (counting unknown_ref) if it is not live.
    size_t Lookup(uint16_t locate, uint64_t ref) {
        const size_t slot = index_.Find(ref);
        if (slot == OrderTable::npos) {
            ++stats_.unknown_ref;
        } else if (At(slot).locate != locate) {
            ++stats_.locate_mismatch;
        }
        return slot;
    }

    Order& At(size_t slot) { return pool_[index_.value(slot)]; }

    Level& LevelOf(const Order& o) {
        if (o.locate >= levels_.size()) levels_.resize(o.locate + 1);
        return levels_[o.locate].side(o.side)[o.price];
    }

    void Insert(uint64_t ref, const Order& order) {
        // Reuse a pool slot freed by a dead order, or append; give it back on a duplicate.
        uint32_t idx;
        if (free_.empty()) {
            idx = static_cast<uint32_t>(pool_.size());
            pool_.push_back(order);
        } else {
            idx = free_.back();
            free_.pop_back();
            pool_[idx] = order;
        }
        if (!index_.Insert(ref, idx)) {
            ++stats_.duplicate_add;
            free_.push_back(idx);
            return;
        }
        stats_.peak_live = std::max(stats_.peak_live, index_.size());
        Level& level = LevelOf(order);
        level.shares += order.shares;
        ++level.count;
    }

    // Removes an order from the index, the pool and its level, dropping the level if
    // empty.
    void Erase(size_t slot) {
        const uint32_t idx = index_.value(slot);
        const Order& o = pool_[idx];
        auto& side = levels_[o.locate].side(o.side);
        const auto level = side.find(o.price);
        level->second.shares -= o.shares;
        if (--level->second.count == 0) side.erase(level);
        index_.Erase(slot);
        free_.push_back(idx);
    }

    void Reduce(uint16_t locate, uint64_t ref, uint32_t shares, uint64_t& overflow_counter) {
        const size_t slot = Lookup(locate, ref);
        if (slot == OrderTable::npos) return;
        Order& o = At(slot);
        // Taking more than remained is counted and clamped, so the unsigned subtraction
        // cannot wrap around; the order then dies at zero like any other.
        if (shares > o.shares) ++overflow_counter;
        const uint32_t taken = std::min(shares, o.shares);
        o.shares -= taken;
        LevelOf(o).shares -= taken;
        if (o.shares == 0) Erase(slot);
    }

    // Sized for the busiest day seen (1.93M live orders) so neither
    // the index nor the pool grows mid-day. Orders live in a flat pool and the index
    // maps reference number to pool slot; no heap allocation per order.
    static constexpr size_t kExpectedLiveOrders = 1 << 22;

    OrderTable index_{kExpectedLiveOrders};
    std::vector<Order> pool_;
    std::vector<uint32_t> free_;  // pool slots whose orders died
    std::vector<Levels> levels_;  // indexed by stock locate
    Stats stats_;
};

}  // namespace fathom
