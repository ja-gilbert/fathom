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
#include <map>
#include <unordered_map>
#include <vector>

#include "itch.hpp"

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

// One stock's levels. Prices are ITCH Price(4): 1072600 is $107.26.
struct Levels {
    std::map<uint32_t, Level> bids;  // best bid is rbegin()
    std::map<uint32_t, Level> asks;  // best ask is begin()
    std::map<uint32_t, Level>& side(char s) { return s == 'B' ? bids : asks; }
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
        const auto it = Lookup(d.hd.locate, d.ref);
        if (it != orders_.end()) Erase(it);
    }

    void Replace(const itch::OrderReplace& u) {
        const auto it = Lookup(u.hd.locate, u.old_ref);
        if (it == orders_.end()) return;
        if (u.new_ref == u.old_ref)
            ++stats_.duplicate_add;  // a replace always issues a new reference
        Order order = it->second;
        Erase(it);
        if (u.shares == 0) return;  // replaced to zero displayed shares: dead (spec 1.4)
        order.shares = u.shares;
        order.price = u.price;
        Insert(u.new_ref, order);
    }

    const Order* Find(uint64_t ref) const {
        const auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
    }

    // A stock's levels; empty if nothing is resting in it.
    const Levels& levels(uint16_t locate) const {
        static const Levels kEmpty;
        return locate < levels_.size() ? levels_[locate] : kEmpty;
    }

    // Rebuilds every level from the orders and compares. False means a level drifted.
    bool LevelsConsistent() const {
        std::vector<Levels> rebuilt(levels_.size());
        for (const auto& [ref, o] : orders_) {
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

    size_t live() const { return orders_.size(); }
    const Stats& stats() const { return stats_; }

private:
    using Map = std::unordered_map<uint64_t, Order>;

    // Finds a live order for a modify message and checks the message's locate against
    // it. Returns end() and counts unknown_ref if the order is not live.
    Map::iterator Lookup(uint16_t locate, uint64_t ref) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++stats_.unknown_ref;
        } else if (it->second.locate != locate) {
            ++stats_.locate_mismatch;
        }
        return it;
    }

    Level& LevelOf(const Order& o) {
        if (o.locate >= levels_.size()) levels_.resize(o.locate + 1);
        return levels_[o.locate].side(o.side)[o.price];
    }

    void Insert(uint64_t ref, const Order& order) {
        if (!orders_.try_emplace(ref, order).second) {
            ++stats_.duplicate_add;
            return;
        }
        stats_.peak_live = std::max(stats_.peak_live, orders_.size());
        Level& level = LevelOf(order);
        level.shares += order.shares;
        ++level.count;
    }

    // Removes an order from the map and from its level, dropping the level if empty.
    void Erase(Map::iterator it) {
        const Order& o = it->second;
        auto& side = levels_[o.locate].side(o.side);
        const auto level = side.find(o.price);
        level->second.shares -= o.shares;
        if (--level->second.count == 0) side.erase(level);
        orders_.erase(it);
    }

    void Reduce(uint16_t locate, uint64_t ref, uint32_t shares, uint64_t& overflow_counter) {
        const auto it = Lookup(locate, ref);
        if (it == orders_.end()) return;
        Order& o = it->second;
        // Taking more than remained is counted and clamped, so the unsigned subtraction
        // cannot wrap around; the order then dies at zero like any other.
        if (shares > o.shares) ++overflow_counter;
        const uint32_t taken = std::min(shares, o.shares);
        o.shares -= taken;
        LevelOf(o).shares -= taken;
        if (o.shares == 0) Erase(it);
    }

    Map orders_;
    std::vector<Levels> levels_;  // indexed by stock locate
    Stats stats_;
};

}  // namespace fathom
