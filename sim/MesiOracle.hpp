#pragma once

#include "Messages.hpp"

#include <string>
#include <fstream>

namespace mesi {

class MesiOracle
{
public:
    explicit MesiOracle(const std::string& outputPath);
    OracleToken oracleToken() const { return oracleToken_; }
    OracleToken nextOracleToken() { return ++oracleToken_; }

    void log(const messages::GetS& message);
    void log(const messages::GetM& message);
    void log(const messages::Grant& message);
    void log(const messages::ProbeS& message);
    void log(const messages::ProbeI& message);
    void log(const messages::ProbeAck& message);
    void log(const messages::EvictReq& message);
    void log(const messages::EvictAck& message);

private:
    void logHeader(const char* type, const messages::MessageHeader& header);
    void logData(const Data& data);

    OracleToken oracleToken_{0};
    std::ofstream output_;
};

} // namespace mesi
