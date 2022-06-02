#include "RPCServer.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <iostream>
#include <memory>

using boost::asio::ip::tcp;

namespace ripple {
namespace sidechain {

class session : public std::enable_shared_from_this<session>
{
public:
    explicit session(boost::asio::io_context& io_context,
                     AttnServer &attn_server,
                     tcp::socket socket)
        : attn_server_(attn_server),
          socket_(std::move(socket)),
          timer_(io_context),
          strand_(io_context.get_executor())
    {
    }

    void go()
    {
        auto self(shared_from_this());
        boost::asio::spawn(strand_, [this, self](boost::asio::yield_context yield) {
            try
            {
                constexpr size_t max_size = 8192;
                char data[max_size];
                for (;;)
                {
                    timer_.expires_from_now(std::chrono::seconds(10));
                    std::size_t n = socket_.async_read_some(boost::asio::buffer(data), yield);

                    assert(n < max_size);
                    auto resp = attn_server_.process_rpc_request(std::string_view(data, n));

                    boost::asio::async_write(socket_, boost::asio::buffer(resp), yield);
                }
            }
            catch (std::exception& e)
            {
                std::cerr << e.what() << "\n";
                socket_.close();
                timer_.cancel();
            }
        });

        boost::asio::spawn(strand_, [this, self](boost::asio::yield_context yield) {
            while (socket_.is_open())
            {
                boost::system::error_code ignored_ec;
                timer_.async_wait(yield[ignored_ec]);
                if (timer_.expires_from_now() <= std::chrono::seconds(0))
                    socket_.close();
            }
        });
    }

private:
    AttnServer &attn_server_;
    tcp::socket socket_;
    boost::asio::steady_timer timer_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
};


RPCServer::RPCServer(boost::asio::io_context& ioc, AttnServer &attn_server)
{
    auto const address = boost::asio::ip::make_address("0.0.0.0");
    unsigned short const port = 8000;

    // Spawn a listening port
    boost::asio::spawn(ioc, [&](boost::asio::yield_context yield) {
        
        tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), port));

        for (;;)
        {
            boost::system::error_code ec;
            tcp::socket socket(ioc);
            acceptor.async_accept(socket, yield[ec]);
            if (!ec)
            {
                std::make_shared<session>(ioc, attn_server, std::move(socket))->go();
            }
        }
    });
}

    
}
}
