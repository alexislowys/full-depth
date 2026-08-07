#pragma once

#include "book/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Binary export layer: replays a day through the book engine while
// writing flat native-endian (little-endian) record files for the
// downstream analysis side. Record layouts are a frozen cross-language
// contract; the static_asserts below are the C++ half of it.

namespace fdexport {

#pragma pack(push, 1)

// One trade print, 24 bytes.
// kind: 0 = 'E' at resting display price, 1 = 'C' printable-only at
// execution price, 2 = 'P' hidden-order trade, 3 = 'Q' cross (shares
// clamped u64 -> u32). side: resting order side for kinds 0/1, message
// side for kind 2, 0 for kind 3.
struct TradeRecord {
    std::uint64_t ts_ns;
    std::uint32_t price;
    std::uint32_t shares;
    std::uint16_t locate;
    std::uint8_t side;
    std::uint8_t kind;
    std::uint32_t pad;
};

// Top-of-book snapshot, 32 bytes, emitted only when (bid_px, ask_px,
// bid_sz, ask_sz) changes. Price 0 = side empty; sizes clamped
// u64 -> u32, order counts clamped to u16.
struct L1Record {
    std::uint64_t ts_ns;
    std::uint32_t bid_px;
    std::uint32_t ask_px;
    std::uint32_t bid_sz;
    std::uint32_t ask_sz;
    std::uint16_t locate;
    std::uint16_t bid_ct;
    std::uint16_t ask_ct;
    std::uint16_t pad;
};

#pragma pack(pop)

static_assert(sizeof(TradeRecord) == 24);
static_assert(offsetof(TradeRecord, ts_ns) == 0);
static_assert(offsetof(TradeRecord, price) == 8);
static_assert(offsetof(TradeRecord, shares) == 12);
static_assert(offsetof(TradeRecord, locate) == 16);
static_assert(offsetof(TradeRecord, side) == 18);
static_assert(offsetof(TradeRecord, kind) == 19);
static_assert(offsetof(TradeRecord, pad) == 20);

static_assert(sizeof(L1Record) == 32);
static_assert(offsetof(L1Record, ts_ns) == 0);
static_assert(offsetof(L1Record, bid_px) == 8);
static_assert(offsetof(L1Record, ask_px) == 12);
static_assert(offsetof(L1Record, bid_sz) == 16);
static_assert(offsetof(L1Record, ask_sz) == 20);
static_assert(offsetof(L1Record, locate) == 24);
static_assert(offsetof(L1Record, bid_ct) == 26);
static_assert(offsetof(L1Record, ask_ct) == 28);
static_assert(offsetof(L1Record, pad) == 30);

// fwrite through a 1 MiB stdio buffer; one write() call = one record.
class BufferedFile {
public:
    explicit BufferedFile(const std::string& path) : buf_(1u << 20) {
        f_ = std::fopen(path.c_str(), "wb");
        ok_ = (f_ != nullptr);
        if (f_) std::setvbuf(f_, buf_.data(), _IOFBF, buf_.size());
    }
    BufferedFile(const BufferedFile&) = delete;
    BufferedFile& operator=(const BufferedFile&) = delete;
    ~BufferedFile() { close(); }

    void write(const void* p, std::size_t n) {
        if (!f_ || std::fwrite(p, n, 1, f_) != 1) {
            ok_ = false;
            return;
        }
        ++count_;
    }

    // fclose flushes the buffered tail, so its error must land in ok_.
    void close() {
        if (!f_) return;
        if (std::fclose(f_) != 0) ok_ = false;
        f_ = nullptr;
    }

    bool ok() const { return ok_; }
    std::uint64_t count() const { return count_; }

private:
    std::vector<char> buf_; // outlives f_: fclose flushes through it
    std::FILE* f_ = nullptr;
    std::uint64_t count_ = 0;
    bool ok_ = false;
};

// decode_stream handler that owns an Engine and mirrors a bare replay
// while writing trades.bin / l1.bin under out_dir. Executions peek the
// resting order before forwarding (the engine consumes it); every
// book-mutating message is followed by an L1 change check.
class Exporter : public itch::NullHandler {
public:
    static constexpr std::uint32_t kLocates = 65536;

    explicit Exporter(const std::string& out_dir,
                      const std::string& filter_symbol = {})
        : engine_(filter_symbol),
          filtered_(!filter_symbol.empty()),
          out_dir_(out_dir),
          trades_(out_dir + "/trades.bin"),
          l1_(out_dir + "/l1.bin"),
          last_l1_(kLocates) {}

    void prefetch_hint(const std::uint8_t* p, std::uint16_t len) {
        engine_.prefetch_hint(p, len);
    }

    void on_stock_directory(const itch::StockDirectory& m) {
        engine_.on_stock_directory(m);
    }

    void on_stock_trading_action(const itch::StockTradingAction& m) {
        engine_.on_stock_trading_action(m);
    }

    void on_add_order(const itch::AddOrder& m) {
        engine_.on_add_order(m);
        check_l1(m.h.locate, m.h.ts_ns);
    }

    void on_order_executed(const itch::OrderExecuted& m) {
        // Copy side/price/locate before forwarding — the engine mutates
        // the order store, invalidating the peeked pointer. The engine
        // applies book changes at the RESTING order's locate, so the L1
        // check and trade record must use it too.
        const book::Engine::Order* ord = engine_.peek(m.ref);
        const bool live = (ord != nullptr);
        const std::uint8_t side =
            live ? static_cast<std::uint8_t>(ord->side) : 0;
        const std::uint32_t price = live ? ord->price : 0;
        const std::uint16_t loc = live ? ord->locate : m.h.locate;
        engine_.on_order_executed(m);
        if (live) emit_trade(m.h.ts_ns, price, m.shares, loc, side, 0);
        check_l1(loc, m.h.ts_ns);
    }

    void on_order_executed_price(const itch::OrderExecutedPrice& m) {
        const book::Engine::Order* ord = engine_.peek(m.ref);
        const bool live = (ord != nullptr);
        const std::uint8_t side =
            live ? static_cast<std::uint8_t>(ord->side) : 0;
        const std::uint16_t loc = live ? ord->locate : m.h.locate;
        engine_.on_order_executed_price(m);
        // Non-printable executions never print to the tape, but they do
        // reduce the displayed book, so the L1 check still runs.
        if (live && m.printable == 'Y')
            emit_trade(m.h.ts_ns, m.price, m.shares, loc, side, 1);
        check_l1(loc, m.h.ts_ns);
    }

    void on_order_cancel(const itch::OrderCancel& m) {
        engine_.on_order_cancel(m);
        check_l1(m.h.locate, m.h.ts_ns);
    }

    void on_order_delete(const itch::OrderDelete& m) {
        engine_.on_order_delete(m);
        check_l1(m.h.locate, m.h.ts_ns);
    }

    void on_order_replace(const itch::OrderReplace& m) {
        engine_.on_order_replace(m);
        check_l1(m.h.locate, m.h.ts_ns);
    }

    // 'P' never touches the displayed book; forwarded anyway so a replay
    // through the exporter is behavior-identical to a bare engine replay.
    void on_trade(const itch::Trade& m) {
        engine_.on_trade(m);
        if (!tracked(m.h.locate)) return;
        emit_trade(m.h.ts_ns, m.price, m.shares, m.h.locate,
                   static_cast<std::uint8_t>(m.side), 2);
    }

    // Forward first: the engine clears auction-pending on 'Q'.
    void on_cross_trade(const itch::CrossTrade& m) {
        engine_.on_cross_trade(m);
        if (!tracked(m.h.locate)) return;
        emit_trade(m.h.ts_ns, m.price, clamp_u32(m.shares), m.h.locate, 0,
                   3);
    }

    // Call once after the replay: symbols.csv from the directory spin,
    // meta.txt with replay totals.
    bool write_symbols_and_meta(const std::string& source_path,
                                std::uint64_t messages) {
        bool ok = true;
        std::FILE* f = std::fopen((out_dir_ + "/symbols.csv").c_str(), "w");
        if (f) {
            std::fprintf(f, "locate,symbol\n");
            for (std::uint32_t loc = 0; loc < kLocates; ++loc) {
                const std::string& sym =
                    engine_.symbol_of(static_cast<std::uint16_t>(loc));
                if (sym == "?") continue;
                std::fprintf(f, "%u,%s\n", loc, sym.c_str());
            }
            if (std::fclose(f) != 0) ok = false;
        } else {
            ok = false;
        }
        f = std::fopen((out_dir_ + "/meta.txt").c_str(), "w");
        if (f) {
            std::fprintf(f, "source=%s\n", source_path.c_str());
            std::fprintf(f, "messages=%llu\n",
                         static_cast<unsigned long long>(messages));
            std::fprintf(f, "trades=%llu\n",
                         static_cast<unsigned long long>(trades_.count()));
            std::fprintf(f, "l1=%llu\n",
                         static_cast<unsigned long long>(l1_.count()));
            if (std::fclose(f) != 0) ok = false;
        } else {
            ok = false;
        }
        if (!ok) aux_ok_ = false;
        return ok;
    }

    // Flush and close the record files so all_ok() covers the final
    // buffer flush and on-disk sizes are final. Destructor also closes.
    void close() {
        trades_.close();
        l1_.close();
    }

    std::uint64_t trades_written() const { return trades_.count(); }
    std::uint64_t l1_written() const { return l1_.count(); }
    const book::Engine& engine() const { return engine_; }
    bool all_ok() const { return trades_.ok() && l1_.ok() && aux_ok_; }

private:
    struct L1Tuple {
        std::uint32_t bid_px = 0;
        std::uint32_t ask_px = 0;
        std::uint32_t bid_sz = 0;
        std::uint32_t ask_sz = 0;

        bool operator==(const L1Tuple&) const = default;
    };

    static std::uint32_t clamp_u32(std::uint64_t v) {
        return v > 0xFFFF'FFFFULL ? 0xFFFF'FFFFu
                                  : static_cast<std::uint32_t>(v);
    }
    static std::uint16_t clamp_u16(std::uint64_t v) {
        return v > 0xFFFFULL ? std::uint16_t{0xFFFF}
                             : static_cast<std::uint16_t>(v);
    }

    bool tracked(std::uint16_t locate) const {
        return !filtered_ || locate == engine_.target_locate();
    }

    void emit_trade(std::uint64_t ts_ns, std::uint32_t price,
                    std::uint32_t shares, std::uint16_t locate,
                    std::uint8_t side, std::uint8_t kind) {
        const TradeRecord rec{ts_ns, price, shares, locate, side, kind, 0};
        trades_.write(&rec, sizeof rec);
    }

    void check_l1(std::uint16_t locate, std::uint64_t ts_ns) {
        const book::Book* b = engine_.book_for(locate);
        const auto bid = b->best_bid();
        const auto ask = b->best_ask();
        L1Tuple now;
        std::uint16_t bid_ct = 0;
        std::uint16_t ask_ct = 0;
        if (bid) {
            now.bid_px = bid->price;
            now.bid_sz = clamp_u32(bid->level.shares);
            bid_ct = clamp_u16(bid->level.orders);
        }
        if (ask) {
            now.ask_px = ask->price;
            now.ask_sz = clamp_u32(ask->level.shares);
            ask_ct = clamp_u16(ask->level.orders);
        }
        if (now == last_l1_[locate]) return;
        last_l1_[locate] = now;
        const L1Record rec{ts_ns,      now.bid_px, now.ask_px,
                           now.bid_sz, now.ask_sz, locate,
                           bid_ct,     ask_ct,     0};
        l1_.write(&rec, sizeof rec);
    }

    book::Engine engine_;
    bool filtered_;
    std::string out_dir_;
    BufferedFile trades_;
    BufferedFile l1_;
    std::vector<L1Tuple> last_l1_; // last emitted top-of-book per locate
    bool aux_ok_ = true;           // symbols.csv / meta.txt write status
};

} // namespace fdexport
