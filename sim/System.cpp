#include "System.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <utility>

namespace mesi {

System::System(const std::string& oracleOutputPath, std::size_t cores, std::size_t homes)
    : homeLocator_(homes)
{

    if (cores == 0) throw std::invalid_argument("need at least one core");
    if (homes == 0) throw std::invalid_argument("need at least one home");

    oracle_ = std::make_unique<MesiOracle>(oracleOutputPath);

    controllers_.reserve(cores);
    homes_.reserve(homes);

    for (Core c = 0; c < cores; ++c) {
        controllers_.emplace_back(c, RequestIdAllocator{c, cores}, homeLocator_, *oracle_);
    }

    for (Home h = 0; h < homes; ++h) {
        homes_.emplace_back(h, *oracle_);
    }

    std::vector<HomeController*> homeSenders;
    homeSenders.reserve(homes_.size());
    for (auto& home : homes_) {
        homeSenders.push_back(&home);
    }

    std::vector<CacheController*> cacheSenders;
    cacheSenders.reserve(controllers_.size());
    for (auto& cache : controllers_) {
        cacheSenders.push_back(&cache);
    }

    messageBus_ = std::make_unique<FifoMessageBus>(
        std::move(homeSenders), std::move(cacheSenders), *oracle_);

    for (auto& cache : controllers_) {
        cache.setClient(*messageBus_);
    }
    for (auto& home : homes_) {
        home.setClient(*messageBus_);
    }

}

System::~System() = default;

CacheLine System::inspect(Core core, Address address) const {
    validate(core, address);
    return controllers_[core].inspect(address);
}

Value System::load(Core core, Address address) {
    validate(core, address);
    Address currLineAddr{0};
    Value value{0};
    Data  currLine{};
    CacheController& controller = controllers_.at(core);

    for (std::size_t i = 0; i < bytes_per_word; ++i) {
        const Address byteAddr = address + i;
        const Address lineAddr = line_base(byteAddr);
        if (i == 0 || lineAddr != currLineAddr) {
            currLineAddr = lineAddr;
            controller.startLoad(currLineAddr);
            bool missRecorded{false};
            while(!controller.addressReadable(currLineAddr)) {
                // cache miss
                if (!missRecorded) {
                    missRecorded = true;
                    ++stats_.gets;
                }
                stepAllSystems();
            }
            currLine = controller.load(lineAddr);
        }

        // assume integers are stored in little endian format in memory
        value |= (Value{currLine[byteAddr - lineAddr]} << (bits_in_byte * i));
    }
    return value;
}

void System::stepAllSystems()
{
    for (CacheController& controller : controllers_) {
        controller.step();
    }

    for (HomeController& home : homes_) {
        home.step();
    }

    messageBus_->step();
}

void System::store(Core core, Address address, Value value) {
    validate(core, address);
    CacheController& controller = controllers_[core];
    Address currLine = 0;
    for (std::size_t i = 0; i < bytes_per_word; ++i) {
        const Address lineBase{line_base(address + i)};
        if ((i == 0) || (lineBase != currLine)) {
            currLine = lineBase;
            controller.startStore(lineBase);
            bool missRecorded{false};
            while(!controller.addressWriteable(lineBase)) {
                // cache miss

                if (!missRecorded) {
                    missRecorded = true;
                    ++stats_.getm;
                }
                
                stepAllSystems();
            }
        }

        const size_t shift {
            i * bits_in_byte
        };
        uint8_t byte {
            static_cast<uint8_t>((value >> shift) & std::numeric_limits<uint8_t>::max())
        };
        controller.store(lineBase, (address + i - lineBase), byte);
    }
}

void System::evict(Core core, Address address) {
    validate(core, address);
    if (line_base(address) != address) {
        std::ostringstream out;
        out << "System::evict: address is not cache-line aligned"
            << " [core=" << core
            << ", address=0x" << std::hex << address
            << ", lineBase=0x" << line_base(address) << std::dec
            << ", lineBytes=" << line_bytes << ']';
        throw std::logic_error(out.str());
    }
    CacheController& controller = controllers_[core];
    controller.startEvict(address);
    while(controller.addressHasEvictTransaction(address)) {
        stepAllSystems();
    }
}

void System::assert_invariants() const {
}

} // namespace mesi
