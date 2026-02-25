#include "MatchingEngine.h"
#include "MessageParser.h"
#include "MarketDataPublisher.h"
#include <iostream>
#include <chrono>
#include <string>

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog
        << " <input.csv> <trades_out.csv> <tob_out.csv>\n"
        << '\n'
        << "  input.csv     – order event stream (see README for format)\n"
        << "  trades_out    – execution reports\n"
        << "  tob_out       – top-of-book snapshots after each change\n";
}

int main(int argc, char* argv[]) {
    if (argc != 4) { usage(argv[0]); return 1; }

    const std::string input_path  = argv[1];
    const std::string trades_path = argv[2];
    const std::string tob_path    = argv[3];

    // ── Parse input ───────────────────────────────────────────────────────────
    std::vector<OrderEvent> events;
    try {
        events = MessageParser::parseCSVFile(input_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << '\n';
        return 1;
    }
    std::cout << "[INFO ] Parsed " << events.size() << " events from "
              << input_path << '\n';

    // ── Engine + publisher ────────────────────────────────────────────────────
    MatchingEngine     engine;
    MarketDataPublisher publisher(trades_path, tob_path);

    engine.setTradeHandler([&](const Trade& t)     { publisher.onTrade(t); });
    engine.setTOBHandler  ([&](const TOBUpdate& u) { publisher.onTOB(u);   });

    // ── Replay ────────────────────────────────────────────────────────────────
    auto t0 = std::chrono::high_resolution_clock::now();

    for (const auto& ev : events)
        engine.processEvent(ev);

    auto t1  = std::chrono::high_resolution_clock::now();
    double elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    publisher.flush();

    // ── Summary ───────────────────────────────────────────────────────────────
    double msgs_per_sec = events.size() / (elapsed_ms / 1000.0);

    std::cout << "[INFO ] Replay complete in " << elapsed_ms << " ms\n"
              << "[INFO ] Throughput      : "
              << static_cast<uint64_t>(msgs_per_sec) << " msgs/sec\n"
              << "[INFO ] Trades emitted  : " << publisher.tradeCount() << '\n'
              << "[INFO ] TOB updates     : " << publisher.tobCount()   << '\n'
              << "[INFO ] trades  → "  << trades_path << '\n'
              << "[INFO ] tob     → "  << tob_path    << '\n';

    return 0;
}
