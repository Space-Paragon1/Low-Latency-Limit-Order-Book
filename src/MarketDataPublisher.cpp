#include "lob/MarketDataPublisher.h"

#include "lob/MessageParser.h"

#include <stdexcept>

namespace lob {

namespace {
constexpr std::size_t kBufferBytes = 1 << 16;
}

void MarketDataPublisher::Sink::open(const std::string& p, std::string_view header) {
    path = p;
    out.open(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open output file: " + p);
    buf.reserve(kBufferBytes + 256);
    buf.append(header);
    buf.push_back('\n');
}

void MarketDataPublisher::Sink::maybeDrain() {
    if (buf.size() >= kBufferBytes) drain();
}

void MarketDataPublisher::Sink::drain() {
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.clear();
    if (!out) throw std::runtime_error("write failed: " + path);
}

MarketDataPublisher::MarketDataPublisher(const std::string& trades_path,
                                         const std::string& tob_path) {
    trades_.open(trades_path, MessageParser::kTradeHeader);
    tob_.open(tob_path, MessageParser::kTOBHeader);
}

MarketDataPublisher::~MarketDataPublisher() {
    try {
        flush();
    } catch (...) {   // destructors must not throw; call flush() to observe errors
    }
}

void MarketDataPublisher::onTrade(const Trade& trade) {
    MessageParser::appendTrade(trades_.buf, trade);
    trades_.buf.push_back('\n');
    ++trade_count_;
    trades_.maybeDrain();
}

void MarketDataPublisher::onTOB(const TOBUpdate& tob) {
    MessageParser::appendTOB(tob_.buf, tob);
    tob_.buf.push_back('\n');
    ++tob_count_;
    tob_.maybeDrain();
}

void MarketDataPublisher::flush() {
    trades_.drain();
    tob_.drain();
    trades_.out.flush();
    tob_.out.flush();
    if (!trades_.out) throw std::runtime_error("flush failed: " + trades_.path);
    if (!tob_.out) throw std::runtime_error("flush failed: " + tob_.path);
}

}  // namespace lob
