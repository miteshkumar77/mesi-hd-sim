#include "sim/AsyncSystem.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace mesi {

namespace {

template<class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};

template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

} // namespace

AsyncSystem::AsyncSystem(
    const std::string& oracleOutputPath,
    std::size_t cores,
    std::size_t homes,
    std::unique_ptr<IOrderingStrategy> orderingStrategy)
    : homeLocator_(homes)
{
    if (cores == 0) throw std::invalid_argument("need at least one core");
    if (homes == 0) throw std::invalid_argument("need at least one home");

    oracle_ = std::make_unique<MesiOracle>(oracleOutputPath);

    controllers_.reserve(cores);
    homes_.reserve(homes);

    for (Core c = 0; c < cores; ++c) {
        controllers_.emplace_back(c, RequestIdAllocator{c, cores}, homeLocator_, *oracle_);
        pendingCommands_.emplace_back();
        runningCommands_.emplace_back();
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

    if (orderingStrategy == nullptr) {
        interactiveMessageBus_ = std::make_unique<InteractiveMessageBus>(
            std::move(homeSenders), std::move(cacheSenders), *oracle_);

        for (auto& cache : controllers_) {
            cache.setClient(*interactiveMessageBus_);
        }
        for (auto& home : homes_) {
            home.setClient(*interactiveMessageBus_);
        }
    } else {
        messageBus_ = std::make_unique<RealisticMessageBus>(
            std::move(homeSenders), std::move(cacheSenders), *oracle_, std::move(orderingStrategy));

        for (auto& cache : controllers_) {
            cache.setClient(*messageBus_);
        }
        for (auto& home : homes_) {
            home.setClient(*messageBus_);
        }
    }
}

InteractiveMessageBus& AsyncSystem::interactiveMessageBus()
{
    if (interactiveMessageBus_ == nullptr) {
        throw std::logic_error("AsyncSystem is not using an InteractiveMessageBus");
    }
    return *interactiveMessageBus_;
}

void AsyncSystem::setCompletionHandler(CompletionHandlerT& completionHandler)
{
    completionHandler_ = completionHandler;
}

CacheLine AsyncSystem::inspect(Core core, Address address) const
{
    return controllers_.at(core).inspect(address);
}

CommandTokenT AsyncSystem::submitLoadCommand(Core core, const LoadCommandTraits::ArgsT& args)
{
    const CommandTokenT commandToken{ nextCommandToken_++ };
    pendingCommands_[core].emplace_back(
        Command{
            .token=commandToken,
            .args=args
        });
    return commandToken;
}

CommandTokenT AsyncSystem::submitStoreCommand(Core core, const StoreCommandTraits::ArgsT& args)
{
    const CommandTokenT commandToken{ nextCommandToken_++ };
    pendingCommands_[core].emplace_back(
        Command{
            .token=commandToken,
            .args=args
        });
    return commandToken;
}

CommandTokenT AsyncSystem::submitEvictCommand(Core core, const EvictCommandTraits::ArgsT& args)
{
    const CommandTokenT commandToken{ nextCommandToken_++ };
    pendingCommands_[core].emplace_back(
        Command{
            .token=commandToken,
            .args=args
        });
    return commandToken;
}

void AsyncSystem::tryDequeCommand(Core core)
{
    if (runningCommands_[core].has_value()) return;
    if (pendingCommands_[core].empty()) return;

    using ProgressUnionT = CommandTraitsT::progress_union_t;

    const Command& nextCommand = pendingCommands_[core].front();
    CacheController& controller = controllers_[core];
    ProgressUnionT progress = std::visit(
        Overloaded{
            [this, &controller](const LoadCommandTraits::ArgsT& args) -> ProgressUnionT {
                CommandProgress<LoadCommandTraits> progress{.args=args};
                progress.state.currLineAddr = line_base(args.wordByteAddress);
                controller.startLoad(progress.state.currLineAddr);
                if (!controller.addressReadable(progress.state.currLineAddr)) {
                    // cache miss
                    ++stats_.gets;
                }
                return progress;
            },
            [this, &controller](const StoreCommandTraits::ArgsT& args) -> ProgressUnionT {
                CommandProgress<StoreCommandTraits> progress{.args=args};
                progress.state.currLineAddr = line_base(args.wordByteAddress);
                controller.startStore(progress.state.currLineAddr);
                if (!controller.addressWriteable(progress.state.currLineAddr)) {
                    // cache miss
                    ++stats_.getm;
                }
                return progress;
            },
            [&controller](const EvictCommandTraits::ArgsT& args) -> ProgressUnionT {
                controller.startEvict(args.cacheLineAddress);
                return CommandProgress<EvictCommandTraits>{.args=args};
            },
        },
        nextCommand.args);

    runningCommands_[core].emplace(RunningCommand{
        .token=nextCommand.token,
        .progress=std::move(progress),
    });

    pendingCommands_[core].pop_front();
}

void AsyncSystem::progressRunningCmd(Core core)
{
    if (!runningCommands_[core].has_value()) return;
    RunningCommand& runningCommand = runningCommands_[core].value();
    CacheController& controller = controllers_[core];

    using OptionalResultT = std::optional<CommandTraitsT::results_union_t>;

    const OptionalResultT result = std::visit(
        Overloaded{
            [this, &controller](CommandProgress<LoadCommandTraits>& progress) -> OptionalResultT {
                const LoadCommandTraits::ArgsT& args = progress.args;
                LoadCommandTraits::StateT& state = progress.state;
                while (state.currByte < bytes_per_word) {
                    const Address byteAddr = args.wordByteAddress + state.currByte;
                    const Address lineAddr = line_base(byteAddr);
                    if (lineAddr != state.currLineAddr) {
                        // word spans into the next cache line
                        state.currLineAddr = lineAddr;
                        controller.startLoad(lineAddr);
                        if (!controller.addressReadable(lineAddr)) {
                            // cache miss
                            ++stats_.gets;
                        }
                    }
                    if (!controller.addressReadable(lineAddr)) return std::nullopt;

                    const Data currLine = controller.load(lineAddr);
                    // assume integers are stored in little endian format in memory
                    state.currValue |= (Value{currLine[byteAddr - lineAddr]} << (bits_in_byte * state.currByte));
                    ++state.currByte;
                }
                return LoadCommandTraits::ResultT{.value=state.currValue};
            },
            [this, &controller](CommandProgress<StoreCommandTraits>& progress) -> OptionalResultT {
                const StoreCommandTraits::ArgsT& args = progress.args;
                StoreCommandTraits::StateT& state = progress.state;
                while (state.currByte < bytes_per_word) {
                    const Address byteAddr = args.wordByteAddress + state.currByte;
                    const Address lineBase = line_base(byteAddr);
                    if (lineBase != state.currLineAddr) {
                        // word spans into the next cache line
                        state.currLineAddr = lineBase;
                        controller.startStore(lineBase);
                        if (!controller.addressWriteable(lineBase)) {
                            // cache miss
                            ++stats_.getm;
                        }
                    }
                    if (!controller.addressWriteable(lineBase)) return std::nullopt;

                    const size_t shift {
                        state.currByte * bits_in_byte
                    };
                    uint8_t byte {
                        static_cast<uint8_t>((args.value >> shift) & std::numeric_limits<uint8_t>::max())
                    };
                    controller.store(lineBase, (byteAddr - lineBase), byte);
                    ++state.currByte;
                }
                return StoreCommandTraits::ResultT{};
            },
            [&controller](const CommandProgress<EvictCommandTraits>& progress) -> OptionalResultT {
                const EvictCommandTraits::ArgsT& args = progress.args;
                if (controller.addressHasEvictTransaction(args.cacheLineAddress)) return std::nullopt;
                return EvictCommandTraits::ResultT{};
            },
        },
        runningCommand.progress);

    if (!result.has_value()) return;

    const CommandTokenT token = runningCommand.token;
    runningCommands_[core].reset();
    if (completionHandler_) {
        completionHandler_(token, *result);
    }
}

void AsyncSystem::step()
{
    for (Core core = 0; core < controllers_.size(); ++core) {
        tryDequeCommand(core);
        progressRunningCmd(core);
    }

    for (Core core = 0; core < controllers_.size(); ++core) {
        controllers_[core].step();
    }

    for (Home home = 0; home < homes_.size(); ++home) {
        homes_[home].step();
    }

    if (messageBus_) {
        messageBus_->step();
    }
    if (interactiveMessageBus_) {
        interactiveMessageBus_->step();
    }
}

} // namespace mesi
