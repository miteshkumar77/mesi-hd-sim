#pragma once

#include "HandleResult.hpp"
#include "IHomeControllerClient.hpp"
#include "Messages.hpp"
#include "MesiOracle.hpp"
#include "Types.hpp"

#include <deque>
#include <iterator>
#include <list>
#include <tuple>
#include <unordered_map>
#include <variant>

namespace mesi {

class HomeController
{
public:
    void step();
    OracleToken oracleToken() const { return oracle_.oracleToken(); }
    OracleToken nextOracleToken() { return oracle_.nextOracleToken(); }

    explicit HomeController(Home homeId, MesiOracle& oracle)
        : homeId_{homeId}
        , oracle_{oracle}
    {}

    void setClient(IHomeControllerClient& client) { client_ = &client; }

    template<class Message>
    void deliver(const Message& message)
    {
        auto& received = std::get<std::list<Message>>(received_);
        received.emplace_back(message);
        fifo_.emplace_back(std::prev(received.end()));
    }

private:
    HandleResult handle(std::list<messages::GetS>::iterator it);
    HandleResult handle(std::list<messages::GetM>::iterator it);
    HandleResult handle(std::list<messages::ProbeAck>::iterator it);
    HandleResult handle(std::list<messages::EvictReq>::iterator it);

    void checkTransactions(Address lineBase);

    bool checkTransactionsImpl(Address lineBase);

    IHomeControllerClient* client_{nullptr};
    Home homeId_;
    MesiOracle& oracle_;

    using QueueEntry_t 
        = std::variant<
            std::list<messages::GetS>::iterator,
            std::list<messages::GetM>::iterator,
            std::list<messages::ProbeAck>::iterator,
            std::list<messages::EvictReq>::iterator>;

    std::deque<QueueEntry_t>  fifo_;
    std::tuple<
        std::list<messages::GetS>,
        std::list<messages::GetM>,
        std::list<messages::ProbeAck>,
        std::list<messages::EvictReq>> received_;


    struct HomeTransactionKey
    {
        bool operator==(const HomeTransactionKey& other) const = default;

        RequestId transactionId;
    };

    struct HomeAccessRequestTransaction
    {
        AccessType accessType;
        bool probed{false};
        bool active{false};
    };

    struct HomeEvictRequestTransaction {
        RequestId targetGrantId;
        bool      probed{false};
    };

    using HomeTransactionDetails_t =
        std::variant<HomeAccessRequestTransaction, HomeEvictRequestTransaction>;

    struct HomeTransaction
    {
        HomeTransactionKey key;
        Core core;
        HomeTransactionDetails_t details;

        HomeAccessRequestTransaction* getHomeAccessDetails()
        {
            return std::get_if<HomeAccessRequestTransaction>(&details);
        }

        HomeEvictRequestTransaction* getHomeEvictDetails()
        {
            return std::get_if<HomeEvictRequestTransaction>(&details);
        }
    };

    struct HomeCacheLineMeta
    {
        Data                       data_;
        std::list<HomeTransaction> transactions_;
    };

    bool tryHandleEvictTransaction(Address line, HomeCacheLineMeta& meta,
                                  std::list<HomeTransaction>::iterator trItForEviction);

    std::unordered_map<Address, HomeCacheLineMeta> lineToHomeMeta_;
};

} // namespace mesi
