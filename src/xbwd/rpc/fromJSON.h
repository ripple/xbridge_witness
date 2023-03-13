#pragma once

#include <xbwd/basics/ChainTypes.h>

#include <ripple/beast/net/IPEndpoint.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/Seed.h>

#include <boost/asio/ip/address.hpp>
#include <boost/filesystem.hpp>

#include <charconv>
#include <limits>
#include <string>

namespace xbwd {
namespace rpc {

// TODO: This is redundant with the `get_or_throw` functions. Remove one set.
//
// Construct a T from the specified json field. Throw if the key is not present
template <class T>
T
fromJson(Json::Value const& jv, char const* key)
{
    static_assert(sizeof(T) == 0, "Must specialize this function");
}

template <>
inline boost::asio::ip::address
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing an ip address");

    return boost::asio::ip::make_address(v.asString());
}

template <>
inline std::uint16_t
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing an uint16");

    auto const uInt = v.asUInt();
    if (uInt > std::numeric_limits<std::uint16_t>::max())
    {
        throw std::runtime_error(
            "json key: "s + key + " is too large for an uint16");
    }

    return static_cast<std::uint16_t>(uInt);
}

template <>
inline std::uint32_t
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing an uint32");

    auto const uInt = v.asUInt();
    if (uInt > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error(
            "json key: "s + key + " is too large for an uint32");
    }

    return static_cast<std::uint32_t>(uInt);
}

template <>
inline std::uint64_t
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing an uint64");

    if (v.isString())
    {
        // parse as hex
        auto const s = v.asString();
        std::uint64_t val;

        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), val, 16);

        if (ec != std::errc() || (p != s.data() + s.size()))
            throw std::runtime_error(
                "json key: "s + key + " can not be parsed as a uint64");
        return val;
    }
    auto const uInt = v.asUInt();

    return static_cast<std::uint64_t>(uInt);
}

template <>
inline std::string
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a string");

    return v.asString();
}

template <>
inline ChainType
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a ChainType");

    auto const s = v.asString();
    if (s == to_string(ChainType::locking))
        return ChainType::locking;
    if (s == to_string(ChainType::issuing))
        return ChainType::issuing;
    throw std::runtime_error(
        "Expected locking or issuing as a ChainType's value");
}

template <>
inline boost::filesystem::path
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a path");

    return boost::filesystem::path{v.asString()};
}

template <>
inline beast::IP::Endpoint
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key +
            " while constructing an IP::Endpoint");

    return beast::IP::Endpoint{
        fromJson<boost::asio::ip::address>(v, "IP"),
        fromJson<std::uint16_t>(v, "Port")};
}

template <>
inline ripple::AccountID
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing an AccountID");

    auto const optID = ripple::parseBase58<ripple::AccountID>(v.asString());
    if (!optID)
        throw std::runtime_error(
            "Invalid account id: "s + v.asString() +
            " while constructing an AccountID");

    return *optID;
}

template <>
inline ripple::Seed
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a secret key");

    auto const optS = ripple::parseBase58<ripple::Seed>(v.asString());
    if (!optS)
        // do not add the key to the error message
        throw std::runtime_error("Invalid base58 seed");

    return *optS;
}

template <>
inline ripple::uint256
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a hash");

    ripple::uint256 h;
    if (!h.parseHex(v.asString()))
        throw std::runtime_error("Invalid hash hex");

    return h;
}

template <>
inline ripple::STXChainBridge
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a sidechain");
    return ripple::STXChainBridge(v);
}

template <>
inline ripple::STAmount
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a sidechain");
    // TODO: Should this be sfAmount or sfGeneric?
    return ripple::amountFromJson(ripple::sfGeneric, v);
}

struct AddrEndpoint
{
    std::string host;
    uint16_t port;
};

template <>
inline AddrEndpoint
fromJson(Json::Value const& jv, char const* key)
{
    using namespace std::literals;
    auto const v = jv[key];
    if (v.isNull())
        throw std::runtime_error(
            "Expected json key: "s + key + " while constructing a secret key");

    auto host = v["Host"].asString();
    auto port = v["Port"].asUInt();

    return {std::move(host), static_cast<uint16_t>(port)};
}

template <class T>
std::optional<T>
optFromJson(Json::Value const& jv, char const* key)
{
    try
    {
        return fromJson<T>(jv, key);
    }
    catch (...)
    {
        return {};
    }
}

}  // namespace rpc
}  // namespace xbwd
