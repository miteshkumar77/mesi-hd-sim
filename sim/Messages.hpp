#pragma once

#include "Types.hpp"

#include <variant>

namespace mesi {

namespace messages {
struct MessageHeader
{
    ControllerType sourceType;
    std::size_t    sourceId;

    ControllerType destinationType;
    std::size_t    destinationId;

    OracleToken    oracleToken;
};

struct GetS
{ // cache -> home
    MessageHeader header;
    Address line;
    RequestId requestId;
};

struct GetM
{ // cache -> home
    MessageHeader header;
    Address line;
    RequestId requestId;
};

struct Grant
{ // home -> cache
    MessageHeader header;
    Address    line;
    RequestId  requestId;
    AccessType accessType;
    Data       data;
};

struct ProbeS
{ // home -> cache
    MessageHeader header;
    Address    line;
    RequestId  transactionId;
    RequestId  targetGrantId;
};

struct ProbeI
{ // home -> cache
    MessageHeader header;
    Address    line;
    RequestId  transactionId;
    RequestId  targetGrantId;
};

struct ProbeAck
{ // cache -> home
    MessageHeader header;
    Address                            echoLine;
    RequestId                          transactionId;
    RequestId                          targetGrantId;
    std::variant<Data, std::monostate> payloadUnion;
};

struct EvictReq
{ // cache -> home
    MessageHeader header;
    Address   line;
    RequestId requestId;
    RequestId targetGrantId;
};

struct EvictAck
{ // home -> cache
    MessageHeader header;
    Address   line;
    RequestId requestId;
    RequestId targetGrantId;
};

}

} // namespace mesi
