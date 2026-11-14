#pragma once

#include "CacheController.hpp"
#include "FifoMessageBus.hpp"
#include "HomeController.hpp"
#include "ICacheControllerClient.hpp"
#include "IHomeControllerClient.hpp"
#include "Messages.hpp"
#include "MesiOracle.hpp"
#include "RealisticMessageBus.hpp"
#include "Types.hpp"

#include <map>
#include <vector>

namespace mesi {

class InteractiveMessageBus : public ICacheControllerClient, public IHomeControllerClient
{
public:
    void step();

    explicit InteractiveMessageBus(
        std::vector<HomeController*> homesById,
        std::vector<CacheController*> cachesById,
        MesiOracle& oracle);

    void send(const messages::GetS& message) override;
    void send(const messages::GetM& message) override;
    void send(const messages::ProbeAck& message) override;
    void send(const messages::EvictReq& message) override;
    void send(const messages::Grant& message) override;
    void send(const messages::ProbeS& message) override;
    void send(const messages::ProbeI& message) override;
    void send(const messages::EvictAck& message) override;

    const std::map<OracleToken, Message>& pendingMessages() const { return pendingMessages_; }

    void dispatchOracleToken(OracleToken oracleToken);

private:
    void store(OracleToken oracleToken, Message message);

    FifoMessageBus fifoBus_;
    std::map<OracleToken, Message> pendingMessages_;
};

} // namespace mesi
