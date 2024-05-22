#include <xbwd/app/DBInit.h>

#include <format>

namespace xbwd {
namespace db_init {

std::string const&
xChainDBName()
{
    static std::string const r{"xchain_txns.db"};
    return r;
}

// Use the source that produce the event to get the table name
std::string const&
xChainTableName(ChainType src)
{
    if (src == ChainType::locking)
    {
        static std::string const r{"XChainTxnLockingToIssuing"};
        return r;
    }
    static std::string const r{"XChainTxnIssuingToLocking"};
    return r;
}

std::string const&
xChainCreateAccountTableName(ChainType src)
{
    if (src == ChainType::locking)
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
            CREATE TABLE IF NOT EXISTS {} (
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
            CREATE INDEX IF NOT EXISTS {}ClaimIDIdx ON {}(ClaimID);",
        )sql";

        auto constexpr createAccTblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {} (
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
            CREATE INDEX IF NOT EXISTS {}CreateCountIdx ON {}(CreateCount);",
        )sql";

        auto constexpr syncTblFmtStr = R"sql(
            CREATE TABLE IF NOT EXISTS {} (
                ChainType         UNSIGNED PRIMARY KEY,
                TransID           CHARACTER(64),
                LedgerSeq         BIGINT UNSIGNED);
        )sql";

        for (auto cd : {ChainType::locking, ChainType::issuing})
        {
            auto const& xChainTabName = xChainTableName(cd);
            r.push_back(std::format(tblFmtStr, xChainTabName));
            r.push_back(std::format(idxFmtStr, xChainTabName, xChainTabName));

            auto const& xChainCATabName = xChainCreateAccountTableName(cd);
            r.push_back(std::format(createAccTblFmtStr, xChainCATabName));
            r.push_back(std::format(
                createAccIdxFmtStr, xChainCATabName, xChainCATabName));
        }

        r.push_back(std::format(syncTblFmtStr, xChainSyncTable));

        r.push_back("END TRANSACTION;");
        return r;
    }();
    return result;
}

}  // namespace db_init
}  // namespace xbwd
