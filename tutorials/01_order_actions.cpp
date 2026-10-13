#include <databento/datetime.hpp>
#include <databento/historical.hpp>
#include <databento/pretty.hpp>
#include <iomanip>
#include <iostream>

namespace db = databento;

int main() {
    auto client = db::Historical::Builder().SetKeyFromEnv().Build();

    // Inspect history of a particular order
    constexpr auto kOrderId = 6410543150678;
    auto handle_record = [](const db::Record& record) {
        const auto& mbo = record.Get<db::MboMsg>();
        if (mbo.order_id == kOrderId) {
            constexpr auto kSep = "  ";
            std::cout << db::ToIso8601(mbo.ts_recv) << kSep << std::setprecision(2)
                      << db::pretty::Px{mbo.price} << kSep << std::setw(2) << std::setfill(' ')
                      << mbo.size << kSep;
            switch (mbo.action) {
                case db::Action::Add:
                    std::cout << "[A]dded";
                    break;
                case db::Action::Cancel:
                    std::cout << "[C]anceled";
                    break;
                case db::Action::Modify:
                    std::cout << "[M]odified";
                    break;
                case db::Action::Clear:
                    std::cout << "clea[R]ed";
                    break;
                case db::Action::Trade:
                    std::cout << "[T]raded";
                    break;
                case db::Action::Fill:
                    std::cout << "[F]illed";
                    break;
                case db::Action::None:
                    std::cout << "[N]one";
                    break;
            }
            std::cout << '\n';
        }
        return db::KeepGoing::Continue;
    };

    // Request a minute of MBO data
    client.TimeseriesGetRange("GLBX.MDP3", db::DateTimeRange<std::string>{"2022-08-26T10:59"},
                              {"ES.v.0"}, db::Schema::Mbo, db::SType::Continuous,
                              db::SType::InstrumentId, {}, {}, handle_record);
    return 0;
}
