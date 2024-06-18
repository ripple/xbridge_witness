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

#include <xbwd/app/App.h>
#include <xbwd/basics/StructuredLog.h>
#include <xbwd/client/WebsocketClient.h>
#include <xbwd/rpc/fromJSON.h>

#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/jss.h>

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/websocket.hpp>
#include <fmt/format.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>

namespace xbwd {
namespace tests {
namespace all {

#if defined(_DEBUG) && defined(TESTS_DEBUG)
#define REL(...)
#define DBG(...) __VA_ARGS__
#define DBG_ARGS(...) __VA_OPT__(, ) __VA_ARGS__
#else
#define REL(...) __VA_ARGS__
#define DBG_ARGS(...)
#define DBG(...)
#endif

namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;
using namespace std::chrono_literals;
using namespace fmt::literals;

static std::unique_ptr<App> gApp;
static std::condition_variable gCv;
static std::mutex gMcv;

static unsigned const NUM_THREADS = 4;
static std::string const LHOST = "127.0.0.1";
static std::uint16_t const PORT_LOC = 56555;
static std::uint16_t const PORT_ISS = 56556;
static std::uint16_t const PORT_RPC = 56557;

extern const char witnessLocal[];
extern const char serverInfoLoc[];
extern const char serverInfoIss[];
extern const char bridgeAccInfoLoc[];
extern const char bridgeAccInfoIss[];
extern const char ledgerSubsLoc[];
extern const char ledgerSubsIss[];
extern const char ledgerEntryLoc[];
extern const char ledgerEntryIss[];
extern const char accTxLoc[];
extern const char accTxIss[];

extern const char ledgerAdvance[];
extern const char accTxLoc1[];
extern const char accTxLoc2[];
extern const char accInfoIss1[];
extern const char accTxIss1[];
extern const char accTxIss2[];
extern const char submIss[];

std::mutex gMCons;

static void
fail(boost::beast::error_code ec, char const* what)
{
    auto s = fmt::format("{}: {}", what, ec.message());
    std::lock_guard cl(gMCons);
    std::cerr << "Error, throw: " << s << std::endl;
    throw std::runtime_error(s);
}

template <class Rep, class Period>
static bool
wait_for(
    std::chrono::duration<Rep, Period> const& to,
    std::function<bool()> stop DBG_ARGS(std::string const& msg))
{
    std::unique_lock l{gMcv};
    if (stop())
        return true;

    auto const b = gCv.wait_for(l, to, stop);
    DBG({
        std::lock_guard cl(gMCons);
        std::cout << msg << ", wait finished: " << (b ? "condition" : "timeout")
                  << std::endl;
    })
    return b;
}

static std::string
prepForFmt(std::string_view const sw)
{
    std::string s;
    bool bopen = true;
    for (auto const c : sw)
    {
        if (c == '{')
            s += "{{";
        else if (c == '}')
            s += "}}";
        else if (c == '`')
        {
            s += bopen ? '{' : '}';
            bopen = !bopen;
        }
        else
            s += c;
    }
    return s;
}

//------------------------------------------------------------------------------

class engineLoc
{
    std::atomic_bool clientInit_ = false;
    unsigned accTxCtr = 0;
    unsigned closed_ledger = 5;

public:
    Json::Value
    process(Json::Value const& msg)
    {
        if (!msg.isMember(ripple::jss::method))
            throw std::runtime_error("No 'method' parameter");
        auto const method = msg[ripple::jss::method].asString();

        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "process(), method:" << method << ", side: " << side()
                      << std::endl;
        })

        std::string s;
        unsigned const id = msg["id"].asUInt();

        if (method == "server_info")
            s = fmt::format(
                fmt::runtime(prepForFmt(serverInfoLoc)), "id"_a = id);
        else if (method == "account_info")
            s = fmt::format(
                fmt::runtime(prepForFmt(bridgeAccInfoLoc)), "id"_a = id);
        else if (method == "subscribe")
            s = fmt::format(
                fmt::runtime(prepForFmt(ledgerSubsLoc)), "id"_a = id);
        else if (method == "ledger_entry")
            s = fmt::format(
                fmt::runtime(prepForFmt(ledgerEntryLoc)), "id"_a = id);
        else if (method == "account_tx")
        {
            if (!accTxCtr)
                s = fmt::format(
                    fmt::runtime(prepForFmt(accTxLoc)), "id"_a = id);
            else if (accTxCtr == 1)
                s = fmt::format(
                    fmt::runtime(prepForFmt(accTxLoc1)), "id"_a = id);
            else
                s = fmt::format(
                    fmt::runtime(prepForFmt(accTxLoc2)), "id"_a = id);
            ++accTxCtr;

            if (!clientInit_)
            {
                DBG({
                    std::lock_guard cl(gMCons);
                    std::cout << side() << " clientInit" << std::endl;
                })
                std::unique_lock l(gMcv);
                clientInit_ = true;
                gCv.notify_all();
            }
        }
        else
            throw std::runtime_error(fmt::format("Unknown method: {}", method));

        Json::Value jv;
        Json::Reader().parse(s, jv);
        return jv;
    }

    bool
    clientInit() const
    {
        return clientInit_;
    }

    Json::Value
    getNewLedger()
    {
        ++closed_ledger;
        Json::Value jv;
        Json::Reader().parse(
            fmt::format(
                fmt::runtime(prepForFmt(ledgerAdvance)),
                "closed_ledger"_a = closed_ledger),
            jv);
        return jv;
    }

    bool
    attSubmitted() const
    {
        return false;
    }

    bool
    blobOk() const
    {
        return false;
    }

    std::string
    side() const
    {
        return "locking";
    }
};

class engineIss
{
    std::atomic_bool clientInit_ = false;
    std::atomic_bool attSubmitted_ = false;
    std::atomic_bool blobOk_ = false;
    unsigned accInfoCtr = 0;
    unsigned accTxCtr = 0;
    unsigned closed_ledger = 4;

public:
    Json::Value
    process(Json::Value const& msg)
    {
        if (!msg.isMember(ripple::jss::method))
            throw std::runtime_error("No 'method' parameter");
        auto const method = msg[ripple::jss::method].asString();

        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "process(), method:" << method << ", side: " << side()
                      << std::endl;
        })

        std::string s;
        unsigned const id = msg["id"].asUInt();

        if (method == "server_info")
            s = fmt::format(
                fmt::runtime(prepForFmt(serverInfoIss)), "id"_a = id);
        else if (method == "account_info")
        {
            if (!accInfoCtr)
                s = fmt::format(
                    fmt::runtime(prepForFmt(bridgeAccInfoIss)), "id"_a = id);
            else
                s = fmt::format(
                    fmt::runtime(prepForFmt(accInfoIss1)), "id"_a = id);
            ++accInfoCtr;
        }
        else if (method == "subscribe")
            s = fmt::format(
                fmt::runtime(prepForFmt(ledgerSubsIss)), "id"_a = id);
        else if (method == "ledger_entry")
            s = fmt::format(
                fmt::runtime(prepForFmt(ledgerEntryIss)), "id"_a = id);
        else if (method == "account_tx")
        {
            if (!accTxCtr)
                s = fmt::format(
                    fmt::runtime(prepForFmt(accTxIss)), "id"_a = id);
            else if (accTxCtr == 1)
                s = fmt::format(
                    fmt::runtime(prepForFmt(accTxIss1)), "id"_a = id);
            else
                s = fmt::format(
                    fmt::runtime(prepForFmt(accTxIss2)), "id"_a = id);
            ++accTxCtr;

            if (!clientInit_)
            {
                DBG({
                    std::lock_guard cl(gMCons);
                    std::cout << side() << " clientInit" << std::endl;
                })
                std::unique_lock l(gMcv);
                clientInit_ = true;
                gCv.notify_all();
            }
        }
        else if (method == "submit")
        {
            auto const blob = msg["tx_blob"].asString();
            blobOk_ = blob.starts_with("12002E2100003D8C24");
            s = fmt::format(fmt::runtime(prepForFmt(submIss)), "id"_a = id);
            attSubmitted_ = true;
        }
        else
            throw std::runtime_error(fmt::format("Unknown method: {}", method));

        Json::Value jv;
        Json::Reader().parse(s, jv);
        return jv;
    }

    bool
    clientInit() const
    {
        return clientInit_;
    }

    Json::Value
    getNewLedger()
    {
        ++closed_ledger;
        Json::Value jv;
        Json::Reader().parse(
            fmt::format(
                fmt::runtime(prepForFmt(ledgerAdvance)),
                "closed_ledger"_a = closed_ledger),
            jv);
        return jv;
    }

    bool
    attSubmitted() const
    {
        return attSubmitted_;
    }

    bool
    blobOk() const
    {
        return blobOk_;
    }

    std::string
    side() const
    {
        return "issuing";
    }
};

//------------------------------------------------------------------------------

// Emulate some rippled responses
template <class engine>
class session : public std::enable_shared_from_this<session<engine>>
{
    boost::asio::io_context& ios_;
    websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer buffer_;
    std::atomic_bool finished_ = false;
    boost::asio::steady_timer t_;

    std::atomic_bool clientInit_ = false;
    engine e_;

public:
    explicit session(boost::asio::io_context& ios, tcp::socket&& socket)
        : ios_(ios), ws_(std::move(socket)), t_(ios_, 1s)
    {
    }

    void
    run()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "session::run()"
                      << ", side: " << e_.side() << std::endl;
        })
        ws_.set_option(websocket::stream_base::timeout::suggested(
            boost::beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(
                    boost::beast::http::field::server,
                    std::string(BOOST_BEAST_VERSION_STRING) +
                        " websocket-server-async");
            }));

        ws_.async_accept(boost::beast::bind_front_handler(
            &session::onAccept,
            std::enable_shared_from_this<session<engine>>::shared_from_this()));
    }

    void
    onAccept(boost::beast::error_code ec)
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "session::onAccept(), ec:" << ec
                      << ", side: " << e_.side() << std::endl;
        })
        if (ec == websocket::error::closed)
            return;
        if (ec)
            return fail(ec, "accept");
        doRead();
    }

    void
    doRead()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "session::doRead()"
                      << ", side: " << e_.side() << std::endl;
        })
        ws_.async_read(
            buffer_,
            boost::beast::bind_front_handler(
                &session::onRead,
                std::enable_shared_from_this<
                    session<engine>>::shared_from_this()));
    }

    void
    onRead(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "session::onRead(), ec:" << ec
                      << " bytes: " << bytes_transferred
                      << ", side: " << e_.side() << std::endl;
        })
        boost::ignore_unused(bytes_transferred);
        if (ec == websocket::error::closed)
            return;

        if (ec)
            fail(ec, "read");

        Json::Value jv;
        Json::Reader().parse(
            static_cast<char const*>(buffer_.data().data()), jv);
        auto const jr = e_.process(jv);

        auto const method = jv[ripple::jss::method].asString();
        if (method == "ledger_entry")
        {
            t_.expires_after(1s);
            t_.async_wait([this](const boost::system::error_code&) {
                return this->sendNewLedger();
            });
        }

        if (!clientInit_)
        {
            if (e_.clientInit())
            {
                clientInit_ = true;
                t_.expires_after(1s);
                t_.async_wait([this](const boost::system::error_code&) {
                    return this->sendNewLedger();
                });
            }
        }
        auto const s = to_string(jr);

        ws_.text(true);
        ws_.async_write(
            boost::asio::buffer(s),
            boost::beast::bind_front_handler(
                &session::onWrite,
                std::enable_shared_from_this<
                    session<engine>>::shared_from_this()));
    }

    void
    onWrite(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "session::onWrite(), ec:" << ec
                      << " bytes: " << bytes_transferred
                      << ", side: " << e_.side() << std::endl;
        })
        boost::ignore_unused(bytes_transferred);
        if (ec == websocket::error::closed)
            return;
        if (ec)
            return fail(ec, "write");
        buffer_.consume(buffer_.size());
        doRead();
    }

    void
    shutdown()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "session::shutdown()"
                      << ", side: " << e_.side() << std::endl;
        })
        ios_.post([this] {
            ws_.async_close({}, [this](boost::beast::error_code const& ec) {
                DBG({
                    std::lock_guard cl(gMCons);
                    std::cout << "session::onAsync_close(), ec:" << ec
                              << ", side: " << e_.side() << std::endl;
                })
                std::unique_lock l(gMcv);
                this->finished_ = true;
                gCv.notify_all();
            });
        });
    }

    bool
    finished() const
    {
        return finished_;
    }

    bool
    clientInit() const
    {
        return e_.clientInit();
    }

    void
    sendNewLedger()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << e_.side() << " session::sendNewLedger()" << std::endl;
        })
        auto const jv = e_.getNewLedger();
        auto const s = to_string(jv);

        ws_.text(true);
        ws_.async_write(
            boost::asio::buffer(s),
            boost::beast::bind_front_handler(
                &session::onWriteNewLedger,
                std::enable_shared_from_this<
                    session<engine>>::shared_from_this()));
    }

    void
    onWriteNewLedger(boost::beast::error_code ec, std::size_t bytes_transferred)
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << " session::onWriteNewLedger(), ec:" << ec
                      << ", bytes: " << bytes_transferred
                      << ", side: " << side() << std::endl;
        })
        boost::ignore_unused(bytes_transferred);
        if (ec == websocket::error::closed)
            return;
        if (ec)
            return fail(ec, "onWriteNewLedger");
    }

    bool
    attSubmitted() const
    {
        return e_.attSubmitted();
    }

    bool
    blobOk() const
    {
        return e_.blobOk();
    }

    std::string
    side() const
    {
        return e_.side();
    }
};

//------------------------------------------------------------------------------

template <class engine>
class listener : public std::enable_shared_from_this<listener<engine>>
{
    boost::asio::io_context& ios_;
    tcp::acceptor acceptor_;
    std::shared_ptr<session<engine>> session_;

public:
    listener(boost::asio::io_context& ioc, tcp::endpoint endpoint)
        : ios_(ioc), acceptor_(ioc)
    {
        boost::beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
            fail(ec, "open");

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec)
            fail(ec, "set_option");

        acceptor_.bind(endpoint, ec);
        if (ec)
            fail(ec, "bind");

        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "listener::listen()"
                      << ", side: " << session_->side() << std::endl;
        })
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
            fail(ec, "listen");
    }

    void
    run()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "listener::run()"
                      << ", side: " << session_->side() << std::endl;
        })
        doAccept();
    }

    void
    shutdown()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "listener::shutdown()"
                      << ", side: " << session_->side() << std::endl;
        })
        boost::system::error_code ec;
        acceptor_.cancel(ec);
        DBG(if (ec) {
            std::lock_guard cl(gMCons);
            std::cout << "cancel error: " << ec
                      << ", side: " << session_->side() << std::endl;
        })
        if (session_)
            session_->shutdown();
    }

    bool
    finished() const
    {
        return session_ ? session_->finished() : true;
    }

    bool
    clientInit() const
    {
        return session_ ? session_->clientInit() : true;
    }

    bool
    attSubmitted() const
    {
        return session_ ? session_->attSubmitted() : false;
    };

    bool
    blobOk() const
    {
        return session_ ? session_->blobOk() : false;
    };

private:
    void
    doAccept()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "listener::doAccept()"
                      << ", side: " << session_->side() << std::endl;
        })
        acceptor_.async_accept(
            boost::asio::make_strand(ios_),
            boost::beast::bind_front_handler(
                &listener::onAccept,
                std::enable_shared_from_this<
                    listener<engine>>::shared_from_this()));
    }

    void
    onAccept(boost::beast::error_code ec, tcp::socket socket)
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "listener::onAccept(), ec:" << ec
                      << ", side: " << session_->side() << std::endl;
        })
        if (ec == boost::system::errc::operation_canceled)
            return;
        if (ec)
            fail(ec, "accept");

        session_ = std::make_shared<session<engine>>(ios_, std::move(socket));
        session_->run();

        doAccept();
    }
};

struct Connection
{
    boost::asio::io_service ios_;
    std::vector<std::thread> ioThreads_;
    std::shared_ptr<listener<engineLoc>> serverLoc_;
    std::shared_ptr<listener<engineIss>> serverIss_;

    Connection() : ios_(NUM_THREADS)
    {
    }

    ~Connection()
    {
        shutdownServers();
        waitIOThreads();
    }

    void
    startIOThreads()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "Connection::startIOThreads()" << std::endl;
        })
        ioThreads_.reserve(NUM_THREADS);
        for (auto i = 0; i < NUM_THREADS; ++i)
            ioThreads_.emplace_back([this, i] {
                ios_.run();
                DBG({
                    std::lock_guard cl(gMCons);
                    std::cout << "ioThread #" << i << " finished" << std::endl;
                })
            });
    }

    void
    waitIOThreads()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "Connection::waitIOThreads()" << std::endl;
        })
        for (auto& t : ioThreads_)
            if (t.joinable())
                t.join();
        ioThreads_.clear();
    }

    void
    startServers()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "Connection::startServers()" << std::endl;
        })
        auto const address = boost::asio::ip::make_address(LHOST);
        serverLoc_ = std::make_shared<listener<engineLoc>>(
            ios_, tcp::endpoint{address, PORT_LOC});
        serverLoc_->run();
        serverIss_ = std::make_shared<listener<engineIss>>(
            ios_, tcp::endpoint{address, PORT_ISS});
        serverIss_->run();
    }

    void
    shutdownServers()
    {
        DBG({
            std::lock_guard cl(gMCons);
            std::cout << "Connection::shutdownServers()" << std::endl;
        })
        serverLoc_->shutdown();
        serverIss_->shutdown();
        wait_for(1s, [this]() {
            return serverLoc_->finished() && serverIss_->finished();
        } DBG_ARGS("Connection::shutdownServers()"));
        serverLoc_.reset();
        serverIss_.reset();
    }

    bool
    clientInit() const
    {
        return (serverLoc_ ? serverLoc_->clientInit() : false) &&
            (serverIss_ ? serverIss_->clientInit() : false);
    }

    bool
    checkAtt() const
    {
        return (serverIss_ ? serverIss_->attSubmitted() : false) &&
            (serverIss_ ? serverIss_->blobOk() : false);
    }
};

class Main_test : public beast::unit_test::suite
{
public:
    Main_test()
    {
    }

private:
    void
    testConnection()
    {
        testcase("Test simulated rippled connection");

        Connection c;
        c.startServers();
        c.startIOThreads();

        auto const sconf = fmt::format(
            fmt::runtime(prepForFmt(witnessLocal)),
            "Host"_a = LHOST,
            "PortLock"_a = PORT_LOC,
            "PortIss"_a = PORT_ISS,
            "PortRPC"_a = PORT_RPC);
        Json::Value jconf;
        Json::Reader().parse(sconf, jconf);
        std::unique_ptr<xbwd::config::Config> config =
            std::make_unique<xbwd::config::Config>(jconf);
        config->logSilent = DBG(false) REL(true);

        gApp = std::make_unique<xbwd::App>(
            std::move(config), beast::severities::kTrace);
        if (!gApp->setup())
            throw std::runtime_error("Can't setup App");
        gApp->start();

        // wait till server send all the messages
        wait_for(7s, [&c]() {
            return c.clientInit();
        } DBG_ARGS("Wait for App init"));
        BEAST_EXPECT(c.clientInit());

        // wait for witness process all the messages
        std::this_thread::sleep_for(3s);

        gApp->signalStop();
        gApp->stop();

        // wait for the threads finished
        std::this_thread::sleep_for(1s);
        gApp.reset();

        BEAST_EXPECT(c.checkAtt());

        checkSignerList();
        checkInit();
    }

    void
    checkInit()
    {
        std::ifstream logf("witness.log");
        bool locRepl = false, issRepl = false;
        for (std::string l; (!locRepl || !issRepl) && std::getline(logf, l);)
        {
            if (l.find("initSyncDone start replay {\"chainType\": "
                       "\"locking\",") != std::string::npos)
            {
                locRepl = true;
                continue;
            }

            if (l.find("initSyncDone start replay {\"chainType\": "
                       "\"issuing\",") != std::string::npos)
            {
                issRepl = true;
                continue;
            }
        }

        BEAST_EXPECT(locRepl && issRepl);
    }

    void
    checkSignerList()
    {
        std::ifstream logf("witness.log");
        bool loc = false, iss = false;
        for (std::string l; (!loc || !iss) && std::getline(logf, l);)
        {
            if ((l.find("onEvent XChainSignerListSet") != std::string::npos) &&
                (l.find("\"presentInSignerList\":true") != std::string::npos))
            {
                if (l.find("\"ChainType\": \"locking\"") != std::string::npos)
                {
                    loc = true;
                    continue;
                }
                if (l.find("\"ChainType\": \"issuing\"") != std::string::npos)
                {
                    iss = true;
                    continue;
                }
            }
        }

        BEAST_EXPECT(loc && iss);
    }

public:
    void
    run() override
    {
        std::filesystem::remove_all("db");
        std::filesystem::remove("witness.log");
        std::filesystem::create_directory("db");
        testConnection();
        REL(std::filesystem::remove_all("db"));
        REL(std::filesystem::remove("witness.log"));
    }
};

BEAST_DEFINE_TESTSUITE(Main, app, xbwd);

const char witnessLocal[] = R"str(
{
  "LockingChain": {
    "Endpoint": {
      "Host": "`Host`",
      "Port": `PortLock`
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
      "Host": "`Host`",
      "Port": `PortIss`
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
    "Host": "`Host`",
    "Port": `PortRPC`
  },
  "LogFile": "witness.log",
  "LogLevel": "Trace",
  "DBDir": "db",
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

const char serverInfoLoc[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "info" : {
         "build_version" : "2.0.0-b2",
         "complete_ledgers" : "2-5",
         "hostid" : "server",
         "initial_sync_duration_us" : "266894",
         "io_latency_ms" : 1,
         "jq_trans_overflow" : "0",
         "last_close" : {
            "converge_time_s" : 0.1,
            "proposers" : 0
         },
         "load" : {
            "job_types" : [
               {
                  "job_type" : "clientRPC",
                  "peak_time" : 8,
                  "per_second" : 4
               },
               {
                  "in_progress" : 1,
                  "job_type" : "clientWebsocket",
                  "per_second" : 3
               },
               {
                  "job_type" : "SyncReadNode",
                  "per_second" : 4
               },
               {
                  "job_type" : "WriteNode",
                  "per_second" : 2
               }
            ],
            "threads" : 1
         },
         "load_factor" : 1,
         "network_id" : 15755,
         "node_size" : "huge",
         "peer_disconnects" : "0",
         "peer_disconnects_resources" : "0",
         "peers" : 0,
         "ports" : [
            {
               "port" : "5005",
               "protocol" : [ "http" ]
            },
            {
               "port" : "6006",
               "protocol" : [ "ws" ]
            },
            {
               "port" : "6005",
               "protocol" : [ "ws" ]
            }
         ],
         "pubkey_node" : "n9Jjh5jz8aaEzkusgXkm2bAEHjdWKh4f7Jy31pWP59GiEqxhhMHy",
         "pubkey_validator" : "none",
         "server_state" : "full",
         "server_state_duration_us" : "14403358",
         "state_accounting" : {
            "connected" : {
               "duration_us" : "0",
               "transitions" : "0"
            },
            "disconnected" : {
               "duration_us" : "266894",
               "transitions" : "1"
            },
            "full" : {
               "duration_us" : "14403358",
               "transitions" : "1"
            },
            "syncing" : {
               "duration_us" : "0",
               "transitions" : "0"
            },
            "tracking" : {
               "duration_us" : "0",
               "transitions" : "0"
            }
         },
         "time" : "2023-Nov-07 01:51:56.559629 UTC",
         "uptime" : 14,
         "validated_ledger" : {
            "age" : 4,
            "base_fee_xrp" : 1e-05,
            "hash" : "33B5AFF2BDF92AB09311F0E024115BB1DAB1CCD2C5365BF5FF387DE7F2672B17",
            "reserve_base_xrp" : 10,
            "reserve_inc_xrp" : 2,
            "seq" : 5
         },
         "validation_quorum" : 0,
         "validator_list" : {
            "count" : 0,
            "expiration" : "unknown",
            "status" : "unknown"
         }
      }
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char serverInfoIss[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "info" : {
         "build_version" : "2.0.0-b2",
         "complete_ledgers" : "2-4",
         "hostid" : "server",
         "initial_sync_duration_us" : "318223",
         "io_latency_ms" : 1,
         "jq_trans_overflow" : "0",
         "last_close" : {
            "converge_time_s" : 0.1,
            "proposers" : 0
         },
         "load" : {
            "job_types" : [
               {
                  "job_type" : "clientRPC",
                  "peak_time" : 9,
                  "per_second" : 3
               },
               {
                  "in_progress" : 1,
                  "job_type" : "clientWebsocket",
                  "per_second" : 3
               },
               {
                  "job_type" : "SyncReadNode",
                  "per_second" : 9
               },
               {
                  "job_type" : "WriteNode",
                  "per_second" : 2
               }
            ],
            "threads" : 1
         },
         "load_factor" : 1,
         "network_id" : 15756,
         "node_size" : "huge",
         "peer_disconnects" : "0",
         "peer_disconnects_resources" : "0",
         "peers" : 0,
         "ports" : [
            {
               "port" : "5006",
               "protocol" : [ "http" ]
            },
            {
               "port" : "6008",
               "protocol" : [ "ws" ]
            },
            {
               "port" : "6007",
               "protocol" : [ "ws" ]
            }
         ],
         "pubkey_node" : "n9KbiUMZu81vhgQViz92LAVNcfVaac21D9C6pHgchLiiKwvs4Cgu",
         "pubkey_validator" : "none",
         "server_state" : "full",
         "server_state_duration_us" : "13215785",
         "state_accounting" : {
            "connected" : {
               "duration_us" : "0",
               "transitions" : "0"
            },
            "disconnected" : {
               "duration_us" : "318223",
               "transitions" : "1"
            },
            "full" : {
               "duration_us" : "13215785",
               "transitions" : "1"
            },
            "syncing" : {
               "duration_us" : "0",
               "transitions" : "0"
            },
            "tracking" : {
               "duration_us" : "0",
               "transitions" : "0"
            }
         },
         "time" : "2023-Nov-07 01:51:56.559985 UTC",
         "uptime" : 13,
         "validated_ledger" : {
            "age" : 5,
            "base_fee_xrp" : 1e-05,
            "hash" : "AAC587927DF053E451551661681355A1B99777E2D2757312EE5F1CDA8BCDD28E",
            "reserve_base_xrp" : 10,
            "reserve_inc_xrp" : 2,
            "seq" : 4
         },
         "validation_quorum" : 0,
         "validator_list" : {
            "count" : 0,
            "expiration" : "unknown",
            "status" : "unknown"
         }
      }
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char bridgeAccInfoLoc[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account_data" : {
         "Account" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
         "Balance" : "119999970",
         "Flags" : 1048576,
         "LedgerEntryType" : "AccountRoot",
         "OwnerCount" : 2,
         "PreviousTxnID" : "3A435F6888202D15F907ECC46D0989CA8AE8A4063735AAE98EE928EDABBAD1F2",
         "PreviousTxnLgrSeq" : 5,
         "Sequence" : 6,
         "index" : "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
         "signer_lists" : [
            {
               "Flags" : 65536,
               "LedgerEntryType" : "SignerList",
               "OwnerNode" : "0",
               "PreviousTxnID" : "FB8BB4BD34823F975FEB190409F3FD20663822E396B34CEAFDB575283D83BE68",
               "PreviousTxnLgrSeq" : 4,
               "SignerEntries" : [
                  {
                     "SignerEntry" : {
                        "Account" : "rhEBTLJZ56mSr8GgvUUmdnmvBvR6niVsJH",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rhL4hgGQRySsE3Y34WQhJL9yMoBQVRjCKY",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rMUU2sMKm4bmsD7s2rk6YTV8Eg9CutoepF",
                        "SignerWeight" : 1
                     }
                  }
               ],
               "SignerListID" : 0,
               "SignerQuorum" : 4,
               "index" : "8A3C62C72C0DE9DF57D4EBE3F54B1BDD783245598C7C3C1C2197536EF6C82F08"
            }
         ]
      },
      "account_flags" : {
         "defaultRipple" : false,
         "depositAuth" : false,
         "disableMasterKey" : true,
         "disallowIncomingXRP" : false,
         "globalFreeze" : false,
         "noFreeze" : false,
         "passwordSpent" : false,
         "requireAuthorization" : false,
         "requireDestinationTag" : false
      },
      "ledger_current_index" : 6,
      "validated" : false
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char bridgeAccInfoIss[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account_data" : {
         "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
         "Balance" : "99999999899999920",
         "Flags" : 1048576,
         "LedgerEntryType" : "AccountRoot",
         "OwnerCount" : 2,
         "PreviousTxnID" : "0DA74C4F4F014943361A4740A7E2C88E7FA1D5AC5B3C90C35B2BAFCE303345DA",
         "PreviousTxnLgrSeq" : 4,
         "Sequence" : 9,
         "index" : "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8",
         "signer_lists" : [
            {
               "Flags" : 65536,
               "LedgerEntryType" : "SignerList",
               "OwnerNode" : "0",
               "PreviousTxnID" : "C27000030FB226FC7FB1DFF6F11CBA32A4170296A4E87DE227D1B20DE01343BC",
               "PreviousTxnLgrSeq" : 4,
               "SignerEntries" : [
                  {
                     "SignerEntry" : {
                        "Account" : "rhEBTLJZ56mSr8GgvUUmdnmvBvR6niVsJH",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rhL4hgGQRySsE3Y34WQhJL9yMoBQVRjCKY",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                        "SignerWeight" : 1
                     }
                  },
                  {
                     "SignerEntry" : {
                        "Account" : "rMUU2sMKm4bmsD7s2rk6YTV8Eg9CutoepF",
                        "SignerWeight" : 1
                     }
                  }
               ],
               "SignerListID" : 0,
               "SignerQuorum" : 4,
               "index" : "778365D5180F5DF3016817D1F318527AD7410D83F8636CF48C43E8AF72AB49BF"
            }
         ]
      },
      "account_flags" : {
         "defaultRipple" : false,
         "depositAuth" : false,
         "disableMasterKey" : true,
         "disallowIncomingXRP" : false,
         "globalFreeze" : false,
         "noFreeze" : false,
         "passwordSpent" : false,
         "requireAuthorization" : false,
         "requireDestinationTag" : false
      },
      "ledger_current_index" : 5,
      "validated" : false
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char ledgerSubsLoc[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "fee_base" : 10,
      "fee_ref" : 10,
      "ledger_hash" : "33B5AFF2BDF92AB09311F0E024115BB1DAB1CCD2C5365BF5FF387DE7F2672B17",
      "ledger_index" : 5,
      "ledger_time" : 752637112,
      "reserve_base" : 10000000,
      "reserve_inc" : 2000000,
      "validated_ledgers" : "2-5"
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char ledgerSubsIss[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "fee_base" : 10,
      "fee_ref" : 10,
      "ledger_hash" : "AAC587927DF053E451551661681355A1B99777E2D2757312EE5F1CDA8BCDD28E",
      "ledger_index" : 4,
      "ledger_time" : 752637111,
      "reserve_base" : 10000000,
      "reserve_inc" : 2000000,
      "validated_ledgers" : "2-4"
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char ledgerEntryLoc[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "index" : "DF794CFEDA27E06DAE87A6EFB489614C1D3FF857F3980661F84FC01933B4AF6F",
      "ledger_current_index" : 6,
      "node" : {
         "Account" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
         "Flags" : 0,
         "LedgerEntryType" : "Bridge",
         "MinAccountCreateAmount" : "10000000",
         "OwnerNode" : "0",
         "PreviousTxnID" : "C7C94CB45DEE44AF0DA90825EE3552C3B50E51109C4E42921E4F5893ECE12318",
         "PreviousTxnLgrSeq" : 4,
         "SignatureReward" : "100",
         "XChainAccountClaimCount" : "0",
         "XChainAccountCreateCount" : "0",
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
         "XChainClaimID" : "0",
         "index" : "DF794CFEDA27E06DAE87A6EFB489614C1D3FF857F3980661F84FC01933B4AF6F"
      },
      "validated" : false
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char ledgerEntryIss[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "index" : "20C736B81A2632BE6A0FCE130FC3649334A041B79C6017896709F39B6059DB92",
      "ledger_current_index" : 5,
      "node" : {
         "Account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
         "Flags" : 0,
         "LedgerEntryType" : "Bridge",
         "MinAccountCreateAmount" : "10000000",
         "OwnerNode" : "0",
         "PreviousTxnID" : "6DC3CD111E0FBC94E731368A5A485D2E6F36874E4FCAA14D555085700591D358",
         "PreviousTxnLgrSeq" : 4,
         "SignatureReward" : "100",
         "XChainAccountClaimCount" : "0",
         "XChainAccountCreateCount" : "0",
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
         "XChainClaimID" : "0",
         "index" : "20C736B81A2632BE6A0FCE130FC3649334A041B79C6017896709F39B6059DB92"
      },
      "validated" : false
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char accTxLoc[] = R"str(
{
  "id": `id`,
  "jsonrpc": "2.0",
  "result": {
    "account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
    "ledger_index_max": 6,
    "ledger_index_min": 2,
    "limit": 10,
    "transactions": [
      {
        "meta": {
          "AffectedNodes": [
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "Balance": "99999999779999930",
                  "Flags": 0,
                  "OwnerCount": 0,
                  "Sequence": 8
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8",
                "PreviousFields": {
                  "Balance": "99999999879999940",
                  "Sequence": 7
                },
                "PreviousTxnID": "5CCF02CE4CA67BAFDFAED17116BAB2302CD31101C486A44A8692C3C5B984A57C",
                "PreviousTxnLgrSeq": 3
              }
            },
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                  "Balance": "119999970",
                  "Flags": 1048576,
                  "OwnerCount": 2,
                  "Sequence": 6
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
                "PreviousFields": {
                  "Balance": "19999970"
                },
                "PreviousTxnID": "76E22E416937C65CB92E8E2C68CB098BA72D13A7A7538E9B4EE357A5EB44EAD8",
                "PreviousTxnLgrSeq": 4
              }
            }
          ],
          "TransactionIndex": 0,
          "TransactionResult": "tesSUCCESS",
          "delivered_amount": "100000000"
        },
        "tx": {
          "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
          "Amount": "100000000",
          "Destination": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
          "Fee": "10",
          "Flags": 0,
          "LastLedgerSequence": 24,
          "NetworkID": 15755,
          "Sequence": 7,
          "SigningPubKey": "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
          "TransactionType": "Payment",
          "TxnSignature": "304502210089F1D4153BBEE70C3930A2B0E97E962183E9844F9DDE6084923DFE40522C34E60220277927B4E9B12586E6CFDFBC91116FD65E01BE8F60CE2EB1C3D00E5C867DD8AF",
          "date": 752637112,
          "hash": "3A435F6888202D15F907ECC46D0989CA8AE8A4063735AAE98EE928EDABBAD1F2",
          "inLedger": 5,
          "ledger_index": 5
        },
        "validated": true
      },
      {
        "meta": {
          "AffectedNodes": [
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                  "Balance": "19999970",
                  "Flags": 1048576,
                  "OwnerCount": 2,
                  "Sequence": 6
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
                "PreviousFields": {
                  "Balance": "19999980",
                  "Flags": 0,
                  "Sequence": 5
                },
                "PreviousTxnID": "FB8BB4BD34823F975FEB190409F3FD20663822E396B34CEAFDB575283D83BE68",
                "PreviousTxnLgrSeq": 4
              }
            }
          ],
          "TransactionIndex": 2,
          "TransactionResult": "tesSUCCESS"
        },
        "tx": {
          "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
          "Fee": "10",
          "Flags": 0,
          "LastLedgerSequence": 23,
          "NetworkID": 15755,
          "Sequence": 5,
          "SetFlag": 4,
          "SigningPubKey": "02B612A1855D846C504F2B72F6931E31EA2EB695F3F6DB6A2FE64CEE4FB2B023C2",
          "TransactionType": "AccountSet",
          "TxnSignature": "3045022100D3E7148444D8509822B904920798CF09BE4689C1D8D46DD0D1F902D532388B98022013E22680EC4E6E3565CBAE8E8F8892FE5EF963B4D0EFF1F0FC380D184553AD6B",
          "date": 752637111,
          "hash": "76E22E416937C65CB92E8E2C68CB098BA72D13A7A7538E9B4EE357A5EB44EAD8",
          "inLedger": 4,
          "ledger_index": 4
        },
        "validated": true
      },
      {
        "meta": {
          "AffectedNodes": [
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Flags": 0,
                  "Owner": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                  "RootIndex": "3B61CB34EDD84A23167F8C8937AB3B6898E594BA76E08AE489FD1F5EA8F01D13"
                },
                "LedgerEntryType": "DirectoryNode",
                "LedgerIndex": "3B61CB34EDD84A23167F8C8937AB3B6898E594BA76E08AE489FD1F5EA8F01D13"
              }
            },
            {
              "CreatedNode": {
                "LedgerEntryType": "SignerList",
                "LedgerIndex": "8A3C62C72C0DE9DF57D4EBE3F54B1BDD783245598C7C3C1C2197536EF6C82F08",
                "NewFields": {
                  "Flags": 65536,
                  "SignerEntries": [
                    {
                      "SignerEntry": {
                        "Account": "rhEBTLJZ56mSr8GgvUUmdnmvBvR6niVsJH",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rhL4hgGQRySsE3Y34WQhJL9yMoBQVRjCKY",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rMUU2sMKm4bmsD7s2rk6YTV8Eg9CutoepF",
                        "SignerWeight": 1
                      }
                    }
                  ],
                  "SignerQuorum": 4
                }
              }
            },
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                  "Balance": "19999980",
                  "Flags": 0,
                  "OwnerCount": 2,
                  "Sequence": 5
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
                "PreviousFields": {
                  "Balance": "19999990",
                  "OwnerCount": 1,
                  "Sequence": 4
                },
                "PreviousTxnID": "C7C94CB45DEE44AF0DA90825EE3552C3B50E51109C4E42921E4F5893ECE12318",
                "PreviousTxnLgrSeq": 4
              }
            }
          ],
          "TransactionIndex": 1,
          "TransactionResult": "tesSUCCESS"
        },
        "tx": {
          "Account": "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
          "Fee": "10",
          "Flags": 0,
          "LastLedgerSequence": 23,
          "NetworkID": 15755,
          "Sequence": 4,
          "SignerEntries": [
            {
              "SignerEntry": {
                "Account": "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rhL4hgGQRySsE3Y34WQhJL9yMoBQVRjCKY",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rMUU2sMKm4bmsD7s2rk6YTV8Eg9CutoepF",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rhEBTLJZ56mSr8GgvUUmdnmvBvR6niVsJH",
                "SignerWeight": 1
              }
            }
          ],
          "SignerQuorum": 4,
          "SigningPubKey": "02B612A1855D846C504F2B72F6931E31EA2EB695F3F6DB6A2FE64CEE4FB2B023C2",
          "TransactionType": "SignerListSet",
          "TxnSignature": "30440220493E6156A4C3352442D376F8AA12266AE1839EFED2FE6F5918F333895D7116C202207383114FD8CB36F776EFBFED076FB6B481543482BDC6914D0A6260A9F329F094",
          "date": 752637111,
          "hash": "FB8BB4BD34823F975FEB190409F3FD20663822E396B34CEAFDB575283D83BE68",
          "inLedger": 4,
          "ledger_index": 4
        },
        "validated": true
      },
      {
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
        "tx": {
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
        "validated": true
      }
    ]
  }
}
)str";

const char accTxIss[] = R"str(
{
  "id": `id`,
  "jsonrpc": "2.0",
  "result": {
    "account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
    "ledger_index_max": 5,
    "ledger_index_min": 2,
    "limit": 10,
    "transactions": [
      {
        "meta": {
          "AffectedNodes": [
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "Balance": "99999999899999920",
                  "Flags": 1048576,
                  "OwnerCount": 2,
                  "Sequence": 9
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8",
                "PreviousFields": {
                  "Balance": "99999999899999930",
                  "Flags": 0,
                  "Sequence": 8
                },
                "PreviousTxnID": "C27000030FB226FC7FB1DFF6F11CBA32A4170296A4E87DE227D1B20DE01343BC",
                "PreviousTxnLgrSeq": 4
              }
            }
          ],
          "TransactionIndex": 2,
          "TransactionResult": "tesSUCCESS"
        },
        "tx": {
          "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
          "Fee": "10",
          "Flags": 0,
          "LastLedgerSequence": 23,
          "NetworkID": 15756,
          "Sequence": 8,
          "SetFlag": 4,
          "SigningPubKey": "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
          "TransactionType": "AccountSet",
          "TxnSignature": "3045022100EC2A9043BF14F86EF476688BA66F40D0A5E0CA76FF17D60318133816CFCC3ECF02203A215985F42B860A092680AC30F946660C262BC8A67BD835CCEE5C1888144A0F",
          "date": 752637111,
          "hash": "0DA74C4F4F014943361A4740A7E2C88E7FA1D5AC5B3C90C35B2BAFCE303345DA",
          "inLedger": 4,
          "ledger_index": 4
        },
        "validated": true
      },
      {
        "meta": {
          "AffectedNodes": [
            {
              "ModifiedNode": {
                "FinalFields": {
                  "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "Balance": "99999999899999930",
                  "Flags": 0,
                  "OwnerCount": 2,
                  "Sequence": 8
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8",
                "PreviousFields": {
                  "Balance": "99999999899999940",
                  "OwnerCount": 1,
                  "Sequence": 7
                },
                "PreviousTxnID": "6DC3CD111E0FBC94E731368A5A485D2E6F36874E4FCAA14D555085700591D358",
                "PreviousTxnLgrSeq": 4
              }
            },
            {
              "CreatedNode": {
                "LedgerEntryType": "SignerList",
                "LedgerIndex": "778365D5180F5DF3016817D1F318527AD7410D83F8636CF48C43E8AF72AB49BF",
                "NewFields": {
                  "Flags": 65536,
                  "SignerEntries": [
                    {
                      "SignerEntry": {
                        "Account": "rhEBTLJZ56mSr8GgvUUmdnmvBvR6niVsJH",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rhL4hgGQRySsE3Y34WQhJL9yMoBQVRjCKY",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                        "SignerWeight": 1
                      }
                    },
                    {
                      "SignerEntry": {
                        "Account": "rMUU2sMKm4bmsD7s2rk6YTV8Eg9CutoepF",
                        "SignerWeight": 1
                      }
                    }
                  ],
                  "SignerQuorum": 4
                }
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
          "TransactionIndex": 1,
          "TransactionResult": "tesSUCCESS"
        },
        "tx": {
          "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
          "Fee": "10",
          "Flags": 0,
          "LastLedgerSequence": 23,
          "NetworkID": 15756,
          "Sequence": 7,
          "SignerEntries": [
            {
              "SignerEntry": {
                "Account": "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rhL4hgGQRySsE3Y34WQhJL9yMoBQVRjCKY",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rMn34di2ntYxWr8uy8pq7qyprZhshQGquK",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rMUU2sMKm4bmsD7s2rk6YTV8Eg9CutoepF",
                "SignerWeight": 1
              }
            },
            {
              "SignerEntry": {
                "Account": "rhEBTLJZ56mSr8GgvUUmdnmvBvR6niVsJH",
                "SignerWeight": 1
              }
            }
          ],
          "SignerQuorum": 4,
          "SigningPubKey": "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
          "TransactionType": "SignerListSet",
          "TxnSignature": "30440220188A068AFEACD2984AEDB1C407BE317D4024870FC2615272D0418732AB30121602202606557ED5E11FC716248232CB5FC5F51D78995F3C4B9090F2FAC8BD59828E00",
          "date": 752637111,
          "hash": "C27000030FB226FC7FB1DFF6F11CBA32A4170296A4E87DE227D1B20DE01343BC",
          "inLedger": 4,
          "ledger_index": 4
        },
        "validated": true
      },
      {
        "meta": {
          "AffectedNodes": [
            {
              "CreatedNode": {
                "LedgerEntryType": "Bridge",
                "LedgerIndex": "20C736B81A2632BE6A0FCE130FC3649334A041B79C6017896709F39B6059DB92",
                "NewFields": {
                  "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
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
                  "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "Balance": "99999999899999940",
                  "Flags": 0,
                  "OwnerCount": 1,
                  "Sequence": 7
                },
                "LedgerEntryType": "AccountRoot",
                "LedgerIndex": "2B6AC232AA4C4BE41BF49D2459FA4A0347E1B543A4C92FCEE0821C0201E2E9A8",
                "PreviousFields": {
                  "Balance": "99999999899999950",
                  "OwnerCount": 0,
                  "Sequence": 6
                },
                "PreviousTxnID": "44BCA9955703D435AD827FD9D58621B691BD188E01F7961111CD8DA1E9F702F6",
                "PreviousTxnLgrSeq": 3
              }
            },
            {
              "CreatedNode": {
                "LedgerEntryType": "DirectoryNode",
                "LedgerIndex": "D8120FC732737A2CF2E9968FDF3797A43B457F2A81AA06D2653171A1EA635204",
                "NewFields": {
                  "Owner": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
                  "RootIndex": "D8120FC732737A2CF2E9968FDF3797A43B457F2A81AA06D2653171A1EA635204"
                }
              }
            }
          ],
          "TransactionIndex": 0,
          "TransactionResult": "tesSUCCESS"
        },
        "tx": {
          "Account": "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
          "Fee": "10",
          "Flags": 0,
          "LastLedgerSequence": 23,
          "MinAccountCreateAmount": "10000000",
          "NetworkID": 15756,
          "Sequence": 6,
          "SignatureReward": "100",
          "SigningPubKey": "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB32654A313222F7FD020",
          "TransactionType": "XChainCreateBridge",
          "TxnSignature": "304502210086530111359BD30CCCCEF86CA3A643D3156DFA61CA8C2937A3B60F3CB30B83D3022067ED72969BC3976B8A49F91083A9A2CBFDA04FA050B99239588405A413227057",
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
          "hash": "6DC3CD111E0FBC94E731368A5A485D2E6F36874E4FCAA14D555085700591D358",
          "inLedger": 4,
          "ledger_index": 4
        },
        "validated": true
      }
    ]
  }
}
)str";

const char ledgerAdvance[] = R"str(
{
   "fee_base" : 10,
   "fee_ref" : 10,
   "ledger_hash" : "B66BEC6A8C3A3585880B37B600A88B0B280104DCC2F4CCB2DED9C605113FE13E",
   "ledger_index" : `closed_ledger`,
   "ledger_time" : 766541670,
   "reserve_base" : 10000000,
   "reserve_inc" : 2000000,
   "txn_count" : 1,
   "type" : "ledgerClosed",
   "validated_ledgers" : "2-`closed_ledger`"
}
)str";

const char accTxLoc1[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
      "ledger_index_max" : 7,
      "ledger_index_min" : 7,
      "limit" : 10,
      "transactions" : [
         {
            "meta" : {
               "AffectedNodes" : [
                  {
                     "ModifiedNode" : {
                        "FinalFields" : {
                           "Account" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
                           "Balance" : "599999890",
                           "Flags" : 0,
                           "OwnerCount" : 0,
                           "Sequence" : 7
                        },
                        "LedgerEntryType" : "AccountRoot",
                        "LedgerIndex" : "AC1F46A5DDA015695AD00328C2410C718B6EA7CBE9D1525D0D4618C9AA62AC83",
                        "PreviousFields" : {
                           "Balance" : "1000000000",
                           "Sequence" : 6
                        },
                        "PreviousTxnID" : "73D46BE70E67C1FD407EC746091938ED1596CB7FE21D9A7A3F10D78739CB5AE2",
                        "PreviousTxnLgrSeq" : 6
                     }
                  },
                  {
                     "ModifiedNode" : {
                        "FinalFields" : {
                           "Account" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                           "Flags" : 0,
                           "MinAccountCreateAmount" : "10000000",
                           "OwnerNode" : "0",
                           "SignatureReward" : "100",
                           "XChainAccountClaimCount" : "0",
                           "XChainAccountCreateCount" : "1",
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
                           "XChainClaimID" : "0"
                        },
                        "LedgerEntryType" : "Bridge",
                        "LedgerIndex" : "DF794CFEDA27E06DAE87A6EFB489614C1D3FF857F3980661F84FC01933B4AF6F",
                        "PreviousFields" : {
                           "XChainAccountCreateCount" : "0"
                        },
                        "PreviousTxnID" : "C7C94CB45DEE44AF0DA90825EE3552C3B50E51109C4E42921E4F5893ECE12318",
                        "PreviousTxnLgrSeq" : 4
                     }
                  },
                  {
                     "ModifiedNode" : {
                        "FinalFields" : {
                           "Account" : "rL9vUaa9eBas32C5bgv4fEmHDfJr3oNd4D",
                           "Balance" : "520000070",
                           "Flags" : 1048576,
                           "OwnerCount" : 2,
                           "Sequence" : 6
                        },
                        "LedgerEntryType" : "AccountRoot",
                        "LedgerIndex" : "F3AE029E77CEDB5C5591623B17B938AE9DCC4AF41F13A87A3B52CE926F28C978",
                        "PreviousFields" : {
                           "Balance" : "119999970"
                        },
                        "PreviousTxnID" : "3A435F6888202D15F907ECC46D0989CA8AE8A4063735AAE98EE928EDABBAD1F2",
                        "PreviousTxnLgrSeq" : 5
                     }
                  }
               ],
               "TransactionIndex" : 0,
               "TransactionResult" : "tesSUCCESS"
            },
            "tx" : {
               "Account" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
               "Amount" : "400000000",
               "Destination" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
               "Fee" : "10",
               "Flags" : 0,
               "LastLedgerSequence" : 26,
               "NetworkID" : 15755,
               "Sequence" : 6,
               "SignatureReward" : "100",
               "SigningPubKey" : "ED77ABF9CA5FB605455ECEC3821246528BD6E822EDFFDC445969DC51C7D036FDF2",
               "TransactionType" : "XChainAccountCreateCommit",
               "TxnSignature" : "B46BA906621F4A7E7512C278ABC58DE163A0FA235D547169446813A26712A47F5B17C8649FB696B712305B987E97B62BAB35799FF2DD96FF0428D9C37A61FE04",
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
               "date" : 752637130,
               "hash" : "A6F65C3C92BD077249C2320FEFC2214B39FDA8DAEF92715EBCF7CA4EC5364EE2",
               "inLedger" : 7,
               "ledger_index" : 7
            },
            "validated" : true
         }
      ],
      "validated" : true
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char accTxLoc2[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
      "ledger_index_max" : 7,
      "ledger_index_min" : 7,
      "limit" : 10,
      "transactions" : [],
      "validated" : true
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char accInfoIss1[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account_data" : {
         "Account" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
         "Balance" : "20000000",
         "Flags" : 0,
         "LedgerEntryType" : "AccountRoot",
         "OwnerCount" : 0,
         "PreviousTxnID" : "5525ECDB5DBDF014AEA4FB02EEAF8ED69E3E63D4AC32C1DB949DD09E5D8FFC68",
         "PreviousTxnLgrSeq" : 3,
         "Sequence" : 3,
         "index" : "B5160B5E8CE0D4158E080630898C4342CB192C211A633A60977A6E8F99DE8A62"
      },
      "account_flags" : {
         "defaultRipple" : false,
         "depositAuth" : false,
         "disableMasterKey" : false,
         "disallowIncomingXRP" : false,
         "globalFreeze" : false,
         "noFreeze" : false,
         "passwordSpent" : false,
         "requireAuthorization" : false,
         "requireDestinationTag" : false
      },
      "ledger_hash" : "AAC587927DF053E451551661681355A1B99777E2D2757312EE5F1CDA8BCDD28E",
      "ledger_index" : 4,
      "validated" : true
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char accTxIss1[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account" : "rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh",
      "ledger_index_max" : 6,
      "ledger_index_min" : 6,
      "limit" : 10,
      "transactions" : [],
      "validated" : true
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char accTxIss2[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "account" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
      "ledger_index_max" : 6,
      "ledger_index_min" : 6,
      "limit" : 10,
      "transactions" : [],
      "validated" : true
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

const char submIss[] = R"str(
{
   "id" : `id`,
   "jsonrpc" : "2.0",
   "result" : {
      "accepted" : true,
      "account_sequence_available" : 4,
      "account_sequence_next" : 4,
      "applied" : true,
      "broadcast" : true,
      "engine_result" : "tesSUCCESS",
      "engine_result_code" : 0,
      "engine_result_message" : "The transaction was applied. Only final in a validated ledger.",
      "kept" : true,
      "open_ledger_cost" : "10",
      "queued" : false,
      "tx_blob" : "12002E2100003D8C2400000003201B0000000930150000000000000001614000000017D78400684000000000000014601D40000000000000647121EDD6EBD41B688433A1E61B9F08C826EDF705797D4A0C77BBB6AB032A78337182B77321ED78A84396829B51E6C2571C05C5C4EA54D9FFAA694B39AC05F5A43434DB0F9D26744081B0559BC2254D88E06219EDD73C172A9F41915D09D8EE3481D0D69ECCAFD3F7B8A90F71588DCC6FB173E66550D235FAF7F332BCF670EDCE6CB32E64E1B24F047640D9663120AA06901D2BA6D4E624E09E2C0B9346A439708642F7C032A94FCEDDB34979D8F001F7AC14068123FBA914098D6DEB832131C16CEA4D9A26301F5E3D0981142C5F3B8A6AD6EC14FEE24E74EA0D699BE0ED52238314B31A3FA9E795D6873F4556AD54A80FC633B96152801214B31A3FA9E795D6873F4556AD54A80FC633B96152801414301C4BD495CBD37626FDE4BA0C10CA0DC4D43C4E8015142C5F3B8A6AD6EC14FEE24E74EA0D699BE0ED522300101301011914D21A1E8CDE618534083C814882EFE416EF94949D000000000000000000000000000000000000000014B5F762798A53D543A014CAF8B297CFF8F2F937E80000000000000000000000000000000000000000",
      "tx_json" : {
         "Account" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
         "Amount" : "400000000",
         "AttestationRewardAccount" : "rnscFKLtPLn9MnUZh8EHi2KEnJR6qcZXWg",
         "AttestationSignerAccount" : "rnPPLGGKZjNe9S5L9238XU7erRTjt6P9aS",
         "Destination" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
         "Fee" : "20",
         "LastLedgerSequence" : 9,
         "NetworkID" : 15756,
         "OtherChainSource" : "rHLrQ3SjzxmkoYgrZ5d4kgHRPF6MdMWpAV",
         "PublicKey" : "EDD6EBD41B688433A1E61B9F08C826EDF705797D4A0C77BBB6AB032A78337182B7",
         "Sequence" : 3,
         "Signature" : "D9663120AA06901D2BA6D4E624E09E2C0B9346A439708642F7C032A94FCEDDB34979D8F001F7AC14068123FBA914098D6DEB832131C16CEA4D9A26301F5E3D09",
         "SignatureReward" : "100",
         "SigningPubKey" : "ED78A84396829B51E6C2571C05C5C4EA54D9FFAA694B39AC05F5A43434DB0F9D26",
         "TransactionType" : "XChainAddAccountCreateAttestation",
         "TxnSignature" : "81B0559BC2254D88E06219EDD73C172A9F41915D09D8EE3481D0D69ECCAFD3F7B8A90F71588DCC6FB173E66550D235FAF7F332BCF670EDCE6CB32E64E1B24F04",
         "WasLockingChainSend" : 1,
         "XChainAccountCreateCount" : "1",
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
         "hash" : "5AF3AFC3156C56B982794572C6E74ED55325865BB7F404DD3F71329539A63CE2"
      },
      "validated_ledger_index" : 5
   },
   "ripplerpc" : "2.0",
   "status" : "success",
   "type" : "response"
}
)str";

}  // namespace all
}  // namespace tests
}  // namespace xbwd
