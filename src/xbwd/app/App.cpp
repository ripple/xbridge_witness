#include "ripple/protocol/SecretKey.h"
#include "ripple/protocol/TER.h"
#include "xbwd/rpc/ServerHandler.h"
#include <xbwd/app/App.h>

#include <xbwd/app/BuildInfo.h>
#include <xbwd/app/DBInit.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/rpc/ServerHandler.h>

#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/STXChainClaimProof.h>

namespace xbwd {

App::App(
    std::unique_ptr<config::Config> config,
    beast::severities::Severity logLevel)
    : logs_(logLevel)
    , j_(logs_.journal("App"))
    , xChainTxnDB_(
          config->dataDir,
          db_init::xChainDBName(),
          db_init::xChainDBPragma(),
          db_init::xChainDBInit())
    , signals_(io_service_)
    , config_(std::move(config))
{
    // TODO initialize the public and secret keys
    int numThreads = std::thread::hardware_concurrency();
    work_.emplace(io_service_);
    threads_.reserve(numThreads);

    while (numThreads--)
    {
        threads_.emplace_back([this, numThreads]() {
            beast::setCurrentThreadName(
                "io svc #" + std::to_string(numThreads));
            this->io_service_.run();
        });
    }

    try
    {
        federator_ = make_Federator(
            *this,
            get_io_service(),
            config_->bridge,
            config_->keyType,
            config_->signingKey,
            config_->lockingchainIp,
            config_->issuingchainIp,
            config_->lockingChainRewardAccount,
            config_->issuingChainRewardAccount,
            logs_.journal("Federator"));

        serverHandler_ = std::make_unique<rpc::ServerHandler>(
            *this, get_io_service(), logs_.journal("ServerHandler"));
    }
    catch (std::exception const& e)
    {
        JLOGV(
            j_.fatal(),
            "Exception while creating app ",
            ripple::jv("what", e.what()));
        work_.reset();
        throw;
    }
}

App::~App()
{
    work_.reset();

    for (auto& t : threads_)
        t.join();
}

bool
App::setup()
{
    // We want to intercept CTRL-C and the standard termination signal
    // SIGTERM and terminate the process. This handler will NEVER be invoked
    // twice.
    //
    // Note that async_wait is "one-shot": for each call, the handler will
    // be invoked exactly once, either when one of the registered signals in
    // the signal set occurs or the signal set is cancelled. Subsequent
    // signals are effectively ignored (technically, they are queued up,
    // waiting for a call to async_wait).
    signals_.add(SIGINT);
    signals_.add(SIGTERM);
    signals_.async_wait(
        [this](boost::system::error_code const& ec, int signum) {
            // Indicates the signal handler has been aborted; do nothing
            if (ec == boost::asio::error::operation_aborted)
                return;

            JLOG(j_.info()) << "Received signal " << signum;

            if (signum == SIGTERM || signum == SIGINT)
                signalStop();
        });

    {
        std::vector<ripple::Port> const ports = [&] {
            auto const& endpoint = config_->rpcEndpoint;
            std::vector<ripple::Port> r;
            ripple::Port p;
            p.ip = endpoint.address();
            p.port = endpoint.port();
            // TODO - encode protocol in config
            p.protocol.insert("http");
            r.push_back(p);
            return r;
        }();

        if (!serverHandler_->setup(ports))
        {
            return false;
        }
    }

    return true;
}

void
App::start()
{
    JLOG(j_.info()) << "Application starting. Version is "
                    << build_info::getVersionString();
    if (federator_)
        federator_->start();
    // TODO: unlockMainLoop should go away
    federator_->unlockMainLoop();
};

void
App::stop()
{
    if (federator_)
        federator_->stop();
    if (serverHandler_)
        serverHandler_->stop();
};

void
App::run()
{
    {
        std::unique_lock<std::mutex> lk{stoppingMutex_};
        stoppingCondition_.wait(lk, [this] { return isTimeToStop_.load(); });
    }
    JLOG(j_.debug()) << "Application stopping";
    stop();
}

DatabaseCon&
App::getXChainTxnDB()
{
    return xChainTxnDB_;
}

boost::asio::io_service&
App::get_io_service()
{
    return io_service_;
}

void
App::signalStop()
{
    if (!isTimeToStop_.exchange(true))
        stoppingCondition_.notify_all();
}

config::Config&
App::config()
{
    return *config_;
}
}  // namespace xbwd
