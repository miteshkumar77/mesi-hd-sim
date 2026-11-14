#pragma once

#include "CacheController.hpp"
#include "HomeController.hpp"
#include "ICacheControllerClient.hpp"
#include "IHomeControllerClient.hpp"
#include "Messages.hpp"
#include "MesiOracle.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <queue>
#include <variant>
#include <vector>

namespace mesi {

using HomeBoundMessage = std::variant<
    messages::GetS,
    messages::GetM,
    messages::ProbeAck,
    messages::EvictReq>;

using CacheBoundMessage = std::variant<
    messages::Grant,
    messages::ProbeS,
    messages::ProbeI,
    messages::EvictAck>;

using Message = std::variant<HomeBoundMessage, CacheBoundMessage>;

class IOrderingStrategy
{
public:
    virtual ~IOrderingStrategy() = default;

    virtual std::size_t getDispatchTick(const Message& message, std::size_t currentTick) = 0;
};

class FifoDelayedOrderingStrategy : public IOrderingStrategy
{
public:
    std::size_t getDispatchTick(const Message& message, std::size_t currentTick) override final;
};

class DelayedGrantOrderingStrategy : public IOrderingStrategy
{
public:
    std::size_t getDispatchTick(const Message& message, std::size_t currentTick) override final;
};

class RealisticMessageBus : public ICacheControllerClient, public IHomeControllerClient
{
public:
    void step();

    explicit RealisticMessageBus(
        std::vector<HomeController*> homesById,
        std::vector<CacheController*> cachesById,
        MesiOracle& oracle,
        std::unique_ptr<IOrderingStrategy> orderingStrategy);

    void send(const messages::GetS& message) override;
    void send(const messages::GetM& message) override;
    void send(const messages::ProbeAck& message) override;
    void send(const messages::EvictReq& message) override;
    void send(const messages::Grant& message) override;
    void send(const messages::ProbeS& message) override;
    void send(const messages::ProbeI& message) override;
    void send(const messages::EvictAck& message) override;

private:
    struct Event
    {
        std::size_t tick;
        Message message;

        bool operator>(const Event& other) const { return tick > other.tick; }
    };

    void enqueue(Message message);

    std::vector<HomeController*> homesById_;
    std::vector<CacheController*> cachesById_;
    MesiOracle& oracle_;
    std::unique_ptr<IOrderingStrategy> orderingStrategy_;
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events_;
    std::size_t currentTick_{0};
};

} // namespace mesi
