#pragma once

#include "CacheController.hpp"
#include "HomeController.hpp"
#include "HomeLocator.hpp"
#include "MesiOracle.hpp"
#include "MessageBus.hpp"
#include "Types.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mesi {

class System {
public:

    explicit System(const std::string& oracleOutputPath, std::size_t cores = 4, std::size_t homes=2);
    ~System();

    Value load(Core core, Address address);
    void store(Core core, Address address, Value value);
    void evict(Core core, Address address);

    void assert_invariants() const;

    std::size_t cores() const { return controllers_.size(); }
    const Stats& stats() const { return stats_; }
    CacheLine inspect(Core core, Address address) const;

    void stepAllSystems();

private:
    void validate(Core core, Address address) const {
        if (core >= cores()) throw std::out_of_range("invalid core");
    }

    Stats stats_{};

    std::unique_ptr<MesiOracle>        oracle_;
    std::unique_ptr<MessageBus>        messageBus_;

    std::vector<CacheController>       controllers_;
    std::vector<HomeController>        homes_;

    HomeLocator                        homeLocator_;
};

} // namespace mesi
