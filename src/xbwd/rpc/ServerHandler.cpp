//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <xbwd/rpc/ServerHandler.h>

#include <xbwd/app/BuildInfo.h>
#include <xbwd/rpc/RPCHandler.h>

#include <ripple/basics/Log.h>
#include <ripple/basics/base64.h>
#include <ripple/basics/contract.h>
#include <ripple/basics/make_SSLContext.h>
#include <ripple/basics/safe_cast.h>
#include <ripple/beast/core/LexicalCast.h>
#include <ripple/beast/net/IPAddressConversion.h>
#include <ripple/beast/rfc2616.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/json/to_string.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/RPCErr.h>
#include <ripple/protocol/jss.h>
#include <ripple/resource/Fees.h>
#include <ripple/server/SimpleWriter.h>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/type_traits.hpp>

#include <algorithm>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace {
boost::string_view
extractIpAddrFromField(boost::string_view field)
{
    // Lambda to trim leading and trailing spaces on the field.
    auto trim = [](boost::string_view str) -> boost::string_view {
        boost::string_view ret = str;

        // Only do the work if there's at least one leading space.
        if (!ret.empty() && ret.front() == ' ')
        {
            std::size_t const firstNonSpace = ret.find_first_not_of(' ');
            if (firstNonSpace == boost::string_view::npos)
                // We know there's at least one leading space.  So if we got
                // npos, then it must be all spaces.  Return empty string_view.
                return {};

            ret = ret.substr(firstNonSpace);
        }
        // Trim trailing spaces.
        if (!ret.empty())
        {
            // Only do the work if there's at least one trailing space.
            if (unsigned char const c = ret.back();
                c == ' ' || c == '\r' || c == '\n')
            {
                std::size_t const lastNonSpace = ret.find_last_not_of(" \r\n");
                if (lastNonSpace == boost::string_view::npos)
                    // We know there's at least one leading space.  So if we
                    // got npos, then it must be all spaces.
                    return {};

                ret = ret.substr(0, lastNonSpace + 1);
            }
        }
        return ret;
    };

    boost::string_view ret = trim(field);
    if (ret.empty())
        return {};

    // If there are surrounding quotes, strip them.
    if (ret.front() == '"')
    {
        ret.remove_prefix(1);
        if (ret.empty() || ret.back() != '"')
            return {};  // Unbalanced double quotes.

        ret.remove_suffix(1);

        // Strip leading and trailing spaces that were inside the quotes.
        ret = trim(ret);
    }
    if (ret.empty())
        return {};

    // If we have an IPv6 or IPv6 (dual) address wrapped in square brackets,
    // then we need to remove the square brackets.
    if (ret.front() == '[')
    {
        // Remove leading '['.
        ret.remove_prefix(1);

        // We may have an IPv6 address in square brackets.  Scan up to the
        // closing square bracket.
        auto const closeBracket =
            std::find_if_not(ret.begin(), ret.end(), [](unsigned char c) {
                return std::isxdigit(c) || c == ':' || c == '.' || c == ' ';
            });

        // If the string does not close with a ']', then it's not valid IPv6
        // or IPv6 (dual).
        if (closeBracket == ret.end() || (*closeBracket) != ']')
            return {};

        // Remove trailing ']'
        ret = ret.substr(0, closeBracket - ret.begin());
        ret = trim(ret);
    }
    if (ret.empty())
        return {};

    // If this is an IPv6 address (after unwrapping from square brackets),
    // then there cannot be an appended port.  In that case we're done.
    {
        // Skip any leading hex digits.
        auto const colon =
            std::find_if_not(ret.begin(), ret.end(), [](unsigned char c) {
                return std::isxdigit(c) || c == ' ';
            });

        // If the string starts with optional hex digits followed by a colon
        // it's an IVv6 address.  We're done.
        if (colon == ret.end() || (*colon) == ':')
            return ret;
    }

    // If there's a port appended to the IP address, strip that by
    // terminating at the colon.
    if (std::size_t colon = ret.find(':'); colon != boost::string_view::npos)
        ret = ret.substr(0, colon);

    return ret;
}

std::string
getHTTPHeaderTimestamp()
{
    // CHECKME This is probably called often enough that optimizing it makes
    //         sense. There's no point in doing all this work if this function
    //         gets called multiple times a second.
    std::array<char, 96> buffer{};
    time_t now;
    time(&now);
    struct tm now_gmt
    {
    };
#ifndef _MSC_VER
    gmtime_r(&now, &now_gmt);
#else
    gmtime_s(&now_gmt, &now);
#endif
    strftime(
        buffer.data(),
        buffer.size(),
        "Date: %a, %d %b %Y %H:%M:%S +0000\r\n",
        &now_gmt);
    return buffer.data();
}

void
HTTPReply(
    int nStatus,
    Json::Value&& jcontent,
    Json::Output const& output,
    beast::Journal j)
{
    auto const content = to_string(jcontent) + '\n';
    auto const level = jcontent.isMember(ripple::jss::error) &&
            jcontent[ripple::jss::error].isString() &&
            (jcontent[ripple::jss::error].asString() == "internalError")
        ? j.warn()
        : j.trace();
    JLOG(level) << "HTTP Reply " << nStatus << " " << content;

    assert(!jcontent[ripple::jss::result].isMember(ripple::jss::result));

    if (nStatus == 401)
    {
        output("HTTP/1.0 401 Authorization Required\r\n");
        output(getHTTPHeaderTimestamp());

        // CHECKME this returns a different version than the replies below. Is
        //         this by design or an accident or should it be using
        //         BuildInfo::getFullVersionString () as well?
        output(
            "Server: " + std::string(xbwd::build_info::serverName) +
            "-json-rpc/v1");
        output("\r\n");

        // Be careful in modifying this! If you change the contents you MUST
        // update the Content-Length header as well to indicate the correct
        // size of the data.
        output(
            "WWW-Authenticate: Basic realm=\"jsonrpc\"\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 296\r\n"
            "\r\n"
            "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 "
            "Transitional//EN\"\r\n"
            "\"http://www.w3.org/TR/1999/REC-html401-19991224/loose.dtd"
            "\">\r\n"
            "<HTML>\r\n"
            "<HEAD>\r\n"
            "<TITLE>Error</TITLE>\r\n"
            "<META HTTP-EQUIV='Content-Type' "
            "CONTENT='text/html; charset=ISO-8859-1'>\r\n"
            "</HEAD>\r\n"
            "<BODY><H1>401 Unauthorized.</H1></BODY>\r\n");

        return;
    }

    switch (nStatus)
    {
        case 200:
            output("HTTP/1.1 200 OK\r\n");
            break;
        case 400:
            output("HTTP/1.1 400 Bad Request\r\n");
            break;
        case 403:
            output("HTTP/1.1 403 Forbidden\r\n");
            break;
        case 404:
            output("HTTP/1.1 404 Not Found\r\n");
            break;
        case 500:
            output("HTTP/1.1 500 Internal Server Error\r\n");
            break;
        case 503:
            output("HTTP/1.1 503 Server is overloaded\r\n");
            break;
    }

    output(getHTTPHeaderTimestamp());

    output(
        "Connection: Keep-Alive\r\n"
        "Content-Length: ");

    // VFALCO TODO Determine if/when this header should be added
    // if (context.app.config().RPC_ALLOW_REMOTE)
    //    output ("Access-Control-Allow-Origin: *\r\n");

    output(std::to_string(content.size() + 2));
    output(
        "\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n");

    output(
        "Server: " + std::string(xbwd::build_info::serverName) + "-json-rpc/");
    output(xbwd::build_info::getFullVersionString());
    output(
        "\r\n"
        "\r\n");
    output(content);
    output("\r\n");
}

bool
isStatusRequest(ripple::http_request_type const& request)
{
    return request.version() >= 11 && request.target() == "/" &&
        request.body().size() == 0 &&
        request.method() == boost::beast::http::verb::get;
}

ripple::Handoff
statusRequestResponse(
    ripple::http_request_type const& request,
    boost::beast::http::status status)
{
    using namespace boost::beast::http;
    ripple::Handoff handoff;
    response<string_body> msg;
    msg.version(request.version());
    msg.result(status);
    msg.insert("Server", xbwd::build_info::getFullVersionString());
    msg.insert("Content-Type", "text/html");
    msg.insert("Connection", "close");
    msg.body() = "Invalid protocol.";
    msg.prepare_payload();
    handoff.response = std::make_shared<ripple::SimpleWriter>(msg);
    return handoff;
}

bool
authorized(
    ripple::Port const& port,
    std::map<std::string, std::string> const& h)
{
    if (port.user.empty() || port.password.empty())
        return true;

    auto const it = h.find("authorization");
    if ((it == h.end()) || (it->second.substr(0, 6) != "Basic "))
        return false;
    std::string strUserPass64 = it->second.substr(6);
    boost::trim(strUserPass64);
    std::string strUserPass = ripple::base64_decode(strUserPass64);
    std::string::size_type nColon = strUserPass.find(":");
    if (nColon == std::string::npos)
        return false;
    std::string strUser = strUserPass.substr(0, nColon);
    std::string strPassword = strUserPass.substr(nColon + 1);

    unsigned const origAuthSize = port.user.size() + port.password.size();
    unsigned const inAuthSize = strUser.size() + strPassword.size();
    unsigned const toAllocateSize =
        std::max(origAuthSize, inAuthSize) + 1;  // +1 for \n separator

    std::string inAuthData(toAllocateSize, char()),
        origAuthData(toAllocateSize, char());
    inAuthData = strUser + '\n' + strPassword;
    origAuthData = port.user + '\n' + port.password;

    // +2 == +1 for \n separator and +1 for \0 comparison
    bool const dataMatch = !CRYPTO_memcmp(
        inAuthData.c_str(), origAuthData.c_str(), origAuthSize + 2);

    return dataMatch;
}
}  // namespace
// TODO Remove this - it's a hack to get around linker errors
namespace ripple {

boost::string_view
forwardedFor(ripple::http_request_type const& request)
{
    // Look for the Forwarded field in the request.
    if (auto it = request.find(boost::beast::http::field::forwarded);
        it != request.end())
    {
        auto ascii_tolower = [](char c) -> char {
            return ((static_cast<unsigned>(c) - 65U) < 26) ? c + 'a' - 'A' : c;
        };

        // Look for the first (case insensitive) "for="
        static std::string const forStr{"for="};
        char const* found = std::search(
            it->value().begin(),
            it->value().end(),
            forStr.begin(),
            forStr.end(),
            [&ascii_tolower](char c1, char c2) {
                return ascii_tolower(c1) == ascii_tolower(c2);
            });

        if (found == it->value().end())
            return {};

        found += forStr.size();

        // We found a "for=".  Scan for the end of the IP address.
        std::size_t const pos = [&found, &it]() {
            std::size_t pos =
                boost::string_view(found, it->value().end() - found)
                    .find_first_of(",;");
            if (pos != boost::string_view::npos)
                return pos;

            return it->value().size() - forStr.size();
        }();

        return extractIpAddrFromField({found, pos});
    }

    // Look for the X-Forwarded-For field in the request.
    if (auto it = request.find("X-Forwarded-For"); it != request.end())
    {
        // The first X-Forwarded-For entry may be terminated by a comma.
        std::size_t found = it->value().find(',');
        if (found == boost::string_view::npos)
            found = it->value().length();
        return extractIpAddrFromField(it->value().substr(0, found));
    }

    return {};
}
bool
Port::secure() const
{
    return protocol.count("peer") > 0 || protocol.count("https") > 0 ||
        protocol.count("wss") > 0 || protocol.count("wss2") > 0;
}

std::string
Port::protocols() const
{
    std::string s;
    for (auto iter = protocol.cbegin(); iter != protocol.cend(); ++iter)
        s += (iter != protocol.cbegin() ? "," : "") + *iter;
    return s;
}

std::ostream&
operator<<(std::ostream& os, Port const& p)
{
    os << "'" << p.name << "' (ip=" << p.ip << ":" << p.port << ", ";

    if (p.admin_nets_v4.size() || p.admin_nets_v6.size())
    {
        os << "admin nets:";
        for (auto const& net : p.admin_nets_v4)
        {
            os << net.to_string();
            os << ", ";
        }
        for (auto const& net : p.admin_nets_v6)
        {
            os << net.to_string();
            os << ", ";
        }
    }

    if (p.secure_gateway_nets_v4.size() || p.secure_gateway_nets_v6.size())
    {
        os << "secure_gateway nets:";
        for (auto const& net : p.secure_gateway_nets_v4)
        {
            os << net.to_string();
            os << ", ";
        }
        for (auto const& net : p.secure_gateway_nets_v6)
        {
            os << net.to_string();
            os << ", ";
        }
    }

    os << p.protocols() << ")";
    return os;
}

//------------------------------------------------------------------------------

static void
populate(
    Section const& section,
    std::string const& field,
    std::ostream& log,
    std::vector<boost::asio::ip::network_v4>& nets4,
    std::vector<boost::asio::ip::network_v6>& nets6)
{
    auto const optResult = section.get(field);
    if (!optResult)
        return;

    std::stringstream ss(*optResult);
    std::string ip;

    while (std::getline(ss, ip, ','))
    {
        boost::algorithm::trim(ip);
        bool v4;
        boost::asio::ip::network_v4 v4Net;
        boost::asio::ip::network_v6 v6Net;

        try
        {
            // First, check to see if 0.0.0.0 or ipv6 equivalent was configured,
            // which means all IP addresses.
            auto const addr = beast::IP::Endpoint::from_string_checked(ip);
            if (addr)
            {
                if (is_unspecified(*addr))
                {
                    nets4.push_back(
                        boost::asio::ip::make_network_v4("0.0.0.0/0"));
                    nets6.push_back(boost::asio::ip::make_network_v6("::/0"));
                    // No reason to allow more IPs--it would be redundant.
                    break;
                }

                // The configured address is a single IP (or else addr would
                // be unset). We need this to be a subnet, so append
                // the number of network bits to make a subnet of 1,
                // depending on type.
                v4 = addr->is_v4();
                std::string addressString = addr->to_string();
                if (v4)
                {
                    addressString += "/32";
                    v4Net = boost::asio::ip::make_network_v4(addressString);
                }
                else
                {
                    addressString += "/128";
                    v6Net = boost::asio::ip::make_network_v6(addressString);
                }
            }
            else
            {
                // Since addr is empty, assume that the entry is
                // for a subnet which includes trailing /0-32 or /0-128
                // depending on ip type.
                // First, see if it's an ipv4 subnet. If not, try ipv6.
                // If that throws, then there's nothing we can do with
                // the entry.
                try
                {
                    v4Net = boost::asio::ip::make_network_v4(ip);
                    v4 = true;
                }
                catch (boost::system::system_error const&)
                {
                    v6Net = boost::asio::ip::make_network_v6(ip);
                    v4 = false;
                }
            }

            // Confirm that the address entry is the same as the subnet's
            // underlying network address.
            // 10.1.2.3/24 makes no sense. The underlying network address
            // is 10.1.2.0/24.
            if (v4)
            {
                if (v4Net != v4Net.canonical())
                {
                    log << "The configured subnet " << v4Net.to_string()
                        << " is not the same as the network address, which is "
                        << v4Net.canonical().to_string();
                    Throw<std::exception>();
                }
                nets4.push_back(v4Net);
            }
            else
            {
                if (v6Net != v6Net.canonical())
                {
                    log << "The configured subnet " << v6Net.to_string()
                        << " is not the same as the network address, which is "
                        << v6Net.canonical().to_string();
                    Throw<std::exception>();
                }
                nets6.push_back(v6Net);
            }
        }
        catch (boost::system::system_error const& e)
        {
            log << "Invalid value '" << ip << "' for key '" << field << "' in ["
                << section.name() << "]: " << e.what();
            Throw<std::exception>();
        }
    }
}

void
parse_Port(ParsedPort& port, Section const& section, std::ostream& log)
{
    {
        auto const optResult = section.get("ip");
        if (optResult)
        {
            try
            {
                port.ip = boost::asio::ip::address::from_string(*optResult);
            }
            catch (std::exception const&)
            {
                log << "Invalid value '" << *optResult << "' for key 'ip' in ["
                    << section.name() << "]";
                Rethrow();
            }
        }
    }

    {
        auto const optResult = section.get("port");
        if (optResult)
        {
            try
            {
                port.port = beast::lexicalCastThrow<std::uint16_t>(*optResult);

                // Port 0 is not supported
                if (*port.port == 0)
                    Throw<std::exception>();
            }
            catch (std::exception const&)
            {
                log << "Invalid value '" << *optResult << "' for key "
                    << "'port' in [" << section.name() << "]";
                Rethrow();
            }
        }
    }

    {
        auto const optResult = section.get("protocol");
        if (optResult)
        {
            for (auto const& s : beast::rfc2616::split_commas(
                     optResult->begin(), optResult->end()))
                port.protocol.insert(s);
        }
    }

    {
        auto const lim = get(section, "limit", "unlimited");

        if (!boost::iequals(lim, "unlimited"))
        {
            try
            {
                port.limit =
                    safe_cast<int>(beast::lexicalCastThrow<std::uint16_t>(lim));
            }
            catch (std::exception const&)
            {
                log << "Invalid value '" << lim << "' for key "
                    << "'limit' in [" << section.name() << "]";
                Rethrow();
            }
        }
    }

    {
        auto const optResult = section.get("send_queue_limit");
        if (optResult)
        {
            try
            {
                port.ws_queue_limit =
                    beast::lexicalCastThrow<std::uint16_t>(*optResult);

                // Queue must be greater than 0
                if (port.ws_queue_limit == 0)
                    Throw<std::exception>();
            }
            catch (std::exception const&)
            {
                log << "Invalid value '" << *optResult << "' for key "
                    << "'send_queue_limit' in [" << section.name() << "]";
                Rethrow();
            }
        }
        else
        {
            // Default Websocket send queue size limit
            port.ws_queue_limit = 100;
        }
    }

    populate(section, "admin", log, port.admin_nets_v4, port.admin_nets_v6);
    populate(
        section,
        "secure_gateway",
        log,
        port.secure_gateway_nets_v4,
        port.secure_gateway_nets_v6);

    set(port.user, "user", section);
    set(port.password, "password", section);
    set(port.admin_user, "admin_user", section);
    set(port.admin_password, "admin_password", section);
    set(port.ssl_key, "ssl_key", section);
    set(port.ssl_cert, "ssl_cert", section);
    set(port.ssl_chain, "ssl_chain", section);
    set(port.ssl_ciphers, "ssl_ciphers", section);

    port.pmd_options.server_enable =
        section.value_or("permessage_deflate", true);
    port.pmd_options.client_max_window_bits =
        section.value_or("client_max_window_bits", 15);
    port.pmd_options.server_max_window_bits =
        section.value_or("server_max_window_bits", 15);
    port.pmd_options.client_no_context_takeover =
        section.value_or("client_no_context_takeover", false);
    port.pmd_options.server_no_context_takeover =
        section.value_or("server_no_context_takeover", false);
    port.pmd_options.compLevel = section.value_or("compress_level", 8);
    port.pmd_options.memLevel = section.value_or("memory_level", 4);
}

}  // namespace ripple
namespace xbwd {
namespace rpc {

ServerHandler::ServerHandler(
    App& app,
    boost::asio::io_service& io_service,
    beast::Journal j)
    : app_(app)
    , server_(ripple::make_Server(*this, io_service, j))
    , threadPool_(std::thread::hardware_concurrency())
    , j_(j)
{
}

ServerHandler::~ServerHandler()
{
    server_ = nullptr;
}

bool
ServerHandler::setup(std::vector<ripple::Port> const& ports)
{
    server_->ports(ports);
    return true;
}

//------------------------------------------------------------------------------

void
ServerHandler::stop()
{
    threadPool_.join();
    server_->close();
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopped_; });
    }
}

//------------------------------------------------------------------------------

bool
ServerHandler::onAccept(
    ripple::Session& session,
    boost::asio::ip::tcp::endpoint endpoint)
{
    auto const& port = session.port();

    auto const c = [this, &port]() {
        std::lock_guard lock(mutex_);
        return ++count_[port];
    }();

    if (port.limit && c >= port.limit)
    {
        JLOG(j_.trace()) << port.name << " is full; dropping " << endpoint;
        return false;
    }

    return true;
}

ripple::Handoff
ServerHandler::onHandoff(
    ripple::Session& session,
    std::unique_ptr<stream_type>&& bundle,
    ripple::http_request_type&& request,
    boost::asio::ip::tcp::endpoint const& remote_address)
{
    using namespace boost::beast;
    auto const& p{session.port().protocol};
    bool const is_ws{
        p.count("ws") > 0 || p.count("ws2") > 0 || p.count("wss") > 0 ||
        p.count("wss2") > 0};

    if (websocket::is_upgrade(request))
    {
        // TODO
        ripple::Handoff handoff;
        handoff.moved = true;
        return handoff;
    }

    if (is_ws && isStatusRequest(request))
        return statusResponse(request);

    // Otherwise pass to legacy onRequest or websocket
    return {};
}

ripple::Handoff
ServerHandler::onHandoff(
    ripple::Session& session,
    ripple::http_request_type&& request,
    boost::asio::ip::tcp::endpoint const& remote_address)
{
    return onHandoff(
        session,
        {},
        std::forward<ripple::http_request_type>(request),
        remote_address);
}

namespace {
inline Json::Output
makeOutput(ripple::Session& session)
{
    return [&](boost::beast::string_view const& b) {
        session.write(b.data(), b.size());
    };
}

std::map<std::string, std::string>
build_map(boost::beast::http::fields const& h)
{
    std::map<std::string, std::string> c;
    for (auto const& e : h)
    {
        std::string key(e.name_string());
        std::transform(key.begin(), key.end(), key.begin(), [](auto kc) {
            return std::tolower(static_cast<unsigned char>(kc));
        });
        c[key] = std::string(e.value());
    }
    return c;
}

template <class ConstBufferSequence>
std::string
buffers_to_string(ConstBufferSequence const& bs)
{
    using boost::asio::buffer_cast;
    using boost::asio::buffer_size;
    std::string s;
    s.reserve(buffer_size(bs));
    // Use auto&& so the right thing happens whether bs returns a copy or
    // a reference
    for (auto&& b : bs)
        s.append(buffer_cast<char const*>(b), buffer_size(b));
    return s;
}
}  // namespace

void
ServerHandler::onRequest(ripple::Session& session)
{
    // Make sure RPC is enabled on the port
    if (session.port().protocol.count("http") == 0 &&
        session.port().protocol.count("https") == 0)
    {
        HTTPReply(403, "Forbidden", makeOutput(session), j_);
        session.close(true);
        return;
    }

    // Check user/password authorization
    if (!authorized(session.port(), build_map(session.request())))
    {
        HTTPReply(403, "Forbidden", makeOutput(session), j_);
        session.close(true);
        return;
    }

    std::shared_ptr<ripple::Session> detachedSession = session.detach();
    boost::asio::post(threadPool_, [this, detachedSession]() {
        this->processSession(detachedSession);
    });
}

void
ServerHandler::onWSMessage(
    std::shared_ptr<ripple::WSSession> session,
    std::vector<boost::asio::const_buffer> const& buffers)
{
    // TODO
}

void
ServerHandler::onClose(
    ripple::Session& session,
    boost::system::error_code const&)
{
    std::lock_guard lock(mutex_);
    --count_[session.port()];
}

void
ServerHandler::onStopped(ripple::Server&)
{
    std::lock_guard lock(mutex_);
    stopped_ = true;
    condition_.notify_one();
}

Json::Value
ServerHandler::processSession(
    std::shared_ptr<ripple::WSSession> const& session,
    Json::Value const& jv)
{
    // Requests without "method" are invalid.
    Json::Value jr(Json::objectValue);
    try
    {
        if (!jv.isMember(ripple::jss::method) &&
            !jv[ripple::jss::method].isString())
        {
            jr[ripple::jss::type] = ripple::jss::response;
            jr[ripple::jss::status] = ripple::jss::error;
            jr[ripple::jss::error] = ripple::jss::missingCommand;
            jr[ripple::jss::request] = jv;
            if (jv.isMember(ripple::jss::id))
                jr[ripple::jss::id] = jv[ripple::jss::id];
            if (jv.isMember(ripple::jss::jsonrpc))
                jr[ripple::jss::jsonrpc] = jv[ripple::jss::jsonrpc];
            if (jv.isMember(ripple::jss::ripplerpc))
                jr[ripple::jss::ripplerpc] = jv[ripple::jss::ripplerpc];

            return jr;
        }

        rpc::doCommand(
            app_,
            beast::IP::from_asio(session->remote_endpoint().address()),
            jv,
            {},  // TODO password
            jr[ripple::jss::result]);
    }
    catch (std::exception const& ex)
    {
        jr[ripple::jss::result] = ripple::RPC::make_error(ripple::rpcINTERNAL);
        JLOG(j_.error()) << "Exception while processing WS: " << ex.what()
                         << "\n"
                         << "Input JSON: " << Json::Compact{Json::Value{jv}};
    }

    // Currently we will simply unwrap errors returned by the RPC
    // API, in the future maybe we can make the responses
    // consistent.
    //
    // Regularize result. This is duplicate code.
    if (jr[ripple::jss::result].isMember(ripple::jss::error))
    {
        jr = jr[ripple::jss::result];
        jr[ripple::jss::status] = ripple::jss::error;

        auto rq = jv;

        if (rq.isObject())
        {
            if (rq.isMember(ripple::jss::passphrase.c_str()))
                rq[ripple::jss::passphrase.c_str()] = "<masked>";
            if (rq.isMember(ripple::jss::secret.c_str()))
                rq[ripple::jss::secret.c_str()] = "<masked>";
            if (rq.isMember(ripple::jss::seed.c_str()))
                rq[ripple::jss::seed.c_str()] = "<masked>";
            if (rq.isMember(ripple::jss::seed_hex.c_str()))
                rq[ripple::jss::seed_hex.c_str()] = "<masked>";
        }

        jr[ripple::jss::request] = rq;
    }

    if (jv.isMember(ripple::jss::id))
        jr[ripple::jss::id] = jv[ripple::jss::id];
    if (jv.isMember(ripple::jss::jsonrpc))
        jr[ripple::jss::jsonrpc] = jv[ripple::jss::jsonrpc];
    if (jv.isMember(ripple::jss::ripplerpc))
        jr[ripple::jss::ripplerpc] = jv[ripple::jss::ripplerpc];

    jr[ripple::jss::type] = ripple::jss::response;
    return jr;
}

void
ServerHandler::processSession(std::shared_ptr<ripple::Session> const& session)
{
    processRequest(
        session->port(),
        buffers_to_string(session->request().body().data()),
        session->remoteAddress().at_port(0),
        makeOutput(*session),
        ripple::forwardedFor(session->request()),
        [&] {
            auto const iter = session->request().find("X-User");
            if (iter != session->request().end())
                return iter->value();
            return boost::beast::string_view{};
        }());

    if (beast::rfc2616::is_keep_alive(session->request()))
        session->complete();
    else
        session->close(true);
}

void
ServerHandler::processRequest(
    ripple::Port const& port,
    std::string const& request,
    beast::IP::Endpoint const& remoteIPAddress,
    Output&& output,
    boost::string_view forwardedFor,
    boost::string_view user)
{
    Json::Value jsonOrig;

    {
        std::size_t const maxRequestSize = 2048;
        Json::Reader reader;
        if ((request.size() > maxRequestSize) ||
            !reader.parse(request, jsonOrig) || !jsonOrig ||
            !jsonOrig.isObject())
        {
            Json::Value reply(Json::objectValue);
            reply[ripple::jss::result][ripple::jss::error] = "invalidRequest";
            reply[ripple::jss::result][ripple::jss::error_message] =
                "Unable to parse request: " + reader.getFormatedErrorMessages();
            HTTPReply(400, std::move(reply), output, j_);
            return;
        }
    }

    auto const start(std::chrono::high_resolution_clock::now());

    Json::Value const& jsonRPC = jsonOrig;

    if (!jsonRPC.isMember(ripple::jss::method) ||
        !jsonRPC[ripple::jss::method].isString() ||
        !jsonRPC[ripple::jss::method])
    {
        Json::Value reply(Json::objectValue);
        reply[ripple::jss::result][ripple::jss::error] = "invalidRequest";
        reply[ripple::jss::result][ripple::jss::error_message] =
            "Invalid 'method' field";
        HTTPReply(400, std::move(reply), output, j_);
        return;
    }

    // Extract request parameters from the request Json as `params`.
    //
    // If the field "params" is empty, `params` is an empty object.
    //
    // Otherwise, that field must be an array of length 1 (why?)
    // and we take that first entry and validate that it's an object.
    Json::Value params;
    std::optional<std::string> passwordOp;

    params = jsonRPC[ripple::jss::params];
    if (!params)
        params = Json::Value(Json::objectValue);
    else if (!params.isArray() || params.size() != 1)
    {
        Json::Value reply(Json::objectValue);
        reply[ripple::jss::result][ripple::jss::error] = "invalidRequest";
        reply[ripple::jss::result][ripple::jss::error_message] =
            "params unparseable";
        HTTPReply(400, std::move(reply), output, j_);
        return;
    }
    else
    {
        params = std::move(params[0u]);
        if (!params.isObjectOrNull())
        {
            Json::Value reply(Json::objectValue);
            reply[ripple::jss::result][ripple::jss::error] = "invalidRequest";
            reply[ripple::jss::result][ripple::jss::error_message] =
                "params unparseable";
            HTTPReply(400, std::move(reply), output, j_);
            return;
        }
    }
    // note: also mask the password
    passwordOp = [&]() -> std::optional<std::string> {
        if (params.isMember("Password") && params["Password"].isString())
        {
            auto pass = params["Password"].asString();
            params["Password"] = "<masked>";
            return pass;
        }
        return {};
    }();

    // Clear header-assigned values since not positively identified from a
    // secure_gateway.
    forwardedFor.clear();
    user.clear();

    Json::Value const& method = jsonRPC[ripple::jss::method];
    std::string strMethod = method.asString();
    // Provide the JSON-RPC method as the field "command" in the request.
    params[ripple::jss::command] = strMethod;
    JLOG(j_.trace()) << "doRpcCommand:" << strMethod << ":" << params;

    Json::Value result;
    rpc::doCommand(app_, remoteIPAddress, params, passwordOp, result);

    Json::Value reply(Json::objectValue);
    if (result.isMember(ripple::jss::error))
    {
        result[ripple::jss::status] = ripple::jss::error;
        JLOG(j_.debug()) << "rpcError: " << result[ripple::jss::error]
                         << " error_message: "
                         << result[ripple::jss::error_message]
                         << " error_code: " << result[ripple::jss::error_code];
    }
    else
    {
        result[ripple::jss::status] = ripple::jss::success;
    }
    reply[ripple::jss::result] = std::move(result);

    if (params.isMember(ripple::jss::jsonrpc))
        reply[ripple::jss::jsonrpc] = params[ripple::jss::jsonrpc];
    if (params.isMember(ripple::jss::ripplerpc))
        reply[ripple::jss::ripplerpc] = params[ripple::jss::ripplerpc];
    if (params.isMember(ripple::jss::id))
        reply[ripple::jss::id] = params[ripple::jss::id];

    HTTPReply(200, std::move(reply), output, j_);
}

//------------------------------------------------------------------------------

/*  This response is used with load balancing.
    If the server is overloaded, status 500 is reported. Otherwise status 200
    is reported, meaning the server can accept more connections.
*/
ripple::Handoff
ServerHandler::statusResponse(ripple::http_request_type const& request) const
{
    using namespace boost::beast::http;
    ripple::Handoff handoff;
    response<string_body> msg;
    std::string reason;
    // TODO: Check for server status
    msg.result(boost::beast::http::status::ok);
    msg.body() = "<!DOCTYPE html><html><head><title>" +
        std::string(build_info::serverName) +
        " Test page for rippled</title></head><body><h1>" +
        build_info::serverName +
        " Test</h1><p>This page shows rippled http(s) "
        "connectivity is working.</p></body></html>";
    msg.version(request.version());
    msg.insert("Server", build_info::getFullVersionString());
    msg.insert("Content-Type", "text/html");
    msg.insert("Connection", "close");
    msg.prepare_payload();
    handoff.response = std::make_shared<ripple::SimpleWriter>(msg);
    return handoff;
}

//------------------------------------------------------------------------------

}  // namespace rpc
}  // namespace xbwd
