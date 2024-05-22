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

#include <xbwd/client/RpcResultParse.h>

#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>

#include <filesystem>
#include <format>

namespace xbwd {
namespace tests {

extern const char TxAccCreate[];
extern const char TxCommit[];
extern const char TxAccCreateAtt[];
extern const char TxAccClaimAtt[];
extern const char TxCreateBridge[];

class RPCParse_test : public beast::unit_test::suite
{
public:
    RPCParse_test() = default;

private:
    void
    testToString()
    {
        testcase("Test enum to string conversion");

        BEAST_EXPECT(to_string(XChainTxnType::xChainCommit) == "xChainCommit");
        BEAST_EXPECT(to_string(XChainTxnType::xChainClaim) == "xChainClaim");
        BEAST_EXPECT(
            to_string(XChainTxnType::xChainAccountCreateCommit) ==
            "xChainAccountCreateCommit");
        BEAST_EXPECT(
            to_string(XChainTxnType::xChainAddAccountCreateAttestation) ==
            "xChainAddAccountCreateAttestation");
        BEAST_EXPECT(
            to_string(XChainTxnType::xChainAddClaimAttestation) ==
            "xChainAddClaimAttestation");
        BEAST_EXPECT(
            to_string(XChainTxnType::xChainCreateBridge) ==
            "xChainCreateBridge");
        BEAST_EXPECT(
            to_string(XChainTxnType::SignerListSet) == "SignerListSet");
        BEAST_EXPECT(to_string(XChainTxnType::AccountSet) == "AccountSet");
        BEAST_EXPECT(
            to_string(XChainTxnType::SetRegularKey) == "SetRegularKey");
    }

    void
    testMatchStr()
    {
        testcase("Test matching string");
        using namespace rpcResultParse;

        Json::Value j;
        j["testStr"] = "testVal";
        BEAST_EXPECT(fieldMatchesStr(j, "testStr", "testVal"));
        BEAST_EXPECT(!fieldMatchesStr(j, "test", "testVal"));
    }

    void
    testParse()
    {
        testcase("Test parser");
        using namespace rpcResultParse;

        Json::Value jvCreate;
        Json::Value jvCommit;
        Json::Value jvCreateAtt;
        Json::Value jvCommitAtt;
        Json::Value jvCreateBridge;

        Json::Value jvEmpty;

        Json::Reader().parse(TxAccCreate, jvCreate);
        auto const& txCreate = jvCreate["transaction"];
        auto const& metaCreate = jvCreate["meta"];

        Json::Reader().parse(TxCommit, jvCommit);
        auto const& txCommit = jvCommit["transaction"];
        auto const& metaCommit = jvCreate["meta"];

        Json::Reader().parse(TxAccCreateAtt, jvCreateAtt);
        auto const& txCreateAtt = jvCreateAtt["transaction"];
        auto const& metaCreateAtt = jvCreate["meta"];

        Json::Reader().parse(TxAccClaimAtt, jvCommitAtt);
        auto const& txCommitAtt = jvCommitAtt["transaction"];
        auto const& metaCommitAtt = jvCreate["meta"];

        Json::Reader().parse(TxCreateBridge, jvCreateBridge);
        auto const& txCreateBridge = jvCreateBridge["transaction"];
        auto const& metaCreateBridge = jvCreate["meta"];

        auto createCount = parseCreateCount(metaCreate);
        BEAST_EXPECT(createCount == 1);
        createCount = parseCreateCount(jvEmpty);
        BEAST_EXPECT(!createCount);

        auto rewAmt = parseRewardAmt(txCreate);
        BEAST_EXPECT(
            rewAmt ==
            ripple::STAmount(ripple::sfGeneric, static_cast<int64_t>(100)));
        rewAmt = parseRewardAmt(txCommit);
        BEAST_EXPECT(!rewAmt);
        rewAmt = parseRewardAmt(txCreateAtt);
        BEAST_EXPECT(
            rewAmt ==
            ripple::STAmount(ripple::sfGeneric, static_cast<int64_t>(100)));
        rewAmt = parseRewardAmt(txCommitAtt);
        BEAST_EXPECT(!rewAmt);
        rewAmt = parseRewardAmt(txCreateBridge);
        BEAST_EXPECT(
            rewAmt ==
            ripple::STAmount(ripple::sfGeneric, static_cast<int64_t>(100)));
        rewAmt = parseRewardAmt(jvEmpty);
        BEAST_EXPECT(!rewAmt);

        auto txType = parseXChainTxnType(txCreate);
        BEAST_EXPECT(txType == XChainTxnType::xChainAccountCreateCommit);
        txType = parseXChainTxnType(txCommit);
        BEAST_EXPECT(txType == XChainTxnType::xChainCommit);
        txType = parseXChainTxnType(txCreateAtt);
        BEAST_EXPECT(
            txType == XChainTxnType::xChainAddAccountCreateAttestation);
        txType = parseXChainTxnType(txCommitAtt);
        BEAST_EXPECT(txType == XChainTxnType::xChainAddClaimAttestation);
        txType = parseXChainTxnType(txCreateBridge);
        BEAST_EXPECT(txType == XChainTxnType::xChainCreateBridge);
        txType = parseXChainTxnType(jvEmpty);
        BEAST_EXPECT(!txType);

        auto src = parseSrcAccount(txCreate);
        BEAST_EXPECT(
            src &&
            ripple::toBase58(*src) == "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV");
        src = parseSrcAccount(txCommit);
        BEAST_EXPECT(
            src &&
            ripple::toBase58(*src) == "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV");
        src = parseSrcAccount(txCreateAtt);
        BEAST_EXPECT(
            src &&
            ripple::toBase58(*src) == "rGrQ8QEAtiKvjXwFxZxv6pxmzWfDcqWeUV");
        src = parseSrcAccount(txCommitAtt);
        BEAST_EXPECT(
            src &&
            ripple::toBase58(*src) == "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg");
        src = parseSrcAccount(txCreateBridge);
        BEAST_EXPECT(
            src &&
            ripple::toBase58(*src) == "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D");
        src = parseSrcAccount(jvEmpty);
        BEAST_EXPECT(!src);

        auto dst =
            parseDstAccount(txCreate, XChainTxnType::xChainAccountCreateCommit);
        BEAST_EXPECT(
            dst &&
            ripple::toBase58(*dst) == "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7");
        dst = parseDstAccount(txCommit, XChainTxnType::xChainCommit);
        BEAST_EXPECT(
            dst &&
            ripple::toBase58(*dst) == "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7");
        dst = parseDstAccount(
            txCreateAtt, XChainTxnType::xChainAddAccountCreateAttestation);
        BEAST_EXPECT(!dst);
        dst = parseDstAccount(
            txCommitAtt, XChainTxnType::xChainAddClaimAttestation);
        BEAST_EXPECT(!dst);
        dst =
            parseDstAccount(txCreateBridge, XChainTxnType::xChainCreateBridge);
        BEAST_EXPECT(!dst);
        dst =
            parseDstAccount(jvEmpty, XChainTxnType::xChainAccountCreateCommit);
        BEAST_EXPECT(!dst);

        auto osrc = parseOtherSrcAccount(
            txCreate, XChainTxnType::xChainAccountCreateCommit);
        BEAST_EXPECT(
            osrc &&
            ripple::toBase58(*osrc) == "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV");
        osrc = parseOtherSrcAccount(txCommit, XChainTxnType::xChainCommit);
        BEAST_EXPECT(
            osrc &&
            ripple::toBase58(*osrc) == "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV");
        osrc = parseOtherSrcAccount(
            txCreateAtt, XChainTxnType::xChainAddAccountCreateAttestation);
        BEAST_EXPECT(
            osrc &&
            ripple::toBase58(*osrc) == "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV");
        osrc = parseOtherSrcAccount(
            txCommitAtt, XChainTxnType::xChainAddClaimAttestation);
        BEAST_EXPECT(
            osrc &&
            ripple::toBase58(*osrc) == "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV");
        osrc = parseOtherSrcAccount(
            txCreateBridge, XChainTxnType::xChainCreateBridge);
        BEAST_EXPECT(!osrc);
        osrc = parseOtherSrcAccount(
            jvEmpty, XChainTxnType::xChainAccountCreateCommit);
        BEAST_EXPECT(!osrc);

        auto odst = parseOtherDstAccount(
            txCreate, XChainTxnType::xChainAccountCreateCommit);
        BEAST_EXPECT(
            odst &&
            ripple::toBase58(*odst) == "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7");
        odst = parseOtherDstAccount(txCommit, XChainTxnType::xChainCommit);
        BEAST_EXPECT(
            odst &&
            ripple::toBase58(*odst) == "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7");
        odst = parseOtherDstAccount(
            txCreateAtt, XChainTxnType::xChainAddAccountCreateAttestation);
        BEAST_EXPECT(
            odst &&
            ripple::toBase58(*odst) == "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7");
        odst = parseOtherDstAccount(
            txCommitAtt, XChainTxnType::xChainAddClaimAttestation);
        BEAST_EXPECT(
            odst &&
            ripple::toBase58(*odst) == "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7");
        odst = parseOtherDstAccount(
            txCreateBridge, XChainTxnType::xChainCreateBridge);
        BEAST_EXPECT(!odst);
        odst = parseOtherDstAccount(
            jvEmpty, XChainTxnType::xChainAccountCreateCommit);
        BEAST_EXPECT(!odst);

        ripple::STXChainBridge const testBridge(
            *ripple::parseBase58<ripple::AccountID>(
                "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D"),
            ripple::xrpIssue(),
            *ripple::parseBase58<ripple::AccountID>(
                "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"),
            ripple::xrpIssue());
        auto bridge = parseBridge(txCreate);
        BEAST_EXPECT(bridge && bridge == testBridge);
        bridge = parseBridge(txCommit);
        BEAST_EXPECT(bridge && bridge == testBridge);
        bridge = parseBridge(txCreateAtt);
        BEAST_EXPECT(bridge && bridge == testBridge);
        bridge = parseBridge(txCommitAtt);
        BEAST_EXPECT(bridge && bridge == testBridge);
        bridge = parseBridge(txCreateBridge);
        BEAST_EXPECT(bridge && bridge == testBridge);
        bridge = parseBridge(jvEmpty);
        BEAST_EXPECT(!bridge);

        auto hash = parseTxHash(txCreate);
        BEAST_EXPECT(
            hash &&
            to_string(*hash) ==
                "A6F65C3C92BD077249C2320FEFC2214B39FDA8DAEF92715EBCF7CA4EC5364E"
                "E2");
        hash = parseTxHash(txCommit);
        BEAST_EXPECT(
            hash &&
            to_string(*hash) ==
                "926D50565D691C072C4A25440E1A58DC6F1D79A7DC7D059490811647614828"
                "24");
        hash = parseTxHash(txCreateAtt);
        BEAST_EXPECT(
            hash &&
            to_string(*hash) ==
                "3623DCB1C5D7F6FB31B5A089614B3C21EC98FC4E009E90E56FF0AFFB58AD3B"
                "69");
        hash = parseTxHash(txCommitAtt);
        BEAST_EXPECT(
            hash &&
            to_string(*hash) ==
                "5F6D4D62A20686EE6FA2AE9D3CE70924D0EB03B80143B8671F30049B99EC1C"
                "69");
        hash = parseTxHash(txCreateBridge);
        BEAST_EXPECT(
            hash &&
            to_string(*hash) ==
                "C7C94CB45DEE44AF0DA90825EE3552C3B50E51109C4E42921E4F5893ECE123"
                "18");
        hash = parseTxHash(jvEmpty);
        BEAST_EXPECT(!hash);

        auto seq = parseTxSeq(txCreate);
        BEAST_EXPECT(seq && seq == 6);
        seq = parseTxSeq(txCommit);
        BEAST_EXPECT(seq && seq == 8);
        seq = parseTxSeq(txCreateAtt);
        BEAST_EXPECT(seq && seq == 3);
        seq = parseTxSeq(txCommitAtt);
        BEAST_EXPECT(seq && seq == 4);
        seq = parseTxSeq(txCreateBridge);
        BEAST_EXPECT(seq && seq == 3);
        seq = parseTxSeq(jvEmpty);
        BEAST_EXPECT(!seq);

        auto ledgSeq = parseLedgerSeq(txCreate);
        BEAST_EXPECT(ledgSeq && ledgSeq == 7);
        ledgSeq = parseLedgerSeq(txCommit);
        BEAST_EXPECT(ledgSeq && ledgSeq == 9);
        ledgSeq = parseLedgerSeq(txCreateAtt);
        BEAST_EXPECT(ledgSeq && ledgSeq == 6);
        ledgSeq = parseLedgerSeq(txCommitAtt);
        BEAST_EXPECT(ledgSeq && ledgSeq == 8);
        ledgSeq = parseLedgerSeq(txCreateBridge);
        BEAST_EXPECT(ledgSeq && ledgSeq == 4);
        ledgSeq = parseLedgerSeq(jvEmpty);
        BEAST_EXPECT(!ledgSeq);

        auto deliveryAmt = parseDeliveredAmt(txCreate, metaCreate);
        BEAST_EXPECT(
            deliveryAmt &&
            deliveryAmt ==
                ripple::STAmount(
                    ripple::sfGeneric, static_cast<int64_t>(400000000)));
        deliveryAmt = parseDeliveredAmt(txCommit, metaCommit);
        BEAST_EXPECT(
            deliveryAmt &&
            deliveryAmt ==
                ripple::STAmount(
                    ripple::sfGeneric, static_cast<int64_t>(1000000)));
        deliveryAmt = parseDeliveredAmt(txCreateAtt, metaCreateAtt);
        BEAST_EXPECT(
            deliveryAmt &&
            deliveryAmt ==
                ripple::STAmount(
                    ripple::sfGeneric, static_cast<int64_t>(400000000)));
        deliveryAmt = parseDeliveredAmt(txCommitAtt, metaCommitAtt);
        BEAST_EXPECT(
            deliveryAmt &&
            deliveryAmt ==
                ripple::STAmount(
                    ripple::sfGeneric, static_cast<int64_t>(1000000)));
        deliveryAmt = parseDeliveredAmt(txCreateBridge, metaCreateBridge);
        BEAST_EXPECT(!deliveryAmt);
        deliveryAmt = parseDeliveredAmt(jvEmpty, jvEmpty);
        BEAST_EXPECT(!deliveryAmt);
    }

public:
    void
    run() override
    {
        testToString();
        testMatchStr();
        testParse();
    }
};

BEAST_DEFINE_TESTSUITE(RPCParse, client, xbwd);

const char TxAccCreate[] = R"str(
{
  "account_history_boundary": true,
  "account_history_tx_index": 0,
  "engine_result": "tesSUCCESS",
  "engine_result_code": 0,
  "ledger_index": 7,
  "meta": {
    "AffectedNodes": [
      {
        "ModifiedNode": {
          "FinalFields": {
            "Account": "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
            "Balance": "599999890",
            "Flags": 0,
            "OwnerCount": 0,
            "Sequence": 7
          },
          "LedgerEntryType": "AccountRoot",
          "LedgerIndex": "AC1F46A5DDA015695AD00328C2410C718B6EA7CBE9D1525D0D4618C9AA62AC83",
          "PreviousFields": {
            "Balance": "1000000000",
            "Sequence": 6
          },
          "PreviousTxnID": "73D46BE70E67C1FD407EC746091938ED1596CB7FE21D9A7A3F10D78739CB5AE2",
          "PreviousTxnLgrSeq": 6
        }
      },
      {
        "ModifiedNode": {
          "FinalFields": {
            "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
            "Flags": 0,
            "MinAccountCreateAmount": "10000000",
            "OwnerNode": "0",
            "SignatureReward": "100",
            "XChainAccountClaimCount": "0",
            "XChainAccountCreateCount": "1",
            "XChainBridge": {
              "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
              "IssuingChainIssue": {
                "currency": "XRP"
              },
              "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
              "LockingChainIssue": {
                "currency": "XRP"
              }
            },
            "XChainClaimID": "0"
          },
          "LedgerEntryType": "Bridge",
          "LedgerIndex": "DF794CFEDA27E06DAE87A6EFB489614C1D3FF857F3980661F84FC01933B4AF6F",
          "PreviousFields": {
            "XChainAccountCreateCount": "0"
          },
          "PreviousTxnID": "C7C94CB45DEE44AF0DA90825EE3552C3B50E51109C4E42921E4F5893ECE12318",
          "PreviousTxnLgrSeq": 4
        }
      },
      {
        "ModifiedNode": {
          "FinalFields": {
            "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
            "Balance": "520000070",
            "Flags": 1048576,
            "OwnerCount": 2,
            "Sequence": 6
          },
          "LedgerEntryType": "AccountRoot",
          "LedgerIndex": "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
          "PreviousFields": {
            "Balance": "119999970"
          },
          "PreviousTxnID": "3A435F6888202D15F907ECC46D0989CA8AE8A4063735AAE98EE928EDABBAD1F2",
          "PreviousTxnLgrSeq": 5
        }
      }
    ],
    "TransactionIndex": 0,
    "TransactionResult": "tesSUCCESS"
  },
  "transaction": {
    "Account": "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
    "Amount": "400000000",
    "Destination": "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7",
    "Fee": "10",
    "Flags": 0,
    "LastLedgerSequence": 26,
    "NetworkID": 15755,
    "Sequence": 6,
    "SignatureReward": "100",
    "SigningPubKey": "ED77ABF9CA5FB605455ECEC3821246528BD6E822EDFFDC445969DC51C7D036FDF2",
    "TransactionType": "XChainAccountCreateCommit",
    "TxnSignature": "B46BA906621F4A7E7512C278ABC58DE163A0FA235D547169446813A26712A47F5B17C8649FB696B712305B987E97B62BAB35799FF2DD96FF0428D9C37A61FE04",
    "XChainBridge": {
      "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
      "IssuingChainIssue": {
        "currency": "XRP"
      },
      "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
      "LockingChainIssue": {
        "currency": "XRP"
      }
    },
    "date": 752637130,
    "hash": "A6F65C3C92BD077249C2320FEFC2214B39FDA8DAEF92715EBCF7CA4EC5364EE2",
    "inLedger": 7,
    "ledger_index": 7
  },
  "type": "transaction",
  "validated": true
}
)str";

const char TxCommit[] = R"str(
{
   "account_history_boundary" : true,
   "account_history_tx_index" : 2,
   "engine_result" : "tesSUCCESS",
   "engine_result_code" : 0,
   "ledger_index" : 9,
   "meta" : {
      "AffectedNodes" : [
         {
            "ModifiedNode" : {
               "FinalFields" : {
                  "Account" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
                  "Balance" : "597999870",
                  "Flags" : 0,
                  "OwnerCount" : 0,
                  "Sequence" : 9
               },
               "LedgerEntryType" : "AccountRoot",
               "LedgerIndex" : "AC1F46A5DDA015695AD00328C2410C718B6EA7CBE9D1525D0D4618C9AA62AC83",
               "PreviousFields" : {
                  "Balance" : "598999880",
                  "Sequence" : 8
               },
               "PreviousTxnID" : "86CD453B4061FAEF10ED92B7DCDE01D41C0789A9F5A5507E281CC542E1BA693B",
               "PreviousTxnLgrSeq" : 8
            }
         },
         {
            "ModifiedNode" : {
               "FinalFields" : {
                  "Account" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                  "Balance" : "522000070",
                  "Flags" : 1048576,
                  "OwnerCount" : 2,
                  "Sequence" : 6
               },
               "LedgerEntryType" : "AccountRoot",
               "LedgerIndex" : "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
               "PreviousFields" : {
                  "Balance" : "521000070"
               },
               "PreviousTxnID" : "86CD453B4061FAEF10ED92B7DCDE01D41C0789A9F5A5507E281CC542E1BA693B",
               "PreviousTxnLgrSeq" : 8
            }
         }
      ],
      "TransactionIndex" : 0,
      "TransactionResult" : "tesSUCCESS"
   },
   "transaction" : {
      "Account" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
      "Amount" : "1000000",
      "Fee" : "10",
      "Flags" : 0,
      "LastLedgerSequence" : 28,
      "NetworkID" : 15755,
      "OtherChainDestination" : "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7",
      "Sequence" : 8,
      "SigningPubKey" : "ED77ABF9CA5FB605455ECEC3821246528BD6E822EDFFDC445969DC51C7D036FDF2",
      "TransactionType" : "XChainCommit",
      "TxnSignature" : "58BC9DF00A61B9B3F8A75620F8F0EA26A9E8AC55758BC957441B6DB6C52AA27405C42C35A83E2B43E65538DD29F82E3046C8B8ABBC50CAF0F542F49FCB3E9A05",
      "XChainBridge" : {
         "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
         "IssuingChainIssue" : {
            "currency" : "XRP"
         },
         "LockingChainDoor" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
         "LockingChainIssue" : {
            "currency" : "XRP"
         }
      },
      "XChainClaimID" : "2",
      "date" : 752637150,
      "hash" : "926D50565D691C072C4A25440E1A58DC6F1D79A7DC7D05949081164761482824",
      "inLedger" : 9,
      "ledger_index" : 9
   },
   "type" : "transaction",
   "validated" : true
}
)str";

const char TxAccCreateAtt[] = R"str(
{
  "account_history_tx_index": 0,
  "engine_result": "tesSUCCESS",
  "engine_result_code": 0,
  "ledger_index": 6,
  "meta": {
    "AffectedNodes": [
      {
        "CreatedNode": {
          "LedgerEntryType": "XChainOwnedCreateAccountClaimID",
          "LedgerIndex": "17DF9C22F8E6BFE2959C01E8F51F791D5F5243368E3AE2234085ED5F0B805538",
          "NewFields": {
            "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            "XChainAccountCreateCount": "1",
            "XChainBridge": {
              "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
              "IssuingChainIssue": {
                "currency": "XRP"
              },
              "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
              "LockingChainIssue": {
                "currency": "XRP"
              }
            },
            "XChainCreateAccountAttestations": [
              {
                "XChainCreateAccountProofSig": {
                  "Amount": "400000000",
                  "AttestationRewardAccount": "rGrQ8QEAtiKvjXwFxZxv6pxmzWfDcqWeUV",
                  "AttestationSignerAccount": "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                  "Destination": "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7",
                  "PublicKey": "EDE9ABB25A0921467973A2987E64B9B1C945D043EA528C791C9527D91EE43A5387",
                  "SignatureReward": "100",
                  "WasLockingChainSend": 1
                }
              }
            ]
          }
        }
      },
      {
        "ModifiedNode": {
          "FinalFields": {
            "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            "Balance": "99999999899999920",
            "Flags": 1048576,
            "OwnerCount": 3,
            "Sequence": 9
          },
          "LedgerEntryType": "AccountRoot",
          "LedgerIndex": "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8",
          "PreviousFields": {
            "OwnerCount": 2
          },
          "PreviousTxnID": "0DA74C4F4F014943361A4740A7E2C88E7FA1D5AC5B3C90C35B2BAFCE303345DA",
          "PreviousTxnLgrSeq": 4
        }
      },
      {
        "ModifiedNode": {
          "FinalFields": {
            "Account": "rGrQ8QEAtiKvjXwFxZxv6pxmzWfDcqWeUV",
            "Balance": "19999980",
            "Flags": 0,
            "OwnerCount": 0,
            "Sequence": 4
          },
          "LedgerEntryType": "AccountRoot",
          "LedgerIndex": "4EAB64DD56AA7EBFCF2E0608D5B5E0F80DE29EC1989BC3A6F740D390ED08105E",
          "PreviousFields": {
            "Balance": "20000000",
            "Sequence": 3
          },
          "PreviousTxnID": "D469192831BAE9417BAFC13B7016350C6EFA152C7ED03DF0E379E5AFD640D848",
          "PreviousTxnLgrSeq": 3
        }
      },
      {
        "ModifiedNode": {
          "FinalFields": {
            "Flags": 0,
            "Owner": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
            "RootIndex": "D8120FC732737A2CF2E9968FDF3797A43B457F2A81AA06D2653171A1EA635204"
          },
          "LedgerEntryType": "DirectoryNode",
          "LedgerIndex": "D8120FC732737A2CF2E9968FDF3797A43B457F2A81AA06D2653171A1EA635204"
        }
      }
    ],
    "TransactionIndex": 0,
    "TransactionResult": "tesSUCCESS"
  },
  "transaction": {
    "Account": "rGrQ8QEAtiKvjXwFxZxv6pxmzWfDcqWeUV",
    "Amount": "400000000",
    "AttestationRewardAccount": "rGrQ8QEAtiKvjXwFxZxv6pxmzWfDcqWeUV",
    "AttestationSignerAccount": "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
    "Destination": "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7",
    "Fee": "20",
    "LastLedgerSequence": 9,
    "NetworkID": 15756,
    "OtherChainSource": "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
    "PublicKey": "EDE9ABB25A0921467973A2987E64B9B1C945D043EA528C791C9527D91EE43A5387",
    "Sequence": 3,
    "Signature": "5E3D0C3C9F07E7AF3D5EB51EDE00E2CC96FC4CCE138DBF6ED008C5BF251F65EA297866904D4A382D6ED690B74B94876B60905EF51646C1577210977582D21908",
    "SignatureReward": "100",
    "SigningPubKey": "EDE2CA6BDD44955449CA5A9575DEB73AAC79DB6C721F421D918293559A7AB125EF",
    "TransactionType": "XChainAddAccountCreateAttestation",
    "TxnSignature": "399959497096B29FD5B8FC304F30B9E99DB5569D78A7D1D8D3BB2BF71E9DE117BDF11B416745AA8BDCA937AA32282B26835E57564CE218B9B19BB9C9F246CF07",
    "WasLockingChainSend": 1,
    "XChainAccountCreateCount": "1",
    "XChainBridge": {
      "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
      "IssuingChainIssue": {
        "currency": "XRP"
      },
      "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
      "LockingChainIssue": {
        "currency": "XRP"
      }
    },
    "date": 752637131,
    "hash": "3623DCB1C5D7F6FB31B5A089614B3C21EC98FC4E009E90E56FF0AFFB58AD3B69",
    "inLedger": 6,
    "ledger_index": 6
  },
  "type": "transaction",
  "validated": true
}
)str";

const char TxAccClaimAtt[] = R"str(
{
   "account_history_tx_index" : 8,
   "engine_result" : "tesSUCCESS",
   "engine_result_code" : 0,
   "ledger_index" : 8,
   "meta" : {
      "AffectedNodes" : [
         {
            "ModifiedNode" : {
               "FinalFields" : {
                  "Account" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
                  "Flags" : 0,
                  "OtherChainSource" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
                  "OwnerNode" : "0",
                  "SignatureReward" : "100",
                  "XChainBridge" : {
                     "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                     "IssuingChainIssue" : {
                        "currency" : "XRP"
                     },
                     "LockingChainDoor" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                     "LockingChainIssue" : {
                        "currency" : "XRP"
                     }
                  },
                  "XChainClaimAttestations" : [
                     {
                        "XChainClaimProofSig" : {
                           "Amount" : "1000000",
                           "AttestationRewardAccount" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
                           "AttestationSignerAccount" : "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                           "Destination" : "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7",
                           "PublicKey" : "EDD6EBD41B688433A1E61B9F08C826EDF705797D4A0C77BBB6AB032A78337182B7",
                           "WasLockingChainSend" : 1
                        }
                     }
                  ],
                  "XChainClaimID" : "1"
               },
               "LedgerEntryType" : "XChainOwnedClaimID",
               "LedgerIndex" : "92A931EEADD4CAD8D47D56165C201C4AD9F69080D8C327025A8BEFF797121AF6",
               "PreviousFields" : {
                  "XChainClaimAttestations" : []
               },
               "PreviousTxnID" : "D90D9AD551C667B505950F4680F18AA47BD6146FA97B02F60B160229C81CE312",
               "PreviousTxnLgrSeq" : 7
            }
         },
         {
            "ModifiedNode" : {
               "FinalFields" : {
                  "Account" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
                  "Balance" : "19999985",
                  "Flags" : 0,
                  "OwnerCount" : 0,
                  "Sequence" : 5
               },
               "LedgerEntryType" : "AccountRoot",
               "LedgerIndex" : "B5160B5E8CE0D4158E080630898C4342CB192C211A633A60977A6E8F99DE8A62",
               "PreviousFields" : {
                  "Balance" : "20000005",
                  "Sequence" : 4
               },
               "PreviousTxnID" : "26B8A2514F53A87BC89D46CA0592386C7FC3FA21558DFE4EFDE392755DFFEE4C",
               "PreviousTxnLgrSeq" : 6
            }
         }
      ],
      "TransactionIndex" : 0,
      "TransactionResult" : "tesSUCCESS"
   },
   "transaction" : {
      "Account" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
      "Amount" : "1000000",
      "AttestationRewardAccount" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
      "AttestationSignerAccount" : "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
      "Destination" : "ra8nske62jqNUehr9MEhyEZwMwZgdmCkf7",
      "Fee" : "20",
      "LastLedgerSequence" : 11,
      "NetworkID" : 15756,
      "OtherChainSource" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
      "PublicKey" : "EDD6EBD41B688433A1E61B9F08C826EDF705797D4A0C77BBB6AB032A78337182B7",
      "Sequence" : 4,
      "Signature" : "ADA5361254949C86088C611A20D7C48624030332CFA0493CC219AD7CE952C0BB854CFFE2042A432783FA271BA6BB705460174990F0207BFC2FFB4F2F675B1A00",
      "SigningPubKey" : "ED78A84396829B51E6C2571C05C5C4EA54D9FFAA694B39AC05F5A43434DB0F9D26",
      "TransactionType" : "XChainAddClaimAttestation",
      "TxnSignature" : "69E776A3076BF0B663CEDF9B58E2FEA555F02F43894162EC629E3173DCF65DAB7956FAD823B6D2D0BD20FDB0E47D602A01D06773F9A019FD23FDB928AAE1B20B",
      "WasLockingChainSend" : 1,
      "XChainBridge" : {
         "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
         "IssuingChainIssue" : {
            "currency" : "XRP"
         },
         "LockingChainDoor" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
         "LockingChainIssue" : {
            "currency" : "XRP"
         }
      },
      "XChainClaimID" : "1",
      "date" : 752637141,
      "hash" : "5F6D4D62A20686EE6FA2AE9D3CE70924D0EB03B80143B8671F30049B99EC1C69",
      "inLedger" : 8,
      "ledger_index" : 8
   },
   "type" : "transaction",
   "validated" : true
}
)str";

const char TxCreateBridge[] = R"str(
{
  "account_history_tx_index": -4,
  "engine_result": "tesSUCCESS",
  "engine_result_code": 0,
  "ledger_index": 4,
  "meta": {
    "AffectedNodes": [
      {
        "CreatedNode": {
          "LedgerEntryType": "DirectoryNode",
          "LedgerIndex": "3B61CB34EDD84A23167F8C8937AB3B6898E594BA76E08AE489FD1F5EA8F01D13",
          "NewFields": {
            "Owner": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
            "RootIndex": "3B61CB34EDD84A23167F8C8937AB3B6898E594BA76E08AE489FD1F5EA8F01D13"
          }
        }
      },
      {
        "CreatedNode": {
          "LedgerEntryType": "Bridge",
          "LedgerIndex": "DF794CFEDA27E06DAE87A6EFB489614C1D3FF857F3980661F84FC01933B4AF6F",
          "NewFields": {
            "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
            "MinAccountCreateAmount": "10000000",
            "SignatureReward": "100",
            "XChainBridge": {
              "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
              "IssuingChainIssue": {
                "currency": "XRP"
              },
              "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
              "LockingChainIssue": {
                "currency": "XRP"
              }
            }
          }
        }
      },
      {
        "ModifiedNode": {
          "FinalFields": {
            "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
            "Balance": "19999990",
            "Flags": 0,
            "OwnerCount": 1,
            "Sequence": 4
          },
          "LedgerEntryType": "AccountRoot",
          "LedgerIndex": "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
          "PreviousFields": {
            "Balance": "20000000",
            "OwnerCount": 0,
            "Sequence": 3
          },
          "PreviousTxnID": "1FDB5664EA13A3C44AF5E50BEA33D437CF4E07F18DFFDBE1690E06447F1E779C",
          "PreviousTxnLgrSeq": 3
        }
      }
    ],
    "TransactionIndex": 0,
    "TransactionResult": "tesSUCCESS"
  },
  "transaction": {
    "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "Fee": "10",
    "Flags": 0,
    "LastLedgerSequence": 23,
    "MinAccountCreateAmount": "10000000",
    "NetworkID": 15755,
    "Sequence": 3,
    "SignatureReward": "100",
    "SigningPubKey": "02B612A1855D846C504F2B72F6931E31EA2EB695F3F6DB6A2FE64CEE4FB2B023C2",
    "TransactionType": "XChainCreateBridge",
    "TxnSignature": "3045022100DB0389C42CB9AC36559171BE062BD7D9C54A7499048E469A6AA56144201287380220228E5503BA4C60F7A6DA4253D8C8C79A2E1AD2A7873E9C05467857F44C909F27",
    "XChainBridge": {
      "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
      "IssuingChainIssue": {
        "currency": "XRP"
      },
      "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
      "LockingChainIssue": {
        "currency": "XRP"
      }
    },
    "date": 752637111,
    "hash": "C7C94CB45DEE44AF0DA90825EE3552C3B50E51109C4E42921E4F5893ECE12318",
    "inLedger": 4,
    "ledger_index": 4
  },
  "type": "transaction",
  "validated": true
}
)str";

}  // namespace tests

}  // namespace xbwd
