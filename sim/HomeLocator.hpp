#pragma once

#include "Types.hpp"

#include <cstddef>

namespace mesi {

struct HomeLocator
{
public:

    explicit HomeLocator(std::size_t homes)
        : homes_{homes}
    {}

    Home locateHomeId(Address byteAddress)
    {
        const Address line{line_base(byteAddress)};
        return line % homes_;
    }

private:
    std::size_t homes_;
};

} // namespace mesi
