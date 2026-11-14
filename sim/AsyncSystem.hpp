#pragma once

#include "CacheController.hpp"
#include "HomeController.hpp"
#include "HomeLocator.hpp"
#include "InteractiveMessageBus.hpp"
#include "MesiOracle.hpp"
#include "RealisticMessageBus.hpp"
#include "Types.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <optional>
#include <functional>

namespace mesi {


using CommandTokenT = uint64_t;

struct LoadCommandTraits
{
    struct ArgsT
    {
        Address wordByteAddress;
    };

    struct StateT
    {
        size_t  currByte{0};
        Address currLineAddr{0};
        Value   currValue{0};
    };

    struct ResultT
    {
        Value value;
    };
};


struct StoreCommandTraits
{
    struct ArgsT
    {
        Address wordByteAddress;
        Value   value;
    };

    struct StateT
    {
        size_t  currByte{0};
        Address currLineAddr{0};
    };

    struct ResultT{};
};


struct EvictCommandTraits
{
    struct ArgsT
    {
        Address cacheLineAddress;
    };

    struct StateT{};

    struct ResultT{};
};

template <typename TraitT>
struct CommandProgress
{
    typename TraitT::ArgsT  args;
    typename TraitT::StateT state{};
};

namespace detail {

template <typename ...TraitTs>
struct CommandTraitsSet
{
    using args_union_t     = std::variant<typename TraitTs::ArgsT...>;
    using progress_union_t = std::variant<CommandProgress<TraitTs>...>;
    using results_union_t  = std::variant<typename TraitTs::ResultT...>;
};

}

using CommandTraitsT = detail::CommandTraitsSet<LoadCommandTraits, StoreCommandTraits, EvictCommandTraits>;

struct Command
{
    CommandTokenT token;
    CommandTraitsT::args_union_t args;
};

struct RunningCommand
{
    CommandTokenT token;
    CommandTraitsT::progress_union_t progress;
};

class AsyncSystem
{
public:
    explicit AsyncSystem(
        const std::string& oracleOutputPath,
        std::size_t cores = 4,
        std::size_t homes = 1,
        std::unique_ptr<IOrderingStrategy> orderingStrategy
            = std::make_unique<FifoDelayedOrderingStrategy>());

    CommandTokenT submitLoadCommand(Core core, const LoadCommandTraits::ArgsT& args);
    CommandTokenT submitStoreCommand(Core core, const StoreCommandTraits::ArgsT& args);
    CommandTokenT submitEvictCommand(Core core, const EvictCommandTraits::ArgsT& args);

    using CompletionHandlerT = std::function<void(CommandTokenT, const CommandTraitsT::results_union_t&)>;
    void setCompletionHandler(CompletionHandlerT& completionHandler);
    void step();

    CacheLine inspect(Core core, Address address) const;
    std::size_t cores() const { return controllers_.size(); }

    InteractiveMessageBus& interactiveMessageBus();

private:
    void tryDequeCommand(Core core);
    void progressRunningCmd(Core core);

    Stats stats_{};

    std::unique_ptr<MesiOracle> oracle_;
    std::unique_ptr<RealisticMessageBus> messageBus_;
    std::unique_ptr<InteractiveMessageBus> interactiveMessageBus_;

    std::vector<CacheController>               controllers_;
    std::vector<std::deque<Command>>           pendingCommands_;
    std::vector<std::optional<RunningCommand>> runningCommands_;

    std::vector<HomeController>  homes_;

    HomeLocator                  homeLocator_;

    CommandTokenT nextCommandToken_{0};

    CompletionHandlerT completionHandler_{nullptr};
};

}