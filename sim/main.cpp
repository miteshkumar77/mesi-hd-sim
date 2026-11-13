#include "System.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::uint64_t number(const std::string& s) {
    if (s.empty() || s[0] == '-') throw std::invalid_argument("expected unsigned number");
    std::size_t used = 0;
    auto n = std::stoull(s, &used, 0);
    if (used != s.size()) throw std::invalid_argument("bad numeric token: " + s);
    return n;
}
void usage() {
    std::cout << "Usage: mesi-sim [--parse-only] [--oracle-log PATH] TRACE\n"
              << "Trace: CORE R ADDRESS | CORE W ADDRESS VALUE | CORE E ADDRESS\n"
              << "4 simulated cores; decimal or 0x hex; # starts a comment.\n"
              << "E means evict here. Addresses are physical, 8-byte aligned.\n"
              << "--oracle-log sets the message log path (default: output.txt).\n";
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
        bool parse_only = false;
        std::string oracle_log = "output.txt";
        std::string trace;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--parse-only") parse_only = true;
            else if (arg == "--oracle-log" && i + 1 < argc) oracle_log = argv[++i];
            else if (arg.rfind("--", 0) != 0 && trace.empty()) trace = arg;
            else { usage(); return 2; }
        }
        if (trace.empty()) { usage(); return 2; }
        std::ifstream input(trace);
        if (!input) throw std::runtime_error("cannot open trace");
        mesi::System system(oracle_log);
        std::string line;
        std::size_t row = 0, operations = 0;
        while (std::getline(input, line)) {
            ++row;
            if (auto p = line.find('#'); p != std::string::npos) line.resize(p);
            std::istringstream fields(line);
            std::string c, op, a, v, excess;
            if (!(fields >> c)) continue;
            if (!(fields >> op >> a)) throw std::runtime_error("short trace row " + std::to_string(row));
            if (op == "W" && !(fields >> v)) throw std::runtime_error("missing store value");
            if (op != "R" && op != "W" && op != "E") throw std::runtime_error("unknown operation");
            if (fields >> excess) throw std::runtime_error("extra token on row " + std::to_string(row));
            auto core = static_cast<mesi::Core>(number(c));
            auto address = number(a);
            auto value = v.empty() ? 0 : number(v);
            if (core >= system.cores())
                throw std::runtime_error("bad core/address on row " + std::to_string(row));
            ++operations;
            if (parse_only) continue;
            mesi::Value loaded = 0;
            if (op == "R") loaded = system.load(core, address);
            if (op == "W") system.store(core, address, value);
            if (op == "E") system.evict(core, address);
            system.assert_invariants();
            std::cout << operations << " C" << core << ' ' << op << " 0x" << std::hex << address << std::dec;
            if (op == "R") std::cout << " -> " << loaded;
            if (op == "W") std::cout << " = " << value;
            std::cout << " |";
            for (mesi::Core p = 0; p < system.cores(); ++p)
                std::cout << " C" << p << '=' << mesi::letter(system.inspect(p, address).state);
            std::cout << '\n';
        }
        if (parse_only) { std::cout << "Parsed " << operations << " operations.\n"; return 0; }
        auto s = system.stats();
        std::cout << "stats gets=" << s.gets << " getm=" << s.getm
                  << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
