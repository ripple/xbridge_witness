#pragma once

#include <ripple/basics/hardened_hash.h>
#include <ripple/protocol/STXChainBridge.h>

#include <array>
#include <string>
#include <tuple>
#include <utility>

template <>
struct std::hash<ripple::STXChainBridge>
{
    std::size_t
    operator()(ripple::STXChainBridge const& b) const noexcept
    {
        beast::xxhasher hasher;
        beast::hash_append(hasher, b.lockingChainDoor());
        ripple::hash_append(hasher, b.lockingChainIssue());
        beast::hash_append(hasher, b.issuingChainDoor());
        ripple::hash_append(hasher, b.issuingChainIssue());
        const std::size_t h = static_cast<std::size_t>(hasher);
        return h;
    }
};

namespace xbwd {

using ChainType = ripple::STXChainBridge::ChainType;
enum class ChainDir { issuingToLocking, lockingToIssuing };

inline std::string
to_string(ChainType ct)
{
    if (ct == ChainType::locking)
    {
        static std::string r{"locking"};
        return r;
    }
    static std::string r{"issuing"};
    return r;
}

inline std::string
to_string(ChainDir cd)
{
    if (cd == ChainDir::lockingToIssuing)
    {
        static std::string r{"lockingToIssuing"};
        return r;
    }
    static std::string r{"issuingToLocking"};
    return r;
}

inline ChainType
otherChain(ChainType ct)
{
    return ct == ChainType::locking ? ChainType::issuing : ChainType::locking;
}

// Array indexed by an enum class
template <class T>
class ChainArray
{
    std::array<T, 2> array_;

public:
    ChainArray() = default;
    template <class L, class I>
    explicit ChainArray(L&& l, I&& i)
        : array_{T{std::forward<L>(l)}, T{std::forward<I>(i)}}
    {
    }

    T&
    operator[](ChainType i)
    {
        return array_[static_cast<int>(i)];
    }

    T const&
    operator[](ChainType i) const
    {
        return array_[static_cast<int>(i)];
    }

    // For structured bindings
    auto
    get()
    {
        return std::tie(array_[0], array_[1]);
    }

    auto
    get() const
    {
        return std::tie(array_[0], array_[1]);
    }

    auto
    size() const
    {
        return array_.size();
    }

    auto
    begin()
    {
        return array_.begin();
    }
    auto
    end()
    {
        return array_.end();
    }
    auto
    begin() const
    {
        return array_.begin();
    }
    auto
    end() const
    {
        return array_.end();
    }
};

}  // namespace xbwd
