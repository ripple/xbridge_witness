#pragma once

#include <ripple/json/Output.h>
#include <ripple/server/Server.h>
#include <ripple/server/Session.h>
#include <ripple/server/WSSession.h>

#include <boost/asio/io_service.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/utility/string_view.hpp>

#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

namespace xbwd {

class App;

namespace rpc {

struct ComparePorts
{
    bool
    operator()(ripple::Port const& lhs, ripple::Port const& rhs) const
    {
        return lhs.name < rhs.name;
    }
};

class ServerHandler
{
    using socket_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<socket_type>;

    App& app_;
    std::unique_ptr<ripple::Server> server_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_{false};
    std::map<std::reference_wrapper<ripple::Port const>, int, ComparePorts>
        count_;
    boost::asio::thread_pool threadPool_;
    beast::Journal j_;

public:
    ServerHandler(
        App& app,
        boost::asio::io_service& io_service,
        beast::Journal j);

    ~ServerHandler();

    bool
    setup(std::vector<ripple::Port> const& ports);

    using Output = Json::Output;

    void
    stop();

    //
    // Handler
    //

    bool
    onAccept(ripple::Session& session, boost::asio::ip::tcp::endpoint endpoint);

    ripple::Handoff
    onHandoff(
        ripple::Session& session,
        std::unique_ptr<stream_type>&& bundle,
        ripple::http_request_type&& request,
        boost::asio::ip::tcp::endpoint const& remote_address);

    ripple::Handoff
    onHandoff(
        ripple::Session& session,
        ripple::http_request_type&& request,
        boost::asio::ip::tcp::endpoint const& remote_address);

    void
    onRequest(ripple::Session& session);

    void
    onWSMessage(
        std::shared_ptr<ripple::WSSession> session,
        std::vector<boost::asio::const_buffer> const& buffers);

    void
    onClose(ripple::Session& session, boost::system::error_code const&);

    void
    onStopped(ripple::Server&);

private:
    Json::Value
    processSession(
        std::shared_ptr<ripple::WSSession> const& session,
        Json::Value const& jv);

    void
    processSession(std::shared_ptr<ripple::Session> const&);

    void
    processRequest(
        ripple::Port const& port,
        std::string const& request,
        beast::IP::Endpoint const& remoteIPAddress,
        Output&&,
        boost::string_view forwardedFor,
        boost::string_view user);

    ripple::Handoff
    statusResponse(ripple::http_request_type const& request) const;
};

}  // namespace rpc
}  // namespace xbwd
