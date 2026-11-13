#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace mesi {

using Address = std::uint64_t;
using Value = std::uint64_t;
using Core = std::size_t;
using Home = std::size_t;
using OracleToken = std::size_t;
using RequestId = std::size_t;
constexpr Address     line_bytes = 64;
constexpr std::size_t words_per_line = 8;
constexpr std::size_t bytes_per_word = 8;
constexpr std::size_t bits_in_byte = 8;
using Data = std::array<uint8_t, words_per_line * bytes_per_word>;

enum class State { I, S, E, M };

enum class ControllerType { HOME, CACHE };
enum class AccessType { READ, WRITE };

inline char letter(State s) {
    switch (s) {
    case State::I: return 'I';
    case State::S: return 'S';
    case State::E: return 'E';
    case State::M: return 'M';
    }
    throw std::logic_error("invalid state");
}

constexpr Address line_base(Address address) {
    return address & ~(line_bytes - 1);
}

constexpr std::size_t word_index(Address address) {
    return static_cast<std::size_t>((address % line_bytes) / sizeof(Value));
}

struct CacheLine {
    State state = State::I;
    Data data{};
};

struct Stats {
    std::uint64_t gets = 0;              // load miss requests
    std::uint64_t getm = 0;              // store requests from I or S
};

} // namespace mesi
