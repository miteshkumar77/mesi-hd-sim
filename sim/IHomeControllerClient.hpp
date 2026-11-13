#pragma once

#include "Messages.hpp"

namespace mesi {

class IHomeControllerClient
{
public:
    virtual ~IHomeControllerClient() = default;

    virtual void send(const messages::Grant& message) = 0;
    virtual void send(const messages::ProbeS& message) = 0;
    virtual void send(const messages::ProbeI& message) = 0;
    virtual void send(const messages::EvictAck& message) = 0;
};

} // namespace mesi
