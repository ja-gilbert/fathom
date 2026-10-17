// The order map: every displayed order currently resting on Nasdaq, keyed by its
// reference number. (Non-displayed orders are never announced by ITCH, so they are
// invisible here; their executions show up only as 'P' trades.)
//
// Five of the seven order messages (E, C, X, D, U) identify an order only by reference
// number and never repeat its side or symbol, so the engine has to remember what the
// add said. The map keeps what the book will need (locate, side, shares, price) and
// deliberately drops the MPID attribution, which nothing in the engine uses yet. The
// lifecycle rules live here:
//   E, C, X  subtract shares; the order dies when it reaches zero
//   D        removes the order outright
//   U        removes the old reference and inserts the new one with the old side/locate;
//            a replace to zero shares just kills the order
//
// Instead of asserting, every violation of the lifecycle rules is counted in Stats. A
// full-day replay proves the logic is right by ending with every counter at zero AND
// no orders left live after the End of Messages event; the counters alone cannot see a
// bug that fails to take shares off an order.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "itch.hpp"

namespace fathom {

struct Order {
    uint16_t locate;
    char side;  // 'B' or 'S'
    uint32_t shares;
    uint32_t price;
};

class OrderMap {
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
        if (it != orders_.end()) orders_.erase(it);
    }

    void Replace(const itch::OrderReplace& u) {
        const auto it = Lookup(u.hd.locate, u.old_ref);
        if (it == orders_.end()) return;
        if (u.new_ref == u.old_ref)
            ++stats_.duplicate_add;  // a replace always issues a new reference
        Order order = it->second;
        orders_.erase(it);
        if (u.shares == 0) return;  // replaced to zero displayed shares: dead (spec 1.4)
        order.shares = u.shares;
        order.price = u.price;
        Insert(u.new_ref, order);
    }

    const Order* Find(uint64_t ref) const {
        const auto it = orders_.find(ref);
        return it == orders_.end() ? nullptr : &it->second;
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

    void Insert(uint64_t ref, const Order& order) {
        if (!orders_.try_emplace(ref, order).second) {
            ++stats_.duplicate_add;
            return;
        }
        stats_.peak_live = std::max(stats_.peak_live, orders_.size());
    }

    void Reduce(uint16_t locate, uint64_t ref, uint32_t shares, uint64_t& overflow_counter) {
        const auto it = Lookup(locate, ref);
        if (it == orders_.end()) return;
        // Taking more than remained is counted and clamped, so the unsigned subtraction
        // cannot wrap around; the order then dies at zero like any other.
        if (shares > it->second.shares) ++overflow_counter;
        it->second.shares -= std::min(shares, it->second.shares);
        if (it->second.shares == 0) orders_.erase(it);
    }

    Map orders_;
    Stats stats_;
};

}  // namespace fathom
