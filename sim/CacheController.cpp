#include "CacheController.hpp"
#include "ProbeDetails.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace mesi {

CacheLine CacheController::inspect(Address address) const
{
    const auto it = lineToMeta_.find(line_base(address));
    return it == lineToMeta_.end()
        ? CacheLine{}
        : CacheLine{it->second.state_, it->second.data_};
}

void CacheController::step()
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

HandleResult CacheController::handle(std::list<messages::Grant>::iterator grantIt)
{
    const messages::Grant& msg = *grantIt;
    auto cacheIt = lineToMeta_.find(msg.line);
    if (cacheIt == lineToMeta_.end()) {
        throw std::logic_error("cacheIt not found for line " + std::to_string(msg.line));
    }

    CacheLineMeta& meta = cacheIt->second;
    auto trGrantIt = std::find_if(meta.transactions_.begin(),
                             meta.transactions_.end(),
                             [&msg](const CacheTransaction& transaction) -> bool {
                                return transaction.key == CacheTransactionKey{
                                    .transactionId=msg.requestId,
                                };
                             });

    if (trGrantIt == meta.transactions_.end()) {
        throw std::logic_error("trGrantIt not found for transaction "
            + std::to_string(msg.requestId));
    }

    trGrantIt->getCacheAccessDetails()->granted = true;
    meta.data_ = msg.data;
    if (msg.accessType == AccessType::WRITE) {
        trGrantIt->getCacheAccessDetails()->accessType = msg.accessType;
        for (auto trIt = meta.transactions_.begin();
             trIt != trGrantIt;) {
            
            if (trIt->getCacheAccessDetails()->accessType == AccessType::READ && trIt->getCacheAccessDetails()->granted) {
                trIt = meta.transactions_.erase(trIt);
            } else {
                // a read can be the only preceding transaction
                throw std::logic_error("invalid!");
            }
        }
        meta.state_ = State::E;
    } else {
        meta.state_ = State::S;
    }
    return HandleResult::REMOVE;
}

HandleResult CacheController::handle(std::list<messages::ProbeS>::iterator probeSIt)
{
    const messages::ProbeS& msg = *probeSIt;
    auto cacheIt = lineToMeta_.find(msg.line);
    if (cacheIt == lineToMeta_.end()) {
        throw std::logic_error("ProbeS: cache line not found" + probeDetails(msg));
    }
    CacheLineMeta& meta = cacheIt->second;
    auto trIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [&msg](const CacheTransaction& transaction) -> bool {
            return transaction.key == CacheTransactionKey{
                .transactionId=msg.targetGrantId,
            };
        });

    if (trIt == meta.transactions_.end()) {
        throw std::logic_error("ProbeS: target grant transaction not found" + probeDetails(msg));
    }

    if (trIt->getCacheAccessDetails()->accessType != AccessType::WRITE) {
        throw std::logic_error("ProbeS: target transaction lacks write access" + probeDetails(msg));
    }

    if (!trIt->getCacheAccessDetails()->granted) {
        return HandleResult::DEFER;
    }

    client_->send(messages::ProbeAck{
        .header=messages::MessageHeader{
            .sourceType=ControllerType::CACHE,
            .sourceId=coreId_,
            .destinationType=ControllerType::HOME,
            .destinationId=msg.header.sourceId,
            .oracleToken=nextOracleToken(),
        },
        .echoLine=msg.line,
        .transactionId=msg.transactionId,
        .targetGrantId=msg.targetGrantId,
        .payloadUnion=(
            meta.state_ == State::M ?
            std::variant<Data, std::monostate>(meta.data_) :
            std::variant<Data, std::monostate>(std::monostate{})),
    });

    trIt->getCacheAccessDetails()->accessType = AccessType::READ;
    meta.state_ = State::S;

    return HandleResult::REMOVE;
}

HandleResult CacheController::handle(std::list<messages::ProbeI>::iterator probeIIt)
{
    const messages::ProbeI& msg = *probeIIt;
    auto cacheIt = lineToMeta_.find(msg.line);
    if (cacheIt == lineToMeta_.end()) {
        throw std::logic_error("ProbeI: cache line not found" + probeDetails(msg));
    }
    CacheLineMeta& meta = cacheIt->second;
    auto trIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [&msg](const CacheTransaction& transaction) -> bool {
            return transaction.key == CacheTransactionKey{
                .transactionId=msg.targetGrantId,
            };
        });

    if (trIt == meta.transactions_.end()) {
        throw std::logic_error("ProbeI: target grant transaction not found" + probeDetails(msg));
    }

    if (!trIt->getCacheAccessDetails()->granted) {
        return HandleResult::DEFER;
    }

    client_->send(messages::ProbeAck{
        .header=messages::MessageHeader{
            .sourceType=ControllerType::CACHE,
            .sourceId=coreId_,
            .destinationType=ControllerType::HOME,
            .destinationId=msg.header.sourceId,
            .oracleToken=nextOracleToken(),
        },
        .echoLine=msg.line,
        .transactionId=msg.transactionId,
        .targetGrantId=msg.targetGrantId,
        .payloadUnion=(
            meta.state_ == State::M ?
            std::variant<Data, std::monostate>(meta.data_) :
            std::variant<Data, std::monostate>(std::monostate{})),
    });


    meta.transactions_.erase(trIt);
    meta.state_ = State::I;

    return HandleResult::REMOVE;
}

HandleResult CacheController::handle(std::list<messages::EvictAck>::iterator evictAckIt)
{
    const messages::EvictAck& msg = *evictAckIt;
    auto cacheIt = lineToMeta_.find(msg.line);
    if (cacheIt == lineToMeta_.end()) {
        throw std::logic_error("EvictAck: cache line not found");
    }

    CacheLineMeta& meta = cacheIt->second;
    auto trIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [&msg](CacheTransaction& transaction) {
            return transaction.key == CacheTransactionKey{
                .transactionId=msg.requestId,
            } && transaction.getCacheEvictDetails() != nullptr;
        });
    if (trIt == meta.transactions_.end()) {
        throw std::logic_error("EvictAck: eviction transaction not found");
    }

    meta.transactions_.erase(trIt);
    return HandleResult::REMOVE;
}

void CacheController::startLoad(Address address)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end()) {
        it = lineToMeta_.emplace(lineBase, CacheLineMeta{
            .state_=State::I,
            .data_=Data{},
        }).first;
    }

    if (it->second.state_ != State::I) return;

    CacheTransaction& transaction =
        it->second.transactions_.emplace_back(
            CacheTransaction{
                .key=CacheTransactionKey{
                    .transactionId=requestIdAllocator_.fetchAddRequestId(),
                },
                .details=CacheAccessRequestTransaction{
                    .accessType=AccessType::READ,
                },
            });
    
    client_->send(messages::GetS{
        .header=messages::MessageHeader{
            .sourceType=ControllerType::CACHE,
            .sourceId=coreId_,
            .destinationType=ControllerType::HOME,
            .destinationId=homeLocator_.locateHomeId(lineBase),
            .oracleToken=nextOracleToken(),
        },
        .line=lineBase,
        .requestId=transaction.key.transactionId,
    });
}

bool CacheController::addressReadable(Address address)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end()) {
        throw std::logic_error(
            "address readable needs cache line!");
    }

    return it->second.state_ != State::I;
}


Data CacheController::load(Address address)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end()) {
        throw std::logic_error(
            "address readable needs cache line!");
    }

    if (it->second.state_ == State::I) {
        throw std::logic_error(
            "address load invoked on cache line state I!");
    }

    return it->second.data_;
}


void CacheController::startStore(Address address)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end()) {
        it = lineToMeta_.emplace(lineBase, CacheLineMeta{
            .state_=State::I,
        }).first;
    }

    if (it->second.state_ == State::E ||
        it->second.state_ == State::M) {
        return;
    }
    CacheTransaction& transaction =
        it->second.transactions_.emplace_back(CacheTransaction{
            .key=CacheTransactionKey{
                .transactionId=requestIdAllocator_.fetchAddRequestId(),
            },
            .details=CacheAccessRequestTransaction{
                .accessType=AccessType::WRITE,
            },
        });
    client_->send(messages::GetM{
        .header=messages::MessageHeader{
            .sourceType=ControllerType::CACHE,
            .sourceId=coreId_,
            .destinationType=ControllerType::HOME,
            .destinationId=homeLocator_.locateHomeId(lineBase),
            .oracleToken=nextOracleToken(),
        },
        .line=lineBase,
        .requestId=transaction.key.transactionId,
    });
}

bool CacheController::addressWriteable(Address address)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end()) {
        throw std::logic_error(
            "address writeable needs cache line!");
    }

    return it->second.state_ == State::E ||
           it->second.state_ == State::M;
}

void CacheController::store(Address address, size_t idx, uint8_t value)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end()) {
        throw std::logic_error(
            "address writeable needs cache line!");
    }

    if (it->second.state_ != State::E &&
        it->second.state_ != State::M) {

        const std::string stateStr({letter(it->second.state_)});

        throw std::logic_error(
            "address store invoked on cache line state " + stateStr + "!");
    }

    Data& data = it->second.data_;
    data[idx] = value;
    it->second.state_ = State::M;
}

void CacheController::startEvict(Address address)
{
    const Address lineBase{line_base(address)};
    auto it = lineToMeta_.find(lineBase);
    if (it == lineToMeta_.end() || it->second.state_ == State::I) return;

    CacheLineMeta& meta = it->second;
    auto grantIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [](CacheTransaction& transaction) {
            auto* access = transaction.getCacheAccessDetails();
            return access && access->granted;
        });
    if (grantIt == meta.transactions_.end()) {
        throw std::logic_error("startEvict: granted transaction not found");
    }

    CacheTransaction& transaction = meta.transactions_.emplace_back(CacheTransaction{
        .key=CacheTransactionKey{
            .transactionId=requestIdAllocator_.fetchAddRequestId(),
        },
        .details=CacheEvictRequestTransaction{},
    });
    client_->send(messages::EvictReq{
        .header=messages::MessageHeader{
            .sourceType=ControllerType::CACHE,
            .sourceId=coreId_,
            .destinationType=ControllerType::HOME,
            .destinationId=homeLocator_.locateHomeId(lineBase),
            .oracleToken=nextOracleToken(),
        },
        .line=lineBase,
        .requestId=transaction.key.transactionId,
        .targetGrantId=grantIt->key.transactionId,
    });
}

bool CacheController::addressHasEvictTransaction(Address address)
{
    auto cacheIt = lineToMeta_.find(line_base(address));
    if (cacheIt == lineToMeta_.end()) return false;
    CacheLineMeta& meta = cacheIt->second;
    auto trIt = std::find_if(meta.transactions_.begin(), meta.transactions_.end(),
        [](CacheTransaction& transaction) {
            return transaction.getCacheEvictDetails() != nullptr;
        });
    return trIt != meta.transactions_.end();
}

} // namespace mesi
