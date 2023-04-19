#include <xbwd/app/App.h>

#include <xbwd/app/BuildInfo.h>
#include <xbwd/app/DBInit.h>
#include <xbwd/federator/Federator.h>
#include <xbwd/rpc/RPCCall.h>
#include <xbwd/rpc/ServerHandler.h>

#include <ripple/beast/core/CurrentThreadName.h>
#include <ripple/protocol/STAmount.h>
#include <ripple/protocol/STXChainBridge.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/TER.h>

#include <fmt/format.h>

#include <filesystem>

namespace xbwd {

BasicApp::BasicApp(std::size_t numberOfThreads)
{
    work_.emplace(io_service_);
    threads_.reserve(numberOfThreads);

    while (numberOfThreads--)
    {
        threads_.emplace_back([this, numberOfThreads]() {
            beast::setCurrentThreadName(
                "io svc #" + std::to_string(numberOfThreads));
            this->io_service_.run();
        });
    }
}

BasicApp::~BasicApp()
{
    work_.reset();

    for (auto& t : threads_)
        t.join();
}

App::App(
    std::unique_ptr<config::Config> config,
    beast::severities::Severity logLevel)
    : BasicApp(std::thread::hardware_concurrency())
    , logs_(logLevel)
    , j_([&, this]() {
        if (!config->logFile.empty())
        {
            if (!logs_.open(config->logFile))
                std::cerr << "Can't open log file " << config->logFile
                          << std::endl;
        }
        // Optionally turn off logging to console.
        logs_.silent(config->logSilent);
        return logs_.journal("App");
    }())
    , xChainTxnDB_(
          config->dataDir,
          db_init::xChainDBName(),
          db_init::xChainDBPragma(),
          db_init::xChainDBInit(),
          j_)
    , signals_(io_service_)
    , config_(std::move(config))
{
    // TODO initialize the public and secret keys

    config_->rpcEndpoint = xbwd::rpc_call::addrToEndpoint(
        get_io_service(), config_->addrRpcEndpoint);
    config_->issuingChainConfig.chainIp = xbwd::rpc_call::addrToEndpoint(
        get_io_service(), config_->issuingChainConfig.addrChainIp);
    config_->lockingChainConfig.chainIp = xbwd::rpc_call::addrToEndpoint(
        get_io_service(), config_->lockingChainConfig.addrChainIp);

    try
    {
        federator_ = make_Federator(
            *this, get_io_service(), *config_, logs_.journal("Federator"));

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

    logRotation_ = std::thread(&App::logRotation, this);
}

void
App::stop()
{
    if (federator_)
        federator_->stop();
    if (serverHandler_)
        serverHandler_->stop();
    if (logRotation_.joinable())
        logRotation_.join();
}

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

std::shared_ptr<Federator>
App::federator()
{
    return federator_;
}

void
App::logRotation()
{
#ifdef _WIN32
    JLOG(j_.info()) << "Log rotation not supported on windows";
    return;
#endif

    if (config_->logFile.empty() || !config_->logSizeToRotateMb)
        return;

    JLOG(j_.info()) << "Log rotation thread started";

    auto makeBakPath = [](std::filesystem::path const orig,
                          unsigned const num) {
        if (!num)
            return orig;

        auto const p = orig.parent_path();
        auto fn = orig.stem();
        auto const ext = orig.extension();

        fn += fmt::format("_{}", num);
        std::filesystem::path newPath(p);
        newPath /= fn;
        newPath.replace_extension(ext);

        return newPath;
    };

    using namespace std::chrono_literals;

    std::filesystem::path const logFile(config_->logFile);
    uint64_t const logSize(
        static_cast<uint64_t>(config_->logSizeToRotateMb) << 20);

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lk{stoppingMutex_};
            stoppingCondition_.wait_for(
                lk, 1s, [this] { return isTimeToStop_.load(); });
        }
        if (isTimeToStop_)
            break;

        std::error_code ec;
        auto const size = std::filesystem::file_size(logFile, ec);
        if (ec)
        {
            JLOGV(
                j_.warn(),
                "Log rotation thread",
                ripple::jv("error", ec.value()),
                ripple::jv("msg", ec.message()));
            continue;
        }

        if (size < logSize)
            continue;

        unsigned num = config_->logFilesToKeep;
        std::filesystem::path const bak = makeBakPath(logFile, num);
        std::filesystem::remove(bak, ec);

        for (; num > 0; --num)
        {
            std::filesystem::path const bakDst = makeBakPath(logFile, num);
            std::filesystem::path const bakSrc = makeBakPath(logFile, num - 1);
            std::filesystem::rename(bakSrc, bakDst, ec);
        }
        logs_.rotate();
    }

    JLOG(j_.info()) << "Log rotation thread finished";
}

}  // namespace xbwd
