#pragma once

#include "Messages.hpp"

namespace mesi {

class ICacheControllerClient
{
public:
    virtual ~ICacheControllerClient() = default;

    virtual void send(const messages::GetS& message) = 0;
    virtual void send(const messages::GetM& message) = 0;
    virtual void send(const messages::ProbeAck& message) = 0;
    virtual void send(const messages::EvictReq& message) = 0;
};

} // namespace mesi
