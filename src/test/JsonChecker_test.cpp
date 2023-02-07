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

#include <xbwd/core/JsonChecker.h>

#include <ripple/basics/Log.h>
#include <ripple/beast/unit_test/suite.hpp>
#include <ripple/json/Output.h>
#include <ripple/json/json_forwards.h>
#include <ripple/json/json_get_or_throw.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>

namespace xbwd {

namespace tests {

class JsonChecker_test : public beast::unit_test::suite
{
    ripple::Logs logs_;
    beast::Journal j_;
    JsonChecker jc_;

    void
    testGoodCase()
    {
        testcase("Good case");

        static std::string const testArray[] = {
            R"json(
                {
                   "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                   "Amount" : "10000000",
                   "Destination" : "r4LjXuw5M2y42aS4BWzppJ4eV1t4x3SL8P",
                   "Fee" : "10",
                   "Flags" : 0,
                   "LastLedgerSequence" : 26,
                   "Sequence" : 16,
                   "SignatureReward" : "100",
                   "SigningPubKey" : "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
                   "TransactionType" : "XChainAccountCreateCommit",
                   "TxnSignature" : "304402202EF4C0F791DC2DA4263F6708F69E617DFF7CC8C5C8C1BEA54BFBE07F9B1ED18402200F273E4F9B4A7DD8C0AE89A77D6F020AF7D310A6D15456C89EC5CE57256FA60B",
                   "XChainBridge" : {
                      "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                      "IssuingChainIssue" : {
                         "currency" : "XRP"
                      },
                      "LockingChainDoor" : "rJp8AEevarD8E4jVxFHkoymoM8REJu8qmC",
                      "LockingChainIssue" : {
                         "currency" : "XRP"
                      }
                   },
                   "date" : 728590661,
                   "hash" : "71AB015933294612DDA836980C919176DC0E93AE7BCCED1AF85C49A946C9B0BA"
                }
                )json",
            R"json(
                {
                  "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "Fee" : "10",
                  "Flags" : 0,
                  "LastLedgerSequence" : 24,
                  "Sequence" : 3,
                  "SignerEntries" : [
                     {
                        "SignerEntry" : {
                           "Account" : "r4N2ktxi1AsURTtoDfAZeRon26WXQsUWRb",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rHNwE2nbbiGCN9p6PcgnGmoRdpdqGymvkP",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rUQimdQ7JUvnUWtwQqRQSr687tYCEoZjXV",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rsBKES9uaBZ864KzzsvYtiJtf3b8XpswHX",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rQpdTUEYGnAvUSR2FQcW4fFvUrcPYdirge",
                           "SignerWeight" : 1
                        }
                     }
                  ],
                  "SignerQuorum" : 4,
                  "SigningPubKey" : "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
                  "TransactionType" : "SignerListSet",
                  "TxnSignature" : "3045022100CF8DC237EA443F42961CADA07B6F48ADBFAED43EB62767E6B4E748A9ADC57E810220428A052151F428B65C13E34A41B86433DCFC9C3AD00ABE50CF75739BC3134FC2",
                  "date" : 726281052,
                  "hash" : "8DEFA31C1D644B1106FC56919A7D47752F76131C3FA849A890E5824E8C609777"
                }
                )json",
            R"json(
                {
                    "Account" : "raFcdz1g8LWJDJWJE2ZKLRGdmUmsTyxaym",
                    "Amount" : "10000000",
                    "Fee" : "10",
                    "Flags" : 0,
                    "LastLedgerSequence" : 29,
                    "OtherChainDestination" : "rJdTJRJZ6GXCCRaamHJgEqVzB7Zy4557Pi",
                    "Sequence" : 9,
                    "SigningPubKey" : "026BB09608B42B5CB03F142B1325F52CF3DF5EBC4B2D3DE656F105701C28C0762C",
                    "TransactionType" : "XChainCommit",
                    "TxnSignature" : "304402206E889091C6CDE51462E5E0816A3DBFCD5D6CB3670D21657F7E6AFB0D21F3FB01022034759F6C6BA1B127A28B81C15D81475DCEBD579213BFBD6B4AB714E9181AF587",
                    "XChainBridge" : {
                       "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                       "IssuingChainIssue" : {
                          "currency" : "XRP"
                       },
                       "LockingChainDoor" : "rJp8AEevarD8E4jVxFHkoymoM8REJu8qmC",
                       "LockingChainIssue" : {
                          "currency" : "XRP"
                       }
                    },
                    "XChainClaimID" : "1",
                    "date" : 726281110,
                    "hash" : "1977EC7B87F3DDDCCDC277D2B3EA2B76DE7EDF0204EFD0868CE8A5C412255881"
                }
                )json",
            R"json(
                {
                    "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                    "Fee" : "10",
                    "Flags" : 0,
                    "LastLedgerSequence" : 24,
                    "Sequence" : 4,
                    "SetFlag" : 4,
                    "SigningPubKey" : "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
                    "TransactionType" : "AccountSet",
                    "TxnSignature" : "3045022100EBA67CEEF97203EE75A69E312DFEE314FDA5BBE5A511759D8F39AED14C194E54022039226BE1D1DEE9C70FA77F82B272901F2F9587CC8B5AAFC870B07DAF2C0D29C6",
                    "date" : 726281052,
                    "hash" : "B776D2D966AE934BABF525918E5AF4C9CF9DB54E654BD1411E61F46C8FE5CFFE"
                }
                )json"};

        unsigned i = 0;
        for (auto const& test : testArray)
        {
            Json::Value jv;
            Json::Reader().parse(test, jv);
            bool failed = true;
            try
            {
                failed = !jc_.check(jv);
            }
            catch (std::exception const& ex)
            {
                JLOGV(j_.warn(), "Good Case failed", ripple::jv("idx", i));
            }

            BEAST_EXPECT(!failed);
            ++i;
        }
    }

    void
    testBadCase()
    {
        testcase("Bad case");

        static std::string const testArray[] = {
            R"json(
                {
                   "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                   "Amount" : "10000000",
                   "Destination" : "r4LjXuw5M2y42aS4BWzppJ4eV1t4x3SL8P",
                   "Fee" : "10",
                   "Flags" : 0,
                   "LastLedgerSequence" : 26,
                   "Sequence" : 16,
                   "SignatureReward" : "100",
                   "SigningPubKey" : "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
                   "TransactionType" : "XChainAccountCreateCommit",
                   "TxnSignature" : "304402202EF4C0F791DC2DA4263F6708F69E617DFF7CC8C5C8C1BEA54BFBE07F9B1ED18402200F273E4F9B4A7DD8C0AE89A77D6F020AF7D310A6D15456C89EC5CE57256FA60B",
                   "XChainBridge" : {
                      "Extra": 1,
                      "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                      "IssuingChainIssue" : {
                         "currency" : "XRP"
                      },
                      "LockingChainDoor" : "rJp8AEevarD8E4jVxFHkoymoM8REJu8qmC",
                      "LockingChainIssue" : {
                         "currency" : "XRP"
                      }
                   },
                   "date" : 728590661,
                   "hash" : "71AB015933294612DDA836980C919176DC0E93AE7BCCED1AF85C49A946C9B0BA"
                }
                )json",
            R"json(
                {
                  "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "Fee" : "10",
                  "Flags" : 0,
                  "LastLedgerSequence" : 24,
                  "Sequence" : 3,
                  "SignerEntries" : [
                     {
                        "SignerEntry" : {
                           "Account" : "r4N2ktxi1AsURTtoDfAZeRon26WXQsUWRb",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rHNwE2nbbiGCN9p6PcgnGmoRdpdqGymvkP",
                           "SignerWeight" : 1,
                           "Extra": 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rUQimdQ7JUvnUWtwQqRQSr687tYCEoZjXV",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rsBKES9uaBZ864KzzsvYtiJtf3b8XpswHX",
                           "SignerWeight" : 1
                        }
                     },
                     {
                        "SignerEntry" : {
                           "Account" : "rQpdTUEYGnAvUSR2FQcW4fFvUrcPYdirge",
                           "SignerWeight" : 1
                        }
                     }
                  ],
                  "SignerQuorum" : 4,
                  "SigningPubKey" : "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
                  "TransactionType" : "SignerListSet",
                  "TxnSignature" : "3045022100CF8DC237EA443F42961CADA07B6F48ADBFAED43EB62767E6B4E748A9ADC57E810220428A052151F428B65C13E34A41B86433DCFC9C3AD00ABE50CF75739BC3134FC2",
                  "date" : 726281052,
                  "hash" : "8DEFA31C1D644B1106FC56919A7D47752F76131C3FA849A890E5824E8C609777"
                }
                )json",
            R"json(
                {
                    "Account" : "raFcdz1g8LWJDJWJE2ZKLRGdmUmsTyxaym",
                    "Amount" : "10000000",
                    "Fee" : "10",
                    "Flags" : 0,
                    "LastLedgerSequence" : 29,
                    "OtherChainDestination" : "rJdTJRJZ6GXCCRaamHJgEqVzB7Zy4557Pi",
                    "Sequence" : 9,
                    "SigningPubKey" : "026BB09608B42B5CB03F142B1325F52CF3DF5EBC4B2D3DE656F105701C28C0762C",
                    "TransactionType" : "XChainCommit",
                    "TxnSignature" : "304402206E889091C6CDE51462E5E0816A3DBFCD5D6CB3670D21657F7E6AFB0D21F3FB01022034759F6C6BA1B127A28B81C15D81475DCEBD579213BFBD6B4AB714E9181AF587",
                    "XChainBridge" : {
                       "IssuingChainDoor" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                       "IssuingChainIssue" : {
                          "currency" : "XRP"
                       },
                       "LockingChainDoor" : "rJp8AEevarD8E4jVxFHkoymoM8REJu8qmC",
                       "LockingChainIssue" : {
                          "currency" : "XRP"
                       }
                    },
                    "XChainClaimID" : "1",
                    "Extra": 1,
                    "date" : 726281110,
                    "hash" : "1977EC7B87F3DDDCCDC277D2B3EA2B76DE7EDF0204EFD0868CE8A5C412255881"
                }
                )json",
            R"json(
                {
                    "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                    "Fee" : "10",
                    "Flags" : 0,
                    "LastLedgerSequence" : 24,
                    "Sequence" : 4,
                    "Extra": 1,
                    "SetFlag" : 4,
                    "SigningPubKey" : "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
                    "TransactionType" : "AccountSet",
                    "TxnSignature" : "3045022100EBA67CEEF97203EE75A69E312DFEE314FDA5BBE5A511759D8F39AED14C194E54022039226BE1D1DEE9C70FA77F82B272901F2F9587CC8B5AAFC870B07DAF2C0D29C6",
                    "date" : 726281052,
                    "hash" : "B776D2D966AE934BABF525918E5AF4C9CF9DB54E654BD1411E61F46C8FE5CFFE"
                }
                )json"};

        unsigned i = 0;
        unsigned failed_cnt = 0;
        for (auto const& test : testArray)
        {
            Json::Value jv;
            Json::Reader().parse(test, jv);
            bool failed = true;
            try
            {
                failed = !jc_.check(jv);
            }
            catch (std::exception const& ex)
            {
                JLOGV(j_.warn(), "Bad Case exception", ripple::jv("idx", i));
                ++failed_cnt;
                BEAST_EXPECT(failed_cnt == i + 1);
            }
            ++i;
        }
    }

public:
    JsonChecker_test()
        : logs_(beast::severities::Severity::kTrace)
        , j_(logs_.journal("App"))
        , jc_(j_)
    {
    }

    void
    run() override
    {
        testGoodCase();
        testBadCase();
    }
};

BEAST_DEFINE_TESTSUITE(JsonChecker, xbridge_witnessd, xbwd);

}  // namespace tests

}  // namespace xbwd
