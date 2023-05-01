#include <xbwd/app/DBInit.h>

#include <fmt/core.h>

namespace xbwd {
namespace db_init {

std::string const&
xChainDBName()
{
    static std::string const r{"xchain_txns.db"};
    return r;
}

std::string const&
xChainTableName(ChainDir dir)
{
    if (dir == ChainDir::lockingToIssuing)
    {
        static std::string const r{"XChainTxnLockingToIssuing"};
        return r;
    }
    static std::string const r{"XChainTxnIssuingToLocking"};
    return r;
}

std::string const&
xChainCreateAccountTableName(ChainDir dir)
{
    if (dir == ChainDir::lockingToIssuing)
    {
        static std::string const r{"XChainTxnCreateAccountLocking"};
        return r;
    }
    static std::string const r{"XChainTxnCreateAccountIssuing"};
    return r;
}

std::vector<std::string> const&
xChainDBPragma()
{
    static std::vector<std::string> const result = [] {
        std::vector<std::string> r;
        r.push_back("PRAGMA journal_size_limit=1582080;");
        return r;
    }();

    return result;
};

std::vector<std::string> const&
xChainDBInit()
{
    static std::vector<std::string> result = [] {
        std::vector<std::string> r;
        r.push_back("BEGIN TRANSACTION;");

        // DeliveredAmt is encoded as a serialized STAmount
        //              this is raw data - no encoded.
        // Success is a bool (but soci complains about using bools)

        auto constexpr tblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {table_name} (
                TransID           CHARACTER(64) PRIMARY KEY,
                LedgerSeq         BIGINT UNSIGNED,
                ClaimID           BIGINT UNSIGNED,
                Success           UNSIGNED,
                DeliveredAmt      BLOB,
                Bridge            BLOB,
                SendingAccount    BLOB,
                RewardAccount     BLOB,
                OtherChainDst     BLOB,
                SigningAccount    BLOB,
                PublicKey         BLOB,
                Signature         BLOB);
        )sql";
        auto constexpr idxFmtStr = R"sql(
            CREATE INDEX IF NOT EXISTS {table_name}ClaimIDIdx ON {table_name}(ClaimID);",
        )sql";

        auto constexpr createAccTblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {table_name} (
                TransID           CHARACTER(64) PRIMARY KEY,
                LedgerSeq         BIGINT UNSIGNED,
                CreateCount       BIGINT UNSIGNED,
                Success           UNSIGNED,
                DeliveredAmt      BLOB,
                RewardAmt         BLOB,
                Bridge            BLOB,
                SendingAccount    BLOB,
                RewardAccount     BLOB,
                OtherChainDst     BLOB,
                SigningAccount    BLOB,
                PublicKey         BLOB,
                Signature         BLOB);
        )sql";
        auto constexpr createAccIdxFmtStr = R"sql(
            CREATE INDEX IF NOT EXISTS {table_name}CreateCountIdx ON {table_name}(CreateCount);",
        )sql";

        auto constexpr syncTblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {table_name} (
                ChainType         UNSIGNED PRIMARY KEY,
                TransID           CHARACTER(64),
                LedgerSeq         BIGINT UNSIGNED);
        )sql";

        for (auto cd : {ChainDir::lockingToIssuing, ChainDir::issuingToLocking})
        {
            r.push_back(fmt::format(
                tblFmtStr, fmt::arg("table_name", xChainTableName(cd))));
            r.push_back(fmt::format(
                idxFmtStr, fmt::arg("table_name", xChainTableName(cd))));

            r.push_back(fmt::format(
                createAccTblFmtStr,
                fmt::arg("table_name", xChainCreateAccountTableName(cd))));
            r.push_back(fmt::format(
                createAccIdxFmtStr,
                fmt::arg("table_name", xChainCreateAccountTableName(cd))));
        }

        r.push_back(fmt::format(
            syncTblFmtStr, fmt::arg("table_name", xChainSyncTable)));

        r.push_back("END TRANSACTION;");
        return r;
    }();
    return result;
}

}  // namespace db_init
}  // namespace xbwd
