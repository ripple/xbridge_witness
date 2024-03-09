//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2023 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xbwd/app/DBInit.h>
#include <xbwd/core/DatabaseCon.h>
#include <xbwd/core/SociDB.h>

#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/XChainAttestations.h>

#include <fmt/format.h>

#include <filesystem>

namespace xbwd {
namespace tests {

class DB_test : public beast::unit_test::suite
{
    ripple::Logs logs_;
    beast::Journal j_;

public:
    DB_test()
        : logs_(beast::severities::kInfo), j_([&, this]() {
            logs_.silent(true);
            return logs_.journal("Tests");
        }())
    {
    }

private:
    void
    deleteDB()
    {
        std::error_code ec;
        std::filesystem::remove(db_init::xChainDBName(), ec);
    }

    std::unique_ptr<DatabaseCon>
    createDB()
    {
        Json::Value jv;

        try
        {
            return std::make_unique<DatabaseCon>(
                ".",
                db_init::xChainDBName(),
                db_init::xChainDBPragma(),
                db_init::xChainDBInit(),
                j_);
        }
        catch (std::exception&)
        {
            return std::unique_ptr<DatabaseCon>();
        }
    }

    void
    testCreateDB()
    {
        testcase("Create db");

        auto db = createDB();
        BEAST_EXPECT(db);
        db.reset();

        testcase("Open db");
        db = createDB();
        BEAST_EXPECT(db);
        db.reset();

        deleteDB();
    }

    void
    testBadDB()
    {
        testcase("Open bad db");

        std::ofstream f(db_init::xChainDBName());
        f << "blah-blah" << std::endl;
        f.close();

        auto db = createDB();
        BEAST_EXPECT(!db);

        deleteDB();
    }

    void
    testInitDB()
    {
        testcase("Init db");

        auto db = createDB();
        if (!db)
            throw std::runtime_error("Can't create db");

        ripple::uint256 const hash;
        auto const hashHex = ripple::strHex(hash.begin(), hash.end());
        unsigned const seq = 0;

        auto initSyncTable = [&]() {
            for (auto const ct : {ChainType::locking, ChainType::issuing})
            {
                auto session = db->checkoutDb();
                auto const sql = fmt::format(
                    "INSERT INTO {} (ChainType, TransID, LedgerSeq) VALUES "
                    "(:ct, :txnId, :lgrSeq);",
                    db_init::xChainSyncTable);
                *session << sql, soci::use(static_cast<std::uint32_t>(ct)),
                    soci::use(hashHex), soci::use(seq);
            }
        };

        auto readSyncTable = [&]() -> bool {
            auto session = db->checkoutDb();
            auto const sql = fmt::format(
                "SELECT ChainType, TransID, LedgerSeq FROM {};",
                db_init::xChainSyncTable);

            std::uint32_t chainType = 0;
            std::string transID;
            std::uint32_t ledgerSeq = 0;
            int rows = 0;
            soci::statement st =
                ((*session).prepare << sql,
                 soci::into(chainType),
                 soci::into(transID),
                 soci::into(ledgerSeq));
            st.execute();
            while (st.fetch())
            {
                if (chainType !=
                        static_cast<std::uint32_t>(ChainType::issuing) &&
                    chainType != static_cast<std::uint32_t>(ChainType::locking))
                {
                    throw std::runtime_error("unknown chain type");
                }
                auto const ct = static_cast<ChainType>(chainType);

                ripple::uint256 locHash;
                if (!locHash.parseHex(transID))
                {
                    throw std::runtime_error("cannot parse transation hash");
                }

                if (!BEAST_EXPECT(seq == ledgerSeq))
                    return false;
                if (!BEAST_EXPECT(hash == locHash))
                    return false;
                ++rows;
            }
            return BEAST_EXPECT(rows == 2);
        };

        initSyncTable();
        readSyncTable();

        deleteDB();
    }

    void
    testSubmitTable()
    {
        testcase("Submit table");

        auto db = createDB();
        if (!db)
            throw std::runtime_error("Can't create db");

        for (auto const ct : {ChainType::locking, ChainType::issuing})
        {
            auto const& tblName = db_init::xChainTableName(ct);

            int const success = 1;
            ripple::AccountID const rewAcc;
            ripple::AccountID const dst, src;
            ripple::AccountID const signAcc;
            ripple::STXChainBridge const bridge;
            auto const keys = ripple::generateKeyPair(
                ripple::KeyType::ed25519,
                *ripple::parseBase58<ripple::Seed>(
                    "snnksgXkSTgCBuHJmHeTekJyj4qG6"));

            ripple::PublicKey const signPub = keys.first;
            ripple::SecretKey const signSec = keys.second;
            ripple::STAmount amt;
            std::uint64_t const claimID = 0;
            ripple::uint256 const hash;
            auto const hashHex = ripple::strHex(hash.begin(), hash.end());
            unsigned const seq = 0;

            {
                auto claim = ripple::Attestations::AttestationClaim{
                    bridge,
                    signAcc,
                    signPub,
                    signSec,
                    src,
                    amt,
                    rewAcc,
                    ct == ChainType::locking,
                    claimID,
                    dst};

                auto session = db->checkoutDb();

                soci::blob amtBlob = convert(amt, *session);
                soci::blob bridgeBlob = convert(bridge, *session);
                soci::blob sendingAccountBlob = convert(src, *session);
                soci::blob rewardAccountBlob = convert(rewAcc, *session);
                soci::blob signingAccountBlob =
                    convert(claim.attestationSignerAccount, *session);
                soci::blob publicKeyBlob = convert(signPub, *session);
                soci::blob signatureBlob = convert(claim.signature, *session);
                soci::blob otherChainDstBlob = convert(dst, *session);

                auto const sql = fmt::format(
                    "INSERT INTO {} (TransID, LedgerSeq, ClaimID, Success, "
                    "DeliveredAmt, Bridge, SendingAccount, RewardAccount, "
                    "OtherChainDst, SigningAccount, PublicKey, Signature) "
                    "VALUES (:txnId, :lgrSeq, :claimID, :success, :amt, "
                    ":bridge, :sendingAccount, :rewardAccount, :otherChainDst, "
                    ":signingAccount, :pk, :sig); ",
                    tblName);

                *session << sql, soci::use(hashHex), soci::use(seq),
                    soci::use(claimID), soci::use(success), soci::use(amtBlob),
                    soci::use(bridgeBlob), soci::use(sendingAccountBlob),
                    soci::use(rewardAccountBlob), soci::use(otherChainDstBlob),
                    soci::use(signingAccountBlob), soci::use(publicKeyBlob),
                    soci::use(signatureBlob);
            }

            {
                auto session = db->checkoutDb();

                soci::blob amtBlob(*session);
                soci::blob bridgeBlob(*session);
                soci::blob sendingAccountBlob(*session);
                soci::blob rewardAccountBlob(*session);
                soci::blob otherChainDstBlob(*session);
                soci::blob signingAccountBlob(*session);
                soci::blob publicKeyBlob(*session);
                soci::blob signatureBlob(*session);

                std::string transID;
                int ledgerSeq;
                int claimID;
                int success;

                int rows = 0;

                auto const sql = fmt::format(
                    "SELECT TransID, LedgerSeq, ClaimID, Success, "
                    "DeliveredAmt, Bridge, SendingAccount, RewardAccount, "
                    "OtherChainDst, SigningAccount, PublicKey, Signature "
                    "FROM {} "
                    "ORDER BY ClaimID;",
                    tblName);

                soci::indicator otherChainDstInd;
                soci::statement st =
                    ((*session).prepare << sql,
                     soci::into(transID),
                     soci::into(ledgerSeq),
                     soci::into(claimID),
                     soci::into(success),
                     soci::into(amtBlob),
                     soci::into(bridgeBlob),
                     soci::into(sendingAccountBlob),
                     soci::into(rewardAccountBlob),
                     soci::into(otherChainDstBlob, otherChainDstInd),
                     soci::into(signingAccountBlob),
                     soci::into(publicKeyBlob),
                     soci::into(signatureBlob));
                st.execute();

                while (st.fetch())
                {
                    auto locSignAcc =
                        convert<ripple::AccountID>(signingAccountBlob);
                    auto locSignPub = convert<ripple::PublicKey>(publicKeyBlob);
                    auto sigBuf = convert<ripple::Buffer>(signatureBlob);
                    auto locAmt = convert<ripple::STAmount>(amtBlob);
                    auto locSrc =
                        convert<ripple::AccountID>(sendingAccountBlob);
                    auto locRewAcc =
                        convert<ripple::AccountID>(rewardAccountBlob);
                    auto locDst = convert<ripple::AccountID>(otherChainDstBlob);
                    BEAST_EXPECT(otherChainDstInd == soci::i_ok);
                    auto locBridge =
                        convert<ripple::STXChainBridge>(bridgeBlob);

                    if (!BEAST_EXPECT(signAcc == locSignAcc))
                        return;
                    if (!BEAST_EXPECT(signPub == locSignPub))
                        return;
                    if (!BEAST_EXPECT(amt == locAmt))
                        return;
                    if (!BEAST_EXPECT(src == locSrc))
                        return;
                    if (!BEAST_EXPECT(dst == locDst))
                        return;
                    if (!BEAST_EXPECT(rewAcc == locRewAcc))
                        return;
                    if (!BEAST_EXPECT(bridge == locBridge))
                        return;

                    ++rows;
                }
                BEAST_EXPECT(1 == rows);
            }
        }

        deleteDB();
    }

    void
    testCreateTable()
    {
        testcase("Create table");

        auto db = createDB();
        if (!db)
            throw std::runtime_error("Can't create db");

        for (auto const ct : {ChainType::locking, ChainType::issuing})
        {
            auto const& tblName = db_init::xChainCreateAccountTableName(ct);

            int const success = 1;
            ripple::AccountID const rewAcc;
            ripple::AccountID const dst, src;
            ripple::AccountID const signAcc;
            ripple::STXChainBridge const bridge;
            auto const keys = ripple::generateKeyPair(
                ripple::KeyType::ed25519,
                *ripple::parseBase58<ripple::Seed>(
                    "snnksgXkSTgCBuHJmHeTekJyj4qG6"));

            ripple::PublicKey const signPub = keys.first;
            ripple::SecretKey const signSec = keys.second;
            ripple::STAmount amt, rewAmt;
            std::uint64_t const createCnt = 0;
            ripple::uint256 const hash;
            auto const hashHex = ripple::strHex(hash.begin(), hash.end());
            unsigned const seq = 0;

            {
                auto create = ripple::Attestations::AttestationCreateAccount{
                    bridge,
                    signAcc,
                    signPub,
                    signSec,
                    src,
                    amt,
                    rewAmt,
                    rewAcc,
                    ct == ChainType::locking,
                    createCnt,
                    dst};

                auto session = db->checkoutDb();

                soci::blob amtBlob = convert(amt, *session);
                soci::blob rewardAmtBlob = convert(rewAmt, *session);
                soci::blob bridgeBlob = convert(bridge, *session);
                soci::blob sendingAccountBlob = convert(src, *session);
                soci::blob rewardAccountBlob = convert(rewAcc, *session);
                soci::blob signingAccountBlob =
                    convert(create.attestationSignerAccount, *session);
                soci::blob publicKeyBlob = convert(signPub, *session);
                soci::blob signatureBlob = convert(create.signature, *session);
                soci::blob otherChainDstBlob = convert(dst, *session);

                auto const sql = fmt::format(
                    "INSERT INTO {} "
                    "(TransID, LedgerSeq, CreateCount, Success, DeliveredAmt, "
                    "RewardAmt, Bridge, "
                    "SendingAccount, RewardAccount, otherChainDst, "
                    "SigningAccount, PublicKey, Signature) "
                    "VALUES "
                    "(:txnId, :lgrSeq, :createCount, :success, :amt, "
                    ":rewardAmt, :bridge, "
                    ":sendingAccount, :rewardAccount, :otherChainDst, "
                    ":signingAccount, :pk, :sig);",
                    tblName);

                *session << sql, soci::use(hashHex), soci::use(seq),
                    soci::use(createCnt), soci::use(success),
                    soci::use(amtBlob), soci::use(rewardAmtBlob),
                    soci::use(bridgeBlob), soci::use(sendingAccountBlob),
                    soci::use(rewardAccountBlob), soci::use(otherChainDstBlob),
                    soci::use(signingAccountBlob), soci::use(publicKeyBlob),
                    soci::use(signatureBlob);
            }

            {
                auto session = db->checkoutDb();

                soci::blob amtBlob(*session);
                soci::blob rewardAmtBlob(*session);
                soci::blob bridgeBlob(*session);
                soci::blob sendingAccountBlob(*session);
                soci::blob rewardAccountBlob(*session);
                soci::blob otherChainDstBlob(*session);
                soci::blob signingAccountBlob(*session);
                soci::blob publicKeyBlob(*session);
                soci::blob signatureBlob(*session);

                std::string transID;
                int ledgerSeq = 0;
                int createCount = 0;
                int success = 0;

                int rows = 0;

                auto const sql = fmt::format(
                    "SELECT TransID, LedgerSeq, CreateCount, Success, "
                    "DeliveredAmt, RewardAmt, "
                    "Bridge, SendingAccount, RewardAccount, OtherChainDst, "
                    "SigningAccount, PublicKey, Signature "
                    "FROM {} "
                    "ORDER BY CreateCount;",
                    tblName);

                soci::indicator otherChainDstInd;
                soci::statement st =
                    ((*session).prepare << sql,
                     soci::into(transID),
                     soci::into(ledgerSeq),
                     soci::into(createCount),
                     soci::into(success),
                     soci::into(amtBlob),
                     soci::into(rewardAmtBlob),
                     soci::into(bridgeBlob),
                     soci::into(sendingAccountBlob),
                     soci::into(rewardAccountBlob),
                     soci::into(otherChainDstBlob, otherChainDstInd),
                     soci::into(signingAccountBlob),
                     soci::into(publicKeyBlob),
                     soci::into(signatureBlob));
                st.execute();

                while (st.fetch())
                {
                    auto locSignAcc =
                        convert<ripple::AccountID>(signingAccountBlob);
                    auto locSignPub = convert<ripple::PublicKey>(publicKeyBlob);
                    auto sigBuf = convert<ripple::Buffer>(signatureBlob);
                    auto locAmt = convert<ripple::STAmount>(amtBlob);
                    auto locRewAmt = convert<ripple::STAmount>(rewardAmtBlob);
                    auto locSrc =
                        convert<ripple::AccountID>(sendingAccountBlob);
                    auto locRewAcc =
                        convert<ripple::AccountID>(rewardAccountBlob);
                    auto locDst = convert<ripple::AccountID>(otherChainDstBlob);
                    BEAST_EXPECT(otherChainDstInd == soci::i_ok);
                    auto locBridge =
                        convert<ripple::STXChainBridge>(bridgeBlob);

                    if (!BEAST_EXPECT(signAcc == locSignAcc))
                        return;
                    if (!BEAST_EXPECT(signPub == locSignPub))
                        return;
                    if (!BEAST_EXPECT(amt == locAmt))
                        return;
                    if (!BEAST_EXPECT(rewAmt == locRewAmt))
                        return;
                    if (!BEAST_EXPECT(src == locSrc))
                        return;
                    if (!BEAST_EXPECT(dst == locDst))
                        return;
                    if (!BEAST_EXPECT(rewAcc == locRewAcc))
                        return;
                    if (!BEAST_EXPECT(bridge == locBridge))
                        return;

                    ++rows;
                }
                BEAST_EXPECT(1 == rows);
            }
        }

        deleteDB();
    }

public:
    void
    run() override
    {
        deleteDB();
        testCreateDB();
        testBadDB();
        testInitDB();
        testSubmitTable();
        testCreateTable();
        deleteDB();
    }
};

BEAST_DEFINE_TESTSUITE(DB, app, xbwd);

}  // namespace tests

}  // namespace xbwd
