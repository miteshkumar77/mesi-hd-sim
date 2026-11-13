#pragma once

#include "CacheController.hpp"
#include "HomeController.hpp"
#include "ICacheControllerClient.hpp"
#include "IHomeControllerClient.hpp"
#include "Messages.hpp"
#include "MesiOracle.hpp"

#include <utility>
#include <vector>

namespace mesi {

class MessageBus : public ICacheControllerClient, public IHomeControllerClient
{
public:
    void step();

    explicit MessageBus(
        std::vector<HomeController*> homesById,
        std::vector<CacheController*> cachesById,
        MesiOracle& oracle)
        : homesById_{std::move(homesById)}
        , cachesById_{std::move(cachesById)}
        , oracle_{oracle}
    {}

    void send(const messages::GetS& message) override
    {
        oracle_.log(message);
        homesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::GetM& message) override
    {
        oracle_.log(message);
        homesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::ProbeAck& message) override
    {
        oracle_.log(message);
        homesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::EvictReq& message) override
    {
        oracle_.log(message);
        homesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::Grant& message) override
    {
        oracle_.log(message);
        cachesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::ProbeS& message) override
    {
        oracle_.log(message);
        cachesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::ProbeI& message) override
    {
        oracle_.log(message);
        cachesById_.at(message.header.destinationId)->deliver(message);
    }

    void send(const messages::EvictAck& message) override
    {
        oracle_.log(message);
        cachesById_.at(message.header.destinationId)->deliver(message);
    }

private:
    std::vector<HomeController*> homesById_;
    std::vector<CacheController*> cachesById_;
    MesiOracle& oracle_;
};

} // namespace mesi
