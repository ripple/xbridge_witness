#pragma once

#include <xbwd/app/Config.h>
#include <xbwd/core/DatabaseCon.h>
#include <xbwd/rpc/ServerHandler.h>

#include <ripple/beast/utility/Journal.h>
#include <ripple/protocol/KeyType.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STXChainBridge.h>
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

class BasicApp
{
protected:
    std::optional<boost::asio::io_service::work> work_;
    std::vector<std::thread> threads_;
    boost::asio::io_service io_service_;

public:
    BasicApp(std::size_t numberOfThreads);
    ~BasicApp();

    boost::asio::io_service&
    get_io_service()
    {
        return io_service_;
    }
};

class App : public BasicApp
{
    ripple::Logs logs_;
    beast::Journal j_;

    // Database for cross chain transactions
    DatabaseCon xChainTxnDB_;

    boost::asio::signal_set signals_;

    std::shared_ptr<Federator> federator_;
    std::unique_ptr<rpc::ServerHandler> serverHandler_;

    std::condition_variable stoppingCondition_;
    mutable std::mutex stoppingMutex_;
    std::atomic<bool> isTimeToStop_ = false;
    std::unique_ptr<config::Config> config_;

public:
    explicit App(
        std::unique_ptr<config::Config> config,
        beast::severities::Severity logLevel);

    bool
    setup();

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

    config::Config&
    config();

    Json::Value
    getInfo() const;
};

}  // namespace xbwd
