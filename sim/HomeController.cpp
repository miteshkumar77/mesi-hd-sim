#include "HomeController.hpp"
#include "ProbeDetails.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mesi {

void HomeController::step()
{
    if (fifo_.empty()) return;

    QueueEntry_t entry = fifo_.front();
    fifo_.pop_front();
    std::visit([this](auto it) {
        if (handle(it) == HandleResult::DEFER) {
            fifo_.emplace_back(it);
        } else {
            using Message = typename std::iterator_traits<decltype(it)>::value_type;
            std::get<std::list<Message>>(received_).erase(it);
        }
    }, entry);
}

HandleResult HomeController::handle(std::list<messages::GetS>::iterator getSIt)
{
    const messages::GetS& msg = *getSIt;

    auto cacheIt = lineToHomeMeta_.find(msg.line);
    if (cacheIt == lineToHomeMeta_.end()) {
        cacheIt = lineToHomeMeta_.emplace(msg.line,
            HomeCacheLineMeta{}).first;
    }

    HomeTransaction& transaction
        = cacheIt->second.transactions_.emplace_back(HomeTransaction{
            .key=HomeTransactionKey{
                .transactionId=msg.requestId,
            },
            .core=msg.header.sourceId,
            .details=HomeAccessRequestTransaction{
                .accessType=AccessType::READ,
            },
        });
    
    if (cacheIt->second.transactions_.size() == 1) {
        transaction.getHomeAccessDetails()->accessType = AccessType::WRITE;
    }

    checkTransactions(msg.line);
    return HandleResult::REMOVE;
}

HandleResult HomeController::handle(std::list<messages::GetM>::iterator getMIt)
{
    const messages::GetM& msg = *getMIt;

    auto cacheIt = lineToHomeMeta_.find(msg.line);
    if (cacheIt == lineToHomeMeta_.end()) {
        cacheIt = lineToHomeMeta_.emplace(msg.line,
            HomeCacheLineMeta{}).first;
    }

    HomeTransaction& transaction =
        cacheIt->second.transactions_.emplace_back(HomeTransaction{
            .key=HomeTransactionKey{
                .transactionId=msg.requestId,
            },
            .core=msg.header.sourceId,
            .details=HomeAccessRequestTransaction{
                .accessType=AccessType::WRITE,
            },
        });

    checkTransactions(msg.line);
    return HandleResult::REMOVE;
}

HandleResult HomeController::handle(std::list<messages::ProbeAck>::iterator probeAckIt)
{
    const messages::ProbeAck& msg = *probeAckIt;
    auto cacheIt = lineToHomeMeta_.find(msg.echoLine);
    if (cacheIt == lineToHomeMeta_.end()) {
        throw std::logic_error("ProbeAck: home cache line not found" + probeDetails(msg));
    }

    HomeCacheLineMeta& meta = cacheIt->second;

    // the transaction holding the access which was granted and is being revoked
    auto trIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [&msg](const HomeTransaction& transaction) -> bool {
            return transaction.key == HomeTransactionKey{
                .transactionId=msg.targetGrantId,
            };
        });
    
    // the transaction that requires trIt to be demoted before becoming active
    auto sourceTrIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [&msg](const HomeTransaction& transaction) -> bool {
            return transaction.key == HomeTransactionKey{
                .transactionId=msg.transactionId,
            };
        });
    
    if (trIt == meta.transactions_.end()) {
        throw std::logic_error("ProbeAck: target grant transaction not found" + probeDetails(msg));
    }

    if (sourceTrIt == meta.transactions_.end()) {
        throw std::logic_error("ProbeAck: source transaction not found" + probeDetails(msg));
    }

    if (!trIt->getHomeAccessDetails()->active) {
        throw std::logic_error("ProbeAck: target grant transaction is inactive" + probeDetails(msg));
    }

    if (
        Data const * const writeBack =
        std::get_if<Data>(&msg.payloadUnion); writeBack) {
            
        meta.data_ = *writeBack;
    }

    if (sourceTrIt->getHomeAccessDetails()) {
        if (sourceTrIt->getHomeAccessDetails()->active) {
            throw std::logic_error("ProbeAck: source transaction is already active" + probeDetails(msg));
        }

        if (sourceTrIt->getHomeAccessDetails()->accessType == AccessType::READ) {
            // downgraded to read access
            trIt->getHomeAccessDetails()->accessType = AccessType::READ;
        } else {
            meta.transactions_.erase(trIt);
        }
    } else {
        HomeEvictRequestTransaction* const sourceEvictDetails = sourceTrIt->getHomeEvictDetails();
        if (!sourceEvictDetails) {
            throw std::runtime_error("what!");
        }
        // erasing trIt here will allow us to make progress on the checkTransactions call below
        meta.transactions_.erase(trIt);
    }

    checkTransactions(msg.echoLine);

    return HandleResult::REMOVE;
}

HandleResult HomeController::handle(std::list<messages::EvictReq>::iterator evictReqIt)
{
    const messages::EvictReq& msg = *evictReqIt;
    auto cacheIt = lineToHomeMeta_.find(msg.line);
    if (cacheIt == lineToHomeMeta_.end()) {
        std::ostringstream out;
        out << "EvictReq: home cache line not found"
            << " [homeId=" << homeId_
            << ", sourceCore=" << msg.header.sourceId
            << ", line=0x" << std::hex << msg.line << std::dec
            << ", requestId=" << msg.requestId
            << ", targetGrantId=" << msg.targetGrantId
            << ", oracleToken=" << msg.header.oracleToken << ']';
        throw std::runtime_error(out.str());
    }

    HomeCacheLineMeta& meta = cacheIt->second;
    HomeTransaction& transaction = meta.transactions_.emplace_back(HomeTransaction{
        .key=HomeTransactionKey{
            .transactionId=msg.requestId
        },
        .core=msg.header.sourceId,
        .details=HomeEvictRequestTransaction{
            .targetGrantId=msg.targetGrantId,
        }
    });

    checkTransactions(msg.line);

    return HandleResult::REMOVE;
}

bool HomeController::tryHandleEvictTransaction(
    Address lineBase,
    HomeCacheLineMeta& meta, std::list<HomeTransaction>::iterator trItForEviction)
{
    if (!trItForEviction->getHomeEvictDetails()) {
        std::ostringstream out;
        out << "tryHandleEvictTransaction: transaction is not an eviction"
            << " [homeId=" << homeId_
            << ", line=0x" << std::hex << lineBase << std::dec
            << ", transactionId=" << trItForEviction->key.transactionId
            << ", core=" << trItForEviction->core
            << ", transactionCount=" << meta.transactions_.size() << ']';
        throw std::logic_error(out.str());
    }
    HomeEvictRequestTransaction& evictDetail = *trItForEviction->getHomeEvictDetails();
    auto grantedTrIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [&evictDetail](const HomeTransaction& transaction) -> bool {
            return transaction.key == HomeTransactionKey{
                .transactionId=evictDetail.targetGrantId,
            };
        });
    if (grantedTrIt == meta.transactions_.end()) {
        client_->send(messages::EvictAck{
            .header=messages::MessageHeader{
                .sourceType=ControllerType::HOME,
                .sourceId=homeId_,
                .destinationType=ControllerType::CACHE,
                .destinationId=trItForEviction->core,
                .oracleToken=nextOracleToken(),
            },
            .line=lineBase,
            .requestId=trItForEviction->key.transactionId,
            .targetGrantId=evictDetail.targetGrantId,
        });
        meta.transactions_.erase(trItForEviction);
        return true;
    }
    if (evictDetail.probed) return false;
    evictDetail.probed = true;
    client_->send(messages::ProbeI{
        .header=messages::MessageHeader{
            .sourceType=ControllerType::HOME,
            .sourceId=homeId_,
            .destinationType=ControllerType::CACHE,
            .destinationId=trItForEviction->core,
            .oracleToken=nextOracleToken(),
        },
        .line=lineBase,
        .transactionId=trItForEviction->key.transactionId,
        .targetGrantId=grantedTrIt->key.transactionId,
    });
    return false;
}


void HomeController::checkTransactions(Address lineBase)
{
    while(checkTransactionsImpl(lineBase));
}

bool HomeController::checkTransactionsImpl(Address lineBase)
{
    auto it = lineToHomeMeta_.find(lineBase);
    if (it == lineToHomeMeta_.end()) {
        throw std::logic_error("line base not present " + std::to_string(lineBase));
    }
    HomeCacheLineMeta& meta = it->second;
    bool priorActiveRead  = false;
    bool priorActiveWrite = false;

    auto trItToService = meta.transactions_.end();
    auto trItForEviction = meta.transactions_.end();
    for (auto trIt = meta.transactions_.begin();
         trIt != meta.transactions_.end(); ++trIt) {
        
        if (trIt->getHomeEvictDetails() != nullptr)
        {
            trItForEviction = trIt;
            break;
        }
        
        if (!trIt->getHomeAccessDetails()->active) {
            trItToService = trIt;
            break;
        }
        if (trIt->getHomeAccessDetails()->accessType == AccessType::READ) {
            priorActiveRead = true;
        } else if (trIt->getHomeAccessDetails()->accessType == AccessType::WRITE) {
            priorActiveWrite = true;
        }
    }

    if (trItForEviction != meta.transactions_.end()) {
        return tryHandleEvictTransaction(lineBase, meta, trItForEviction);
    }

    if (trItToService == meta.transactions_.end()) return false;
    HomeTransaction& transaction = *trItToService;

    bool canGrant{false};
    bool isWriteUpgrade{false};
    if (transaction.getHomeAccessDetails()->accessType == AccessType::READ) {
        canGrant = !priorActiveWrite;
    } else if (transaction.getHomeAccessDetails()->accessType == AccessType::WRITE) {
        /**
         * If the core requests write access and it is the only active
         * shared grant for this cache line, we can upgrade it to write
         * without invalidating its existing read grant
         */
        isWriteUpgrade = bool{
            trItToService != meta.transactions_.begin() &&
            prev(trItToService)->core == trItToService->core &&
            prev(trItToService) == meta.transactions_.begin()
        };
        canGrant = isWriteUpgrade || (!priorActiveRead && !priorActiveWrite);
    }


    if (canGrant) {
        if (isWriteUpgrade) {
            // this grant supersedes the previous read grant
            // to the same core. That means, upon receiving this grant
            // the core will also delete its read transaction entry
            meta.transactions_.pop_front();
        }
        transaction.getHomeAccessDetails()->active = true;
        client_->send(messages::Grant{
            .header=messages::MessageHeader{
                .sourceType=ControllerType::HOME,
                .sourceId=homeId_,
                .destinationType=ControllerType::CACHE,
                .destinationId=transaction.core,
                .oracleToken=nextOracleToken(),
            },
            .line=it->first,
            .requestId=transaction.key.transactionId,
            .accessType=transaction.getHomeAccessDetails()->accessType,
            .data=meta.data_,
        });
        return true;
    }

    if (transaction.getHomeAccessDetails()->probed) return false;

    transaction.getHomeAccessDetails()->probed = true;

    for (auto trIt = meta.transactions_.begin();
         trIt != trItToService; ++trIt) {

        const bool requiresProbe = 
            ((transaction.getHomeAccessDetails()->accessType == AccessType::READ &&
             trIt->getHomeAccessDetails()->accessType == AccessType::WRITE) ||
            (transaction.getHomeAccessDetails()->accessType == AccessType::WRITE)) &&
            transaction.core != trIt->core;
        

        if (requiresProbe) {
            const messages::MessageHeader hdr {
                .sourceType=ControllerType::HOME,
                .sourceId=homeId_,
                .destinationType=ControllerType::CACHE,
                .destinationId=trIt->core,
                .oracleToken=nextOracleToken(),
            };
            if (transaction.getHomeAccessDetails()->accessType == AccessType::READ) {
                client_->send(messages::ProbeS{
                    .header=hdr,
                    .line=it->first,
                    .transactionId=transaction.key.transactionId,
                    .targetGrantId=trIt->key.transactionId,
                });
            } else {
                if (transaction.getHomeAccessDetails()->accessType != AccessType::WRITE) {
                    throw std::logic_error("corrupted accessType!");
                }
                client_->send(messages::ProbeI{
                    .header=hdr,
                    .line=it->first,
                    .transactionId=transaction.key.transactionId,
                    .targetGrantId=trIt->key.transactionId,
                });
            }
        }
    }

    return false;
}

} // namespace mesi
