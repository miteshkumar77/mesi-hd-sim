#include "RealisticMessageBus.hpp"

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

std::size_t FifoDelayedOrderingStrategy::getDispatchTick(
    const Message& /*message*/, std::size_t currentTick)
{
    return currentTick + 10;
}

std::size_t DelayedGrantOrderingStrategy::getDispatchTick(
    const Message& message, std::size_t currentTick)
{
    if (std::holds_alternative<CacheBoundMessage>(message) &&
        std::holds_alternative<messages::Grant>(std::get<CacheBoundMessage>(message))) {
        return currentTick + 100;
    }
    return currentTick + 1;
}

RealisticMessageBus::RealisticMessageBus(
    std::vector<HomeController*> homesById,
    std::vector<CacheController*> cachesById,
    MesiOracle& oracle,
    std::unique_ptr<IOrderingStrategy> orderingStrategy)
    : homesById_{std::move(homesById)}
    , cachesById_{std::move(cachesById)}
    , oracle_{oracle}
    , orderingStrategy_{std::move(orderingStrategy)}
{}

void RealisticMessageBus::step()
{
    while(!events_.empty() && events_.top().tick <= currentTick_) {
        const Event& event = events_.top();
        std::visit(
            Overloaded{
                [this](const HomeBoundMessage& homeBound) {
                    std::visit([this](const auto& message) {
                        homesById_.at(message.header.destinationId)->deliver(message);
                    }, homeBound);
                },
                [this](const CacheBoundMessage& cacheBound) {
                    std::visit([this](const auto& message) {
                        cachesById_.at(message.header.destinationId)->deliver(message);
                    }, cacheBound);
                },
            },
            event.message);
        events_.pop();
    }
    ++currentTick_;
}

void RealisticMessageBus::enqueue(Message message)
{
    const std::size_t tick = orderingStrategy_->getDispatchTick(message, currentTick_);
    events_.push(Event{tick, std::move(message)});
}

void RealisticMessageBus::send(const messages::GetS& message)
{
    oracle_.log(message);
    enqueue(HomeBoundMessage{message});
}

void RealisticMessageBus::send(const messages::GetM& message)
{
    oracle_.log(message);
    enqueue(HomeBoundMessage{message});
}

void RealisticMessageBus::send(const messages::ProbeAck& message)
{
    oracle_.log(message);
    enqueue(HomeBoundMessage{message});
}

void RealisticMessageBus::send(const messages::EvictReq& message)
{
    oracle_.log(message);
    enqueue(HomeBoundMessage{message});
}

void RealisticMessageBus::send(const messages::Grant& message)
{
    oracle_.log(message);
    enqueue(CacheBoundMessage{message});
}

void RealisticMessageBus::send(const messages::ProbeS& message)
{
    oracle_.log(message);
    enqueue(CacheBoundMessage{message});
}

void RealisticMessageBus::send(const messages::ProbeI& message)
{
    oracle_.log(message);
    enqueue(CacheBoundMessage{message});
}

void RealisticMessageBus::send(const messages::EvictAck& message)
{
    oracle_.log(message);
    enqueue(CacheBoundMessage{message});
}

} // namespace mesi
