#pragma once

#include "Messages.hpp"

#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

namespace mesi {

template<class Message>
std::string probeDetails(const Message& msg)
{
    std::ostringstream out;
    const auto controllerName = [](ControllerType type) {
        return type == ControllerType::HOME ? "HOME" : "CACHE";
    };
    out << " [source=" << controllerName(msg.header.sourceType)
        << ':' << msg.header.sourceId
        << ", destination=" << controllerName(msg.header.destinationType)
        << ':' << msg.header.destinationId
        << ", oracleToken=" << msg.header.oracleToken;
    if constexpr (std::is_same_v<Message, messages::ProbeAck>) {
        out << ", echoLine=" << msg.echoLine;
    } else {
        out << ", line=" << msg.line;
    }
    out << ", transactionId=" << msg.transactionId
        << ", targetGrantId=" << msg.targetGrantId;
    if constexpr (std::is_same_v<Message, messages::ProbeAck>) {
        out << ", payload=";
        if (const auto* data = std::get_if<Data>(&msg.payloadUnion)) {
            out << '[';
            for (std::size_t i = 0; i < data->size(); ++i) {
                if (i != 0) out << ", ";
                out << (*data)[i];
            }
            out << ']';
        } else {
            out << "none";
        }
    }
    out << ']';
    return out.str();
}

} // namespace mesi
