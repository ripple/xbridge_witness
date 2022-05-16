#ifndef ATTNSERVER_H_INCLUDED
#define ATTNSERVER_H_INCLUDED

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>


//#include <ripple/app/sidechain/impl/DoorKeeper.h>
//#include <ripple/app/sidechain/impl/MainchainListener.h>
//#include <ripple/app/sidechain/impl/SidechainListener.h>
//#include <ripple/app/sidechain/impl/SignatureCollector.h>
//#include <ripple/app/sidechain/impl/SignerList.h>
//#include <ripple/app/sidechain/impl/TicketHolder.h>
#include <ripple/basics/Buffer.h>
#include <ThreadSaftyAnalysis.h>
#include <ripple/basics/UnorderedContainers.h>
#include <ripple/basics/base_uint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/core/Config.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Quality.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/STXChainClaimProof.h>

#include <FederatorEvents.h>

namespace boost {
namespace asio {
    class io_context;
}
}

namespace ripple {
namespace sidechain {

class ChainListener;

enum ChainType { sideChain, mainChain };

// --------------------------------------------------------------------
// --------------------------------------------------------------------
struct ChainConfig
{
    ChainType chaintype;
    std::string ip;
    uint16_t port;
    std::string door_account_str;
    AccountID door_account;
};

struct PeerConfig {
    std::string ip;
    uint16_t port;
    PublicKey public_key;
};

using SNTPServerConfig = std::vector<std::string>;

struct AttnServerConfig
{
    std::string our_public_key_str;
    PublicKey our_public_key;
    SecretKey our_secret_key;
    uint16_t  port_peer; 
    uint16_t  port_ws;  

    std::string ssk_keys_str;
    std::string ssk_cert_str;
    
    ChainConfig mainchain;
    ChainConfig sidechain;

    std::vector<PeerConfig> peers;
    SNTPServerConfig sntp_servers;

    std::string db_path;

};

enum class UnlockMainLoopKey { app, mainChain, sideChain };

// --------------------------------------------------------------------
// --------------------------------------------------------------------
class AttnServer
{
public:
    // These enums are encoded in the transaction. Changing the order will break
    // backward compatibility. If a new type is added change txnTypeLast.
    enum class TxnType { xChain, refund };
    constexpr static std::uint8_t txnTypeLast = 2;

private:
    AttnServerConfig cfg_;
    
    std::shared_ptr<ChainListener> mainchainListener_;
    std::shared_ptr<ChainListener> sidechainListener_;

public:
    AttnServer(std::string const& config_filename);

    std::string process_rpc_request(std::string_view data);

    void start();

    void stop() EXCLUDES(m_);

    // Don't process any events until the bootstrap has a chance to run
    void unlockMainLoop(UnlockMainLoopKey key) EXCLUDES(m_);

#ifdef LATER
    void addPendingTxnSig(
        TxnType txnType,
        ChainType chaintype,
        PublicKey const& federatorPk,
        uint256 const& srcChainTxnHash,
        std::optional<uint256> const& dstChainTxnHash,
        STAmount const& amt,
        AccountID const& srcChainSrcAccount,
        AccountID const& dstChainDstAccount,
        std::uint32_t seq,
        Buffer&& sig) EXCLUDES(federatorPKsMutex_, pendingTxnsM_, toSendTxnsM_);

    void addPendingTxnSig(ChainType chaintype, PublicKey const& publicKey, uint256 const& mId, Buffer&& sig);
#endif
    
    // Return true if a transaction with this sequence has already been sent
    bool alreadySent(ChainType chaintype, std::uint32_t seq) const;

    void setLastXChainTxnWithResult(ChainType chaintype, std::uint32_t seq, std::uint32_t seqTook, uint256 const& hash);
    void setNoLastXChainTxnWithResult(ChainType chaintype);
    void stopHistoricalTxns(ChainType chaintype);
    void initialSyncDone(ChainType chaintype);

    // Get stats on the federator, including pending transactions and
    // initialization state
    Json::Value getInfo() const EXCLUDES(pendingTxnsM_);

    void sweep();

#ifdef LATER
    SignatureCollector& getSignatureCollector(ChainType chain);
    DoorKeeper& getDoorKeeper(ChainType chain);
    TicketRunner& getTicketRunner();
    void addSeqToSkip(ChainType chain, std::uint32_t seq) EXCLUDES(toSendTxnsM_);
    
    // TODO multi-sig refactor?
    void addTxToSend(ChainType chain, std::uint32_t seq, STTx const& tx) EXCLUDES(toSendTxnsM_);
#endif

    // Set the accountSeq to the max of the current value and the requested
    // value. This is done with a lock free algorithm.
    void setAccountSeqMax(ChainType chaintype, std::uint32_t reqValue);

    void mainLoop();


private:
    void loadConfig(std::string const& config_filename);
};

[[nodiscard]] static inline ChainType srcChainType(event::Dir dir)
{
    return dir == event::Dir::mainToSide ? ChainType::mainChain
                                         : ChainType::sideChain;
}

[[nodiscard]] static inline ChainType dstChainType(event::Dir dir)
{
    return dir == event::Dir::mainToSide ? ChainType::sideChain
                                         : ChainType::mainChain;
}

[[nodiscard]] static inline ChainType otherChainType(ChainType ct)
{
    return ct == ChainType::mainChain
        ? ChainType::sideChain
        : ChainType::mainChain;
}

[[nodiscard]] static inline ChainType getChainType(bool isMainchain)
{
    return isMainchain ? ChainType::mainChain
                       : ChainType::sideChain;
}

[[nodiscard]] static inline char const* chainTypeStr(ChainType ct)
{
    return ct == ChainType::mainChain ? "mainchain" : "sidechain";
}
    

}
}

#endif  // ATTNSERVER_H_INCLUDED
