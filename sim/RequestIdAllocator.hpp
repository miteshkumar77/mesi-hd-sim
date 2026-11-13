#pragma once

#include "Types.hpp"

#include <cstddef>

namespace mesi {

struct RequestIdAllocator
{

    RequestIdAllocator(Core coreId, size_t num_cores)
        : m_num_cores{num_cores}
        , m_nextId{coreId}
    {}


    RequestId fetchAddRequestId()
    {
        RequestId id = m_nextId;
        m_nextId += m_num_cores;
        return id;
    }

private:
    size_t m_num_cores;
    RequestId m_nextId;
};

} // namespace mesi
