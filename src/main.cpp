// lob_main — replay an order-event CSV through the matching engine and write
// the resulting trade prints and top-of-book updates.

#include "lob/MarketDataPublisher.h"
#include "lob/MatchingEngine.h"
#include "lob/MessageParser.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaxReported = 20;

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options] <input.csv> <trades_out.csv> <tob_out.csv>\n"
        << "\n"
        << "  input.csv       order event stream (format: see README)\n"
        << "  trades_out.csv  trade prints\n"
        << "  tob_out.csv     top-of-book updates (one row per change)\n"
        << "\n"
        << "Options:\n"
        << "  --strict        exit with status 2 if the input has malformed lines\n"
        << "  --quiet         only print the summary\n"
        << "  -h, --help      show this help\n";
}

void ensureParentDir(const std::string& file) {
    const std::filesystem::path parent = std::filesystem::path(file).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
}

}  // namespace

int main(int argc, char* argv[]) {
    bool strict = false;
    bool quiet  = false;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--strict")      strict = true;
        else if (arg == "--quiet")  quiet = true;
        else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n\n";
            usage(argv[0]);
            return 1;
        } else {
            paths.emplace_back(arg);
        }
    }
    if (paths.size() != 3) {
        usage(argv[0]);
        return 1;
    }
    const std::string& input_path  = paths[0];
    const std::string& trades_path = paths[1];
    const std::string& tob_path    = paths[2];

    // ── Parse ─────────────────────────────────────────────────────────────────
    lob::MessageParser::ParsedFile input;
    try {
        input = lob::MessageParser::parseCSVFile(input_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        return 1;
    }
    for (std::size_t i = 0; i < input.errors.size() && i < kMaxReported; ++i) {
        const auto& err = input.errors[i];
        std::cerr << "[WARN ] " << input_path << ':' << err.line << ": " << err.message
                  << "  | " << err.text << '\n';
    }
    if (input.errors.size() > kMaxReported)
        std::cerr << "[WARN ] ... " << input.errors.size() - kMaxReported
                  << " more malformed lines\n";
    if (strict && !input.errors.empty()) {
        std::cerr << "[ERROR] " << input.errors.size() << " malformed line(s); aborting (--strict)\n";
        return 2;
    }

    // ── Engine + publisher ────────────────────────────────────────────────────
    std::unique_ptr<lob::MarketDataPublisher> publisher;
    try {
        ensureParentDir(trades_path);
        ensureParentDir(tob_path);
        publisher = std::make_unique<lob::MarketDataPublisher>(trades_path, tob_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        return 1;
    }

    lob::MatchingEngine engine;
    engine.setTradeHandler([&](const lob::Trade& t) { publisher->onTrade(t); });
    engine.setTOBHandler([&](const lob::TOBUpdate& u) { publisher->onTOB(u); });

    // ── Replay ────────────────────────────────────────────────────────────────
    std::array<std::uint64_t, lob::kRejectReasonCount> rejects{};
    std::size_t reported = 0;

    const auto t0 = std::chrono::steady_clock::now();
    try {
        for (std::size_t i = 0; i < input.events.size(); ++i) {
            const lob::MatchResult r = engine.processEvent(input.events[i]);
            if (r.accepted()) continue;
            ++rejects[static_cast<std::size_t>(r.reject_reason)];
            if (!quiet && reported++ < kMaxReported) {
                const lob::OrderEvent& ev = input.events[i];
                std::cerr << "[WARN ] " << input_path << ':' << input.line_numbers[i] << ": "
                          << ev.type << ' ' << ev.order_id << " rejected: " << r.reject_reason
                          << '\n';
            }
        }
        publisher->flush();
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        return 1;
    }
    const auto   t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ── Summary ───────────────────────────────────────────────────────────────
    std::uint64_t total_rejects = 0;
    for (const auto n : rejects) total_rejects += n;
    const double rate = ms > 0 ? static_cast<double>(input.events.size()) / (ms / 1000.0) : 0.0;

    std::cout << "[INFO ] Events         : " << input.events.size() << " (from " << input_path
              << ")\n"
              << "[INFO ] Malformed lines: " << input.errors.size() << '\n'
              << "[INFO ] Rejected       : " << total_rejects << '\n';
    for (std::size_t i = 1; i < rejects.size(); ++i)
        if (rejects[i] != 0)
            std::cout << "[INFO ]   " << static_cast<lob::RejectReason>(i) << ": " << rejects[i]
                      << '\n';
    std::cout << "[INFO ] Trades         : " << publisher->tradeCount() << "  -> " << trades_path
              << '\n'
              << "[INFO ] TOB updates    : " << publisher->tobCount() << "  -> " << tob_path << '\n'
              << "[INFO ] Replay time    : " << ms << " ms (" << static_cast<std::uint64_t>(rate)
              << " events/s, including output)\n";
    return 0;
}
