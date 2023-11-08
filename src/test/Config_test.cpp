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

#include <xbwd/app/Config.h>

#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>

#include <fmt/format.h>

namespace xbwd {
namespace tests {

extern const char witness_good[];
extern const char witness_bad_json[];
extern const char witness_no_reward_locking[];
extern const char witness_no_bridge_locking[];
extern const char witness_no_rpc_locking[];
extern const char witness_no_reward_issuing[];
extern const char witness_no_bridge_issuing[];
extern const char witness_no_rpc_issuing[];
extern const char witness_no_db[];
extern const char witness_no_sign[];

class Config_test : public beast::unit_test::suite
{
private:
    std::unique_ptr<xbwd::config::Config>
    loadConfig(std::string const& data)
    {
        Json::Value jv;

        try
        {
            return Json::Reader().parse(data, jv)
                ? std::make_unique<xbwd::config::Config>(jv)
                : std::unique_ptr<xbwd::config::Config>();
        }
        catch (std::exception& ex)
        {
            // std::cerr << "Exception: " << ex.what() << std::endl;
            return std::unique_ptr<xbwd::config::Config>();
        }
    }

    void
    testLoadConfigFile()
    {
        testcase("Load config file");

        auto config = loadConfig(witness_good);
        BEAST_EXPECT(config);
    }

    void
    testBad1ConfigFile()
    {
        testcase("Load non-json config file");

        auto config = loadConfig(witness_bad_json);
        BEAST_EXPECT(!config);
    }

    void
    testConfigData()
    {
        testcase("Check config data");

        auto config = loadConfig(witness_good);

        if (!BEAST_EXPECT(!config->lockingChainConfig.addrChainIp.host.empty()))
            return;
        if (!BEAST_EXPECT(config->lockingChainConfig.addrChainIp.port))
            return;
        if (!BEAST_EXPECT(!config->lockingChainConfig.rewardAccount.isZero()))
            return;

        if (!BEAST_EXPECT(!config->issuingChainConfig.addrChainIp.host.empty()))
            return;
        if (!BEAST_EXPECT(config->issuingChainConfig.addrChainIp.port))
            return;
        if (!BEAST_EXPECT(!config->issuingChainConfig.rewardAccount.isZero()))
            return;

        if (!BEAST_EXPECT(!config->addrRpcEndpoint.host.empty()))
            return;
        if (!BEAST_EXPECT(config->addrRpcEndpoint.port))
            return;
        if (!BEAST_EXPECT(!config->dataDir.empty()))
            return;

        if (!BEAST_EXPECT(!config->bridge.lockingChainDoor().isZero()))
            return;
        if (!BEAST_EXPECT(!config->bridge.issuingChainDoor().isZero()))
            return;
    }

    void
    testBadData()
    {
        testcase("Check bad data");

        auto config = loadConfig(witness_no_reward_locking);
        BEAST_EXPECT(!config);
        config = loadConfig(witness_no_bridge_locking);
        BEAST_EXPECT(!config);
        config = loadConfig(witness_no_rpc_locking);
        BEAST_EXPECT(!config);

        config = loadConfig(witness_no_reward_issuing);
        BEAST_EXPECT(!config);
        config = loadConfig(witness_no_bridge_issuing);
        BEAST_EXPECT(!config);
        config = loadConfig(witness_no_rpc_issuing);
        BEAST_EXPECT(!config);

        config = loadConfig(witness_no_db);
        BEAST_EXPECT(!config);
        config = loadConfig(witness_no_sign);
        BEAST_EXPECT(!config);
    }

public:
    void
    run() override
    {
        testLoadConfigFile();
        testBad1ConfigFile();
        testConfigData();
        testBadData();
    }
};

BEAST_DEFINE_TESTSUITE(Config, app, xbwd);

const char witness_good[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_bad_json[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}

}
)str";

const char witness_no_reward_locking[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",witness_no_db
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": ""
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_bridge_locking[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_rpc_locking[] = R"str(
{
  "LockingChain": {
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_reward_issuing[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    }
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_bridge_issuing[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_rpc_issuing[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_sign[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "DBDir": "/home/user/ripple_conf/witness0/db",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

const char witness_no_db[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "127.0.0.1",
      "Port": 6006
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "IssuingChain": {
    "Endpoint": {
      "Host": "127.0.0.2",
      "Port": 6008
    },
    "TxnSubmit": {
      "ShouldSubmit": true,
      "SigningKeySeed": "saaymey1qGKvsVGA5CuRvoTvcBzwz",
      "SigningKeyType": "ed25519",
      "SubmittingAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
    },
    "RewardAccount": "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg"
  },
  "RPCEndpoint": {
    "Host": "127.0.0.3",
    "Port": 6010
  },
  "LogFile": "/home/user/ripple_conf/witness0/witness.log",
  "LogLevel": "Trace",
  "SigningKeySeed": "snnksgXkSTgCBuHJmHeTekJyj4qG6",
  "SigningKeyType": "ed25519",
  "XChainBridge": {
    "LockingChainDoor": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "LockingChainIssue": {"currency": "XRP"},
    "IssuingChainDoor": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "IssuingChainIssue": {"currency": "XRP"}
  }
}
)str";

}  // namespace tests

}  // namespace xbwd
