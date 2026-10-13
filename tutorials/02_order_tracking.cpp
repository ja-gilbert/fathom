#include <algorithm>
#include <cassert>
#include <cstdint>
#include <databento/constants.hpp>   // kUndefPrice
#include <databento/datetime.hpp>    // UnixNanos
#include <databento/enums.hpp>       // Action, Side
#include <databento/historical.hpp>  // HistoricalBuilder
#include <databento/pretty.hpp>      // Px
#include <functional>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <utility>

namespace db = databento;

struct Order {
    Order() = default;
    Order(db::Side side, int64_t price, uint32_t size, db::UnixNanos ts_event)
        : side{side}, price{price}, size{size}, ts_event{ts_event} {}

    db::Side side{db::Side::None};
    int64_t price{db::kUndefPrice};
    uint32_t size{};
    db::UnixNanos ts_event{};
};

struct PriceLevel {
    PriceLevel() = default;
    explicit PriceLevel(const Order& order) : price{order.price}, size{order.size}, count{1} {}

    int64_t price{db::kUndefPrice};
    uint32_t size{};
    uint32_t count{};
};

class Book {
public:
    std::pair<PriceLevel, PriceLevel> Bbo() const {
        PriceLevel best_bid;
        PriceLevel best_ask;
        for (const auto& order_pair : orders_) {
            const auto& order = order_pair.second;
            PriceLevel* side_best = &best_bid;
            std::function<bool(int64_t, int64_t)> is_improvement = std::greater<int64_t>{};
            if (order.side == db::Side::Ask) {
                side_best = &best_ask;
                is_improvement = std::less<int64_t>{};
            }
            if (side_best->price == db::kUndefPrice ||
                is_improvement(order.price, side_best->price)) {
                *side_best = PriceLevel(order);
            } else if (side_best->price == order.price) {
                side_best->size += order.size;
            }
        }
        return {best_bid, best_ask};
    }

    void Apply(db::UnixNanos ts_event, db::Action action, db::Side side, uint64_t order_id,
               int64_t price, uint32_t size, db::FlagSet flags) {
        switch (action) {
            // Trade, Fill, or None: no change
            case db::Action::Trade:
            case db::Action::Fill:
            case db::Action::None:
                break;

            case db::Action::Clear:  // Clear book: remove all resting orders
                orders_.clear();
                break;

            case db::Action::Add: {  // Add: insert a new order
                if (flags.IsTob()) {
                    // For top-of-book publishers, remove previous order associated with
                    // this side
                    auto order_it = std::find_if(orders_.begin(), orders_.end(), [side](auto pair) {
                        return pair.second.side == side;
                    });
                    if (order_it != orders_.end()) {
                        orders_.erase(order_it->first);
                    }
                    // kUndefPrice indicates the price level was removed. There's no new
                    // level to add
                    if (price == db::kUndefPrice) {
                        break;
                    }
                }
                orders_[order_id] = Order{side, price, size, ts_event};
                break;
            }
            case db::Action::Modify: {  // Modify: change the price and/or size of a
                // resting order
                auto& existing_order = orders_.at(order_id);
                // The order loses its priority if the price changes or the size
                // increases
                if (existing_order.price != price || existing_order.size < size) {
                    existing_order.ts_event = ts_event;
                }
                existing_order.size = size;
                existing_order.price = price;
                break;
            }
            case db::Action::Cancel: {  // Cancel: partially or fully cancel some size
                // from a resting order
                auto order_it = orders_.find(order_id);
                if (order_it == orders_.end()) {
                    break;
                }
                assert(order_it->second.size >= size);
                order_it->second.size -= size;
                // If the full size is cancelled, remove the order from the book
                if (order_it->second.size == 0) {
                    orders_.erase(order_it);
                }
                break;
            }
        }
    }

private:
    std::unordered_map<uint64_t, Order> orders_;
};

int main() {
    auto client = db::Historical::Builder().SetKeyFromEnv().Build();
    // First, we request MBO data starting from the beginning of pre-market
    // trading hours
    Book book;
    bool book_is_ready = false;
    client.TimeseriesGetRange("XNAS.ITCH", {"2022-08-26T08:00:00", "2022-08-26T14:30:00"}, {"MSFT"},
                              db::Schema::Mbo, [&book, &book_is_ready](const db::Record& rec) {
                                  const auto& mbo = rec.Get<db::MboMsg>();
                                  book.Apply(mbo.hd.ts_event, mbo.action, mbo.side, mbo.order_id,
                                             mbo.price, mbo.size, mbo.flags);
                                  book_is_ready = mbo.flags.IsLast();
                                  return db::KeepGoing::Continue;
                              });
    if (book_is_ready) {
        auto bbo = book.Bbo();
        std::cout << "Best ask  " << std::setprecision(2) << db::pretty::Px{bbo.second.price}
                  << " x " << bbo.second.size << '\n';
        std::cout << "Best bid  " << std::setprecision(2) << db::pretty::Px{bbo.first.price}
                  << " x " << bbo.first.size << '\n';
    }
    return 0;
}
