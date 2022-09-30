#pragma once
//------------------------------------------------------------------------------
/*
    THIS FILE IS PART OF RIPPLED: HTTPS://GITHUB.COM/RIPPLE/RIPPLED
    COPYRIGHT (C) 2021 RIPPLE LABS INC.

    PERMISSION TO USE, COPY, MODIFY, AND/OR DISTRIBUTE THIS SOFTWARE FOR ANY
    PURPOSE  WITH  OR WITHOUT FEE IS HEREBY GRANTED, PROVIDED THAT THE ABOVE
    COPYRIGHT NOTICE AND THIS PERMISSION NOTICE APPEAR IN ALL COPIES.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STParsedJSON.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/STXChainAttestationBatch.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/jss.h>

#include <string>

namespace xbwd {
namespace txn {

[[nodiscard]] inline Json::Value
getTxn(
    ripple::AccountID const& acc,
    ripple::STXChainAttestationBatch const& batch,
    std::uint32_t seq,
    std::uint32_t lastLedgerSeq,
    ripple::XRPAmount const& fee)
{
    using namespace ripple;

    Json::Value txnJson;
    txnJson[jss::TransactionType] = jss::XChainAddAttestation;
    txnJson[jss::Account] = toBase58(acc);
    txnJson[sfXChainAttestationBatch.getJsonName()] =
        batch.getJson(JsonOptions::none);
    txnJson[jss::Sequence] = seq;
    txnJson[jss::Fee] = to_string(fee);
    txnJson[jss::LastLedgerSequence] = lastLedgerSeq;
    return txnJson;
};

[[nodiscard]] inline ripple::STTx
getSignedTxn(
    ripple::AccountID const& acc,
    ripple::STXChainAttestationBatch const& batch,
    std::uint32_t seq,
    std::uint32_t lastLedgerSeq,
    ripple::XRPAmount const& fee,
    ripple::PublicKey const& pk,
    ripple::SecretKey const& sk,
    beast::Journal j)
{
    using namespace ripple;

    auto const txnJson = getTxn(acc, batch, seq, lastLedgerSeq, fee);

    try
    {
        STParsedJSONObject parsed(std::string(jss::tx_json), txnJson);
        if (parsed.object == std::nullopt)
        {
            throw std::runtime_error("invalid transaction while signing");
        }
        parsed.object->setFieldVL(sfSigningPubKey, pk.slice());
        STTx txn(std::move(parsed.object.value()));
        txn.sign(pk, sk);
        return txn;
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j.fatal(),
            "exception while signing transation",
            jv("txn", txnJson),
            jv("what", e.what()));
        throw;
    }
};

}  // namespace txn

}  // namespace xbwd
