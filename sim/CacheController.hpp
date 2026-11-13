#pragma once

#include "HandleResult.hpp"
#include "HomeLocator.hpp"
#include "ICacheControllerClient.hpp"
#include "Messages.hpp"
#include "MesiOracle.hpp"
#include "RequestIdAllocator.hpp"
#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>

namespace mesi {

struct CacheTransactionKey
{
    bool operator==(const CacheTransactionKey& other) const = default;

    RequestId transactionId;
};

struct CacheAccessRequestTransaction
{
    AccessType accessType;
    bool granted{false};
};

struct CacheEvictRequestTransaction {};

using CacheTransactionDetails_t =
    std::variant<CacheAccessRequestTransaction, CacheEvictRequestTransaction>;

struct CacheTransaction
{
    CacheTransactionKey key;
    CacheTransactionDetails_t details;

    CacheAccessRequestTransaction* getCacheAccessDetails()
    {
        return std::get_if<CacheAccessRequestTransaction>(&details);
    }

    CacheEvictRequestTransaction* getCacheEvictDetails()
    {
        return std::get_if<CacheEvictRequestTransaction>(&details);
    }
};

struct CacheLineMeta
{
    State state_;
    Data  data_;

    std::list<CacheTransaction> transactions_;
};

class CacheController
{
public:
    CacheLine inspect(Address address) const;

public:
    void step();
    OracleToken oracleToken() const { return oracle_.oracleToken(); }
    OracleToken nextOracleToken() { return oracle_.nextOracleToken(); }

    explicit CacheController(Core coreId, RequestIdAllocator requestIdAllocator, HomeLocator homeLocator, MesiOracle& oracle)
        : coreId_{coreId}
        , requestIdAllocator_{std::move(requestIdAllocator)}
        , homeLocator_{homeLocator}
        , oracle_{oracle}
    {}

    void setClient(ICacheControllerClient& client) { client_ = &client; }

    template<class Message>
    void deliver(const Message& message)
    {
        auto& received = std::get<std::list<Message>>(received_);
        received.emplace_back(message);
        fifo_.emplace_back(std::prev(received.end()));
    }

    void startLoad(Address address);
    bool addressReadable(Address address);
    Data load(Address address);

    void startStore(Address address);
    bool addressWriteable(Address address);
    void store(Address address, size_t idx, uint8_t value);

    void startEvict(Address address);
    bool addressHasEvictTransaction(Address address);

private:
    HandleResult handle(std::list<messages::Grant>::iterator it);
    HandleResult handle(std::list<messages::ProbeS>::iterator it);
    HandleResult handle(std::list<messages::ProbeI>::iterator it);
    HandleResult handle(std::list<messages::EvictAck>::iterator it);


    ICacheControllerClient* client_{nullptr};
    std::unordered_map<Address, CacheLineMeta> lineToMeta_;
    Core                                       coreId_;
    RequestIdAllocator                         requestIdAllocator_;
    HomeLocator                                homeLocator_;
    MesiOracle& oracle_;

    using QueueEntry_t
        = std::variant<
            std::list<messages::Grant>::iterator,
            std::list<messages::ProbeS>::iterator,
            std::list<messages::ProbeI>::iterator,
            std::list<messages::EvictAck>::iterator>;

    std::deque<QueueEntry_t> fifo_;
    std::tuple<
        std::list<messages::Grant>,
        std::list<messages::ProbeS>,
        std::list<messages::ProbeI>,
        std::list<messages::EvictAck>> received_;

};

} // namespace mesi
