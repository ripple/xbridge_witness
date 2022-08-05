#pragma once

#include "ripple/protocol/KeyType.h"
#include "ripple/protocol/STXChainBridge.h"
#include <xbwd/app/Config.h>
#include <xbwd/core/DatabaseCon.h>

#include <ripple/beast/utility/Journal.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/TER.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/signal_set.hpp>

#include <condition_variable>
#include <optional>
#include <thread>
#include <vector>

namespace ripple {
class STXChainBridge;
class STAmount;
}  // namespace ripple

namespace xbwd {

class Federator;
class ServerHandler;

namespace rpc {
class ServerHandler;
}

class App
{
    ripple::Logs logs_;
    beast::Journal j_;

    // Database for cross chain transactions
    DatabaseCon xChainTxnDB_;

    std::optional<boost::asio::io_service::work> work_;
    std::vector<std::thread> threads_;
    boost::asio::io_service io_service_;
    boost::asio::signal_set signals_;

    std::shared_ptr<Federator> federator_;
    std::unique_ptr<rpc::ServerHandler> serverHandler_;

    std::condition_variable stoppingCondition_;
    mutable std::mutex stoppingMutex_;
    std::atomic<bool> isTimeToStop_ = false;

public:
    explicit App(
        config::Config const& config,
        beast::severities::Severity logLevel);

    ~App();

    bool
    setup(config::Config const& config);

    void
    start();

    void
    stop();

    void
    run();

    void
    signalStop();

    DatabaseCon&
    getXChainTxnDB();

    boost::asio::io_service&
    get_io_service();
};

}  // namespace xbwd
