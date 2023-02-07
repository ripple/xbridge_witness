#include "JsonChecker.h"

#include <ripple/basics/Log.h>
#include <ripple/json/Output.h>
#include <ripple/json/json_forwards.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/TxFormats.h>
#include <ripple/protocol/jss.h>

#include <fmt/format.h>

namespace xbwd {

JsonChecker::JsonChecker(beast::Journal j)
    : additionalKeys_{std::string(ripple::jss::date), std::string(ripple::jss::hash)}
    , j_(j)
{
}

bool
checkXBridge(Json::Value const& jv, beast::Journal j)
{
    static auto const jbridge =
        ripple::STXChainBridge().getJson(ripple::JsonOptions::none);
    for (auto it = jv.begin(); it != jv.end(); ++it)
    {
        std::string const name = it.memberName();
        if (!jbridge.isMember(name))
        {
            JLOGV(
                j.warn(),
                "Extra fields checker",
                ripple::jv("invalid_field", name));
            throw std::runtime_error(fmt::format("Invalid field: {}", name));
        }
    }
    return true;
}

bool
checkSignerEntries(Json::Value const& jv, beast::Journal j)
{
    static std::unordered_set<std::string> const signerEntryKeys = {
        ripple::sfAccount.getJsonName().c_str(),
        ripple::sfSignerWeight.getJsonName().c_str(),
        ripple::sfWalletLocator.getJsonName().c_str()};

    if (!jv.isArray())
    {
        JLOGV(
            j.warn(),
            "Extra fields checker",
            ripple::jv(
                ripple::sfSignerEntries.getJsonName().c_str(),
                "is not an array"));
        throw std::runtime_error(fmt::format(
            "{} is not an array",
            ripple::sfSignerEntries.getJsonName().c_str()));
    }

    static std::string seName = ripple::sfSignerEntry.getJsonName().c_str();
    for (auto const& superEntry : jv)
    {
        for (auto it = superEntry.begin(); it != superEntry.end(); ++it)
        {
            std::string const name = it.memberName();
            if (name != seName)
            {
                JLOGV(
                    j.warn(),
                    "Extra fields checker",
                    ripple::jv("invalid_field", name));
                throw std::runtime_error(
                    fmt::format("Invalid field: {}", name));
            }
        }

        auto const& entry = superEntry[seName];
        for (auto it = entry.begin(); it != entry.end(); ++it)
        {
            std::string const name = it.memberName();
            if (!signerEntryKeys.contains(name))
            {
                JLOGV(
                    j.warn(),
                    "Extra fields checker",
                    ripple::jv("invalid_field", name));
                throw std::runtime_error(
                    fmt::format("Invalid field: {}", name));
            }
        }
    }

    return true;
}

bool
JsonChecker::check(Json::Value const& jdata) const
{
    bool ret = true;

    static ripple::TxFormats const& txFormats(ripple::TxFormats::getInstance());

    std::string const txType = jdata[ripple::jss::TransactionType].asString();
    auto const& txi(*txFormats.findByType(txFormats.findTypeByName(txType)));
    auto const& sot(txi.getSOTemplate());

    for (auto it = jdata.begin(); it != jdata.end(); ++it)
    {
        std::string const name = it.memberName();
        if (additionalKeys_.contains(name))
            continue;
        auto const& sf = ripple::SField::getField(name);
        int const idx = sot.getIndex(sf);

        if (idx < 0)
        {
            JLOGV(
                j_.warn(),
                "Extra fields checker",
                ripple::jv("invalid_field", name));
            throw std::runtime_error(fmt::format("Invalid field: {}", name));
        }

        if (ripple::jss::XChainBridge == name)
            checkXBridge(*it, j_);
        else if (ripple::sfSignerEntries.getJsonName() == name)
            checkSignerEntries(*it, j_);
    }

    return ret;
}

}  // namespace xbwd
