#include <algorithm>
#include <cstdint>
#include <databento/constants.hpp>  // dataset, kUndefPrice
#include <databento/datetime.hpp>   // ToIso8601, UnixNanos
#include <databento/dbn_store.hpp>
#include <databento/enums.hpp>  // Action, Side
#include <databento/flag_set.hpp>
#include <databento/historical.hpp>  // HistoricalBuilder
#include <databento/pretty.hpp>      // Px
#include <databento/record.hpp>      // BidAskPair, MboMsg, Record
#include <databento/symbol_map.hpp>  // TsSymbolMap
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace db = databento;

struct PriceLevel {
    int64_t price{db::kUndefPrice};
    uint32_t size{0};
    uint32_t count{0};

    bool IsEmpty() const { return price == db::kUndefPrice; }
    operator bool() const { return !IsEmpty(); }
};

std::ostream& operator<<(std::ostream& stream, const PriceLevel& level) {
    stream << level.size << " @ " << db::pretty::Px{level.price} << " | " << level.count
           << " order(s)";
    return stream;
}

class Book {
public:
    std::pair<PriceLevel, PriceLevel> Bbo() const { return {GetBidLevel(), GetAskLevel()}; }

    PriceLevel GetBidLevel(std::size_t idx = 0) const {
        if (bids_.size() > idx) {
            // Reverse iterator to get highest bid prices first
            auto level_it = bids_.rbegin();
            std::advance(level_it, idx);
            return GetPriceLevel(level_it->first, level_it->second);
        }
        return PriceLevel{};
    }

    PriceLevel GetAskLevel(std::size_t idx = 0) const {
        if (offers_.size() > idx) {
            auto level_it = offers_.begin();
            std::advance(level_it, idx);
            return GetPriceLevel(level_it->first, level_it->second);
        }
        return PriceLevel{};
    }

    PriceLevel GetBidLevelByPx(int64_t px) const {
        auto level_it = bids_.find(px);
        if (level_it == bids_.end()) {
            throw std::invalid_argument{"No bid level at " + db::pretty::PxToString(px)};
        }
        return GetPriceLevel(px, level_it->second);
    }

    PriceLevel GetAskLevelByPx(int64_t px) const {
        auto level_it = offers_.find(px);
        if (level_it == offers_.end()) {
            throw std::invalid_argument{"No ask level at " + db::pretty::PxToString(px)};
        }
        return GetPriceLevel(px, level_it->second);
    }

    const db::MboMsg& GetOrder(uint64_t order_id) {
        auto order_it = orders_by_id_.find(order_id);
        if (order_it == orders_by_id_.end()) {
            throw std::invalid_argument{"No order with ID " + std::to_string(order_id)};
        }
        auto& level = GetLevel(order_it->second.side, order_it->second.price);
        return *GetLevelOrder(level, order_id);
    }

    uint32_t GetQueuePos(uint64_t order_id) {
        auto order_it = orders_by_id_.find(order_id);
        if (order_it == orders_by_id_.end()) {
            throw std::invalid_argument{"No order with ID " + std::to_string(order_id)};
        }
        const auto& level_it = GetLevel(order_it->second.side, order_it->second.price);
        uint32_t prior_size = 0;
        for (const auto& order : level_it) {
            if (order.order_id == order_id) {
                break;
            }
            prior_size += order.size;
        }
        return prior_size;
    }

    std::vector<db::BidAskPair> GetSnapshot(std::size_t level_count = 1) const {
        std::vector<db::BidAskPair> res;
        for (size_t i = 0; i < level_count; ++i) {
            db::BidAskPair ba_pair{db::kUndefPrice, db::kUndefPrice, 0, 0, 0, 0};
            auto bid = GetBidLevel(i);
            if (bid) {
                ba_pair.bid_px = bid.price;
                ba_pair.bid_sz = bid.size;
                ba_pair.bid_ct = bid.count;
            }
            auto ask = GetAskLevel(i);
            if (ask) {
                ba_pair.ask_px = ask.price;
                ba_pair.ask_sz = ask.size;
                ba_pair.ask_ct = ask.count;
            }
            res.emplace_back(ba_pair);
        }
        return res;
    }

    void Apply(const db::MboMsg& mbo) {
        switch (mbo.action) {
            case db::Action::Clear: {
                Clear();
                break;
            }
            case db::Action::Add: {
                Add(mbo);
                break;
            }
            case db::Action::Cancel: {
                Cancel(mbo);
                break;
            }
            case db::Action::Modify: {
                Modify(mbo);
                break;
            }
            case db::Action::Trade:
            case db::Action::Fill:
            case db::Action::None: {
                break;
            }
            default: {
                throw std::invalid_argument{std::string{"Unknown action: "} +
                                            db::ToString(mbo.action)};
            }
        }
    }

private:
    using LevelOrders = std::vector<db::MboMsg>;
    struct PriceAndSide {
        int64_t price;
        db::Side side;
    };
    using Orders = std::unordered_map<uint64_t, PriceAndSide>;
    using SideLevels = std::map<int64_t, LevelOrders>;

    static PriceLevel GetPriceLevel(int64_t price, const LevelOrders& level) {
        PriceLevel res{price};
        for (const auto& order : level) {
            if (!order.flags.IsTob()) {
                ++res.count;
            }
            res.size += order.size;
        }
        return res;
    }

    static LevelOrders::iterator GetLevelOrder(LevelOrders& level, uint64_t order_id) {
        auto order_it = std::find_if(
            level.begin(), level.end(),
            [order_id](const db::MboMsg& order) { return order.order_id == order_id; });
        if (order_it == level.end()) {
            throw std::invalid_argument{"No order with ID " + std::to_string(order_id)};
        }
        return order_it;
    }

    void Clear() {
        orders_by_id_.clear();
        offers_.clear();
        bids_.clear();
    }

    void Add(db::MboMsg mbo) {
        if (mbo.flags.IsTob()) {
            SideLevels& levels = GetSideLevels(mbo.side);
            levels.clear();
            // kUndefPrice indicates the side's book should be cleared
            // and doesn't represent an order that should be added
            if (mbo.price != db::kUndefPrice) {
                LevelOrders level = {mbo};
                levels.emplace(mbo.price, level);
            }
        } else {
            LevelOrders& level = GetOrInsertLevel(mbo.side, mbo.price);
            level.emplace_back(mbo);
            auto res = orders_by_id_.emplace(mbo.order_id, PriceAndSide{mbo.price, mbo.side});
            if (!res.second) {
                throw std::invalid_argument{"Received duplicated order ID " +
                                            std::to_string(mbo.order_id)};
            }
        }
    }

    void Cancel(db::MboMsg mbo) {
        LevelOrders& level = GetLevel(mbo.side, mbo.price);
        auto order_it = GetLevelOrder(level, mbo.order_id);
        if (order_it->size < mbo.size) {
            throw std::logic_error{"Tried to cancel more size than existed for order ID " +
                                   std::to_string(mbo.order_id)};
        }
        order_it->size -= mbo.size;
        if (order_it->size == 0) {
            orders_by_id_.erase(mbo.order_id);
            level.erase(order_it);
            if (level.empty()) {
                RemoveLevel(mbo.side, mbo.price);
            }
        }
    }

    void Modify(db::MboMsg mbo) {
        auto price_side_it = orders_by_id_.find(mbo.order_id);
        if (price_side_it == orders_by_id_.end()) {
            // If order not found, treat it as an add
            Add(mbo);
            return;
        }
        if (price_side_it->second.side != mbo.side) {
            throw std::logic_error{"Order " + std::to_string(mbo.order_id) + " changed side"};
        }
        auto prev_price = price_side_it->second.price;
        LevelOrders& prev_level = GetLevel(mbo.side, prev_price);
        auto level_order_it = GetLevelOrder(prev_level, mbo.order_id);
        if (prev_price != mbo.price) {
            price_side_it->second.price = mbo.price;
            prev_level.erase(level_order_it);
            if (prev_level.empty()) {
                RemoveLevel(mbo.side, prev_price);
            }
            LevelOrders& level = GetOrInsertLevel(mbo.side, mbo.price);
            // Changing price loses priority
            level.emplace_back(mbo);
        } else if (level_order_it->size < mbo.size) {
            LevelOrders& level = prev_level;
            // Increasing size loses priority
            level.erase(level_order_it);
            level.emplace_back(mbo);
        } else {
            level_order_it->size = mbo.size;
        }
    }

    SideLevels& GetSideLevels(db::Side side) {
        switch (side) {
            case db::Side::Ask: {
                return offers_;
            }
            case db::Side::Bid: {
                return bids_;
            }
            case db::Side::None:
            default: {
                throw std::invalid_argument{"Invalid side"};
            }
        }
    }

    LevelOrders& GetLevel(db::Side side, int64_t price) {
        SideLevels& levels = GetSideLevels(side);
        auto level_it = levels.find(price);
        if (level_it == levels.end()) {
            throw std::invalid_argument{std::string{"Received event for unknown level "} +
                                        db::ToString(side) + " " + db::pretty::PxToString(price)};
        }
        return level_it->second;
    }

    LevelOrders& GetOrInsertLevel(db::Side side, int64_t price) {
        SideLevels& levels = GetSideLevels(side);
        return levels[price];
    }

    void RemoveLevel(db::Side side, int64_t price) {
        SideLevels& levels = GetSideLevels(side);
        levels.erase(price);
    }

    Orders orders_by_id_;
    SideLevels offers_;
    SideLevels bids_;
};

class Market {
public:
    struct PublisherBook {
        uint16_t publisher_id;
        Book book;
    };

    const std::vector<PublisherBook>& GetBooksByPub(uint32_t instrument_id) {
        return books_[instrument_id];
    }

    const Book& GetBook(uint32_t instrument_id, uint16_t publisher_id) {
        const std::vector<PublisherBook>& books = GetBooksByPub(instrument_id);
        auto book_it =
            std::find_if(books.begin(), books.end(), [publisher_id](const PublisherBook& pub_book) {
                return pub_book.publisher_id == publisher_id;
            });
        if (book_it == books.end()) {
            throw std::invalid_argument{"No book for publisher ID " + std::to_string(publisher_id)};
        }
        return book_it->book;
    }

    std::pair<PriceLevel, PriceLevel> Bbo(uint32_t instrument_id, uint16_t publisher_id) {
        const auto& book = GetBook(instrument_id, publisher_id);
        return book.Bbo();
    }

    std::pair<PriceLevel, PriceLevel> AggregatedBbo(uint32_t instrument_id) {
        PriceLevel agg_bid;
        PriceLevel agg_ask;
        for (const auto& pub_book : GetBooksByPub(instrument_id)) {
            const auto bbo = pub_book.book.Bbo();
            const auto& bid = bbo.first;
            const auto& ask = bbo.second;
            if (bid) {
                if (agg_bid.IsEmpty() || bid.price > agg_bid.price) {
                    agg_bid = bid;
                } else if (bid.price == agg_bid.price) {
                    agg_bid.count += bid.count;
                    agg_bid.size += bid.size;
                }
            }
            if (ask) {
                if (agg_ask.IsEmpty() || ask.price < agg_ask.price) {
                    agg_ask = ask;
                } else if (ask.price == agg_ask.price) {
                    agg_ask.count += ask.count;
                    agg_ask.size += ask.size;
                }
            }
        }
        return {agg_bid, agg_ask};
    }

    void Apply(const db::MboMsg& mbo_msg) {
        auto& instrument_books = books_[mbo_msg.hd.instrument_id];
        auto book_it = std::find_if(instrument_books.begin(), instrument_books.end(),
                                    [&mbo_msg](const PublisherBook& pub_book) {
                                        return pub_book.publisher_id == mbo_msg.hd.publisher_id;
                                    });
        if (book_it == instrument_books.end()) {
            instrument_books.emplace_back(PublisherBook{mbo_msg.hd.publisher_id, {}});
            book_it = std::prev(instrument_books.end());
        }
        book_it->book.Apply(mbo_msg);
    }

private:
    std::unordered_map<uint32_t, std::vector<PublisherBook>> books_;
};

int main() {
    // First, create a historical client
    auto client = db::Historical::Builder().SetKeyFromEnv().Build();

    // Next, we'll set up the books and book handlers
    Market market;
    // We'll parse symbology from the DBN metadata
    db::TsSymbolMap symbol_map;
    auto metadata_callback = [&symbol_map](db::Metadata metadata) {
        symbol_map = metadata.CreateSymbolMap();
    };
    // For each book update...
    auto record_callback = [&market, &symbol_map](const db::Record& record) {
        if (auto* mbo = record.GetIf<db::MboMsg>()) {
            // We'll apply
            market.Apply(*mbo);
            // If it's the last update in an event, print the state of the aggregated
            // book
            if (mbo->flags.IsLast()) {
                const auto& symbol = symbol_map.At(*mbo);
                auto bbo = market.AggregatedBbo(mbo->hd.instrument_id);
                std::cout << symbol << " Aggregated BBO | " << db::ToIso8601(mbo->ts_recv)
                          << "\n    " << bbo.second << "\n    " << bbo.first << '\n';
            }
        }
        return db::KeepGoing::Continue;
    };

    auto file_path = "dbeq-basic-20240403.mbo.dbn.zst";
    // Check if file exists
    auto file_store =
        std::ifstream(file_path).good()
            // And open it if it does
            ? db::DbnStore{file_path}  // Or we'll request the data starting from the beginning
                                       // of pre-market trading hours
            : client.TimeseriesGetRangeToFile(db::dataset::kDbeqBasic,
                                              {"2024-04-03T08:00:00", "2024-04-03T14:00:00"},
                                              {"GOOG", "GOOGL"}, db::Schema::Mbo, file_path);
    // Finally, we'll replay each book update
    file_store.Replay(metadata_callback, record_callback);
    return 0;
}
