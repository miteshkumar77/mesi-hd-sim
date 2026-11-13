#include "MesiOracle.hpp"

namespace mesi {

MesiOracle::MesiOracle(const std::string& outputPath)
{
    output_.exceptions(std::ios::failbit | std::ios::badbit);
    output_.open(outputPath);
}

void MesiOracle::logHeader(const char* type, const messages::MessageHeader& header)
{
    output_ << type
            << " sourceType=" << (header.sourceType == ControllerType::CACHE ? "CACHE" : "HOME")
            << " sourceId=" << header.sourceId
            << " destinationType=" << (header.destinationType == ControllerType::CACHE ? "CACHE" : "HOME")
            << " destinationId=" << header.destinationId
            << " oracleToken=" << header.oracleToken;
}

void MesiOracle::logData(const Data& data)
{
    output_ << '[';
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i != 0) output_ << ',';
        output_ << static_cast<uint64_t>(data[i]);
    }
    output_ << ']';
}

void MesiOracle::log(const messages::GetS& message)
{
    logHeader("GetS", message.header);
    output_ << " line=" << message.line;
    output_ << " requestId=" << message.requestId;
    output_ << std::endl;
}

void MesiOracle::log(const messages::GetM& message)
{
    logHeader("GetM", message.header);
    output_ << " line=" << message.line;
    output_ << " requestId=" << message.requestId;
    output_ << std::endl;
}

void MesiOracle::log(const messages::Grant& message)
{
    logHeader("Grant", message.header);
    output_ << " line=" << message.line;
    output_ << " requestId=" << message.requestId;
    output_ << " accessType=" << (message.accessType == AccessType::READ ? "READ" : "WRITE");
    output_ << " data=";
    logData(message.data);
    output_ << std::endl;
}

void MesiOracle::log(const messages::ProbeS& message)
{
    logHeader("ProbeS", message.header);
    output_ << " line=" << message.line;
    output_ << " transactionId=" << message.transactionId;
    output_ << " targetGrantId=" << message.targetGrantId;
    output_ << std::endl;
}

void MesiOracle::log(const messages::ProbeI& message)
{
    logHeader("ProbeI", message.header);
    output_ << " line=" << message.line;
    output_ << " transactionId=" << message.transactionId;
    output_ << " targetGrantId=" << message.targetGrantId;
    output_ << std::endl;
}

void MesiOracle::log(const messages::ProbeAck& message)
{
    logHeader("ProbeAck", message.header);
    output_ << " echoLine=" << message.echoLine;
    output_ << " transactionId=" << message.transactionId;
    output_ << " targetGrantId=" << message.targetGrantId;
    output_ << " payload=";
    if (const auto* data = std::get_if<Data>(&message.payloadUnion)) {
        logData(*data);
    } else {
        output_ << "none";
    }
    output_ << std::endl;
}

void MesiOracle::log(const messages::EvictReq& message)
{
    logHeader("EvictReq", message.header);
    output_ << " line=" << message.line;
    output_ << " requestId=" << message.requestId;
    output_ << " targetGrantId=" << message.targetGrantId;
    output_ << std::endl;
}

void MesiOracle::log(const messages::EvictAck& message)
{
    logHeader("EvictAck", message.header);
    output_ << " line=" << message.line;
    output_ << " requestId=" << message.requestId;
    output_ << " targetGrantId=" << message.targetGrantId;
    output_ << std::endl;
}

} // namespace mesi
