#include "InteractiveMessageBus.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace mesi {

InteractiveMessageBus::InteractiveMessageBus(
    std::vector<HomeController*> homesById,
    std::vector<CacheController*> cachesById,
    MesiOracle& oracle)
    : fifoBus_{std::move(homesById), std::move(cachesById), oracle}
{}

void InteractiveMessageBus::step()
{
    fifoBus_.step();
}

void InteractiveMessageBus::store(OracleToken oracleToken, Message message)
{
    const auto [it, inserted] = pendingMessages_.emplace(oracleToken, std::move(message));
    if (!inserted) {
        throw std::logic_error("duplicate oracleToken: " + std::to_string(oracleToken));
    }
}

void InteractiveMessageBus::dispatchOracleToken(OracleToken oracleToken)
{
    const auto it = pendingMessages_.find(oracleToken);
    if (it == pendingMessages_.end()) {
        throw std::out_of_range("unknown oracleToken: " + std::to_string(oracleToken));
    }

    // remove before delivering: delivery may trigger sends that add to the map
    const Message message = std::move(it->second);
    pendingMessages_.erase(it);

    std::visit([this](const auto& bound) {
        std::visit([this](const auto& msg) { fifoBus_.send(msg); }, bound);
    }, message);
}

void InteractiveMessageBus::send(const messages::GetS& message)
{
    store(message.header.oracleToken, HomeBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::GetM& message)
{
    store(message.header.oracleToken, HomeBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::ProbeAck& message)
{
    store(message.header.oracleToken, HomeBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::EvictReq& message)
{
    store(message.header.oracleToken, HomeBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::Grant& message)
{
    store(message.header.oracleToken, CacheBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::ProbeS& message)
{
    store(message.header.oracleToken, CacheBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::ProbeI& message)
{
    store(message.header.oracleToken, CacheBoundMessage{message});
}

void InteractiveMessageBus::send(const messages::EvictAck& message)
{
    store(message.header.oracleToken, CacheBoundMessage{message});
}

} // namespace mesi
