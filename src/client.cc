#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <iostream>

// Logger for the client
seastar::logger clogger("client");

namespace bpo = boost::program_options;

int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()
        ("host", bpo::value<std::string>()->default_value("127.0.0.1"), "Server IP or host")
        ("port", bpo::value<uint16_t>()->default_value(12345), "Server port");

    return app.run(argc, argv, [&app]() -> seastar::future<> {
        auto& config = app.configuration();
        auto host = config["host"].as<std::string>();
        auto port = config["port"].as<uint16_t>();

        seastar::ipv4_addr server_addr(host, port);
        clogger.info("Connecting to {}:{}", host, port);

        // Connect to the server
        return seastar::connect(server_addr).then([](seastar::connected_socket socket) {
            clogger.info("Connected to server.");
            auto read_buf = socket.input();
            auto write_buf = socket.output();

            std::string client_hello = "HelloSrv! "; // Server expects 10 bytes
            // The original code padded with spaces if shorter, ensuring 10 bytes.
            // "HelloSrv! " is exactly 10 bytes.
            // If you change client_hello to something not 10 bytes, adjust server's read_exactly(10) or pad here.


            clogger.info("Sending message: '{}'", client_hello);
            // Send a message to the server
            return write_buf.write(client_hello).then([write_buf]() mutable {
                return write_buf.flush();
            }).then([read_buf, write_buf, socket]() mutable {
                // Read the response from the server
                // The server sends "Hola Cliente!" which is 13 bytes
                return read_buf.read_exactly(13).then([read_buf, write_buf, socket] (seastar::temporary_buffer<char> server_msg_buf) mutable {
                    if (server_msg_buf.empty()) {
                        clogger.warn("Server disconnected or did not send a complete response.");
                        return seastar::make_ready_future<>();
                    }
                    std::string server_msg(server_msg_buf.get(), server_msg_buf.size());
                    clogger.info("Response received from server: '{}'", server_msg);
                    
                    // Close streams and socket
                    return write_buf.close().then([read_buf]() mutable {
                        return read_buf.close();
                    }).finally([socket]() mutable {
                        // socket.shutdown_output(); // Optional, close() on output_stream already does it.
                        // socket.shutdown_input();  // Optional, close() on input_stream already does it.
                    });
                });
            });
        }).handle_exception([](std::exception_ptr e) {
            clogger.error("Client error: {}", e);
            seastar::engine().exit(1); // Exit with error
        }).then([] {
            clogger.info("Client finished.");
            seastar::engine().exit(0); // Exit successfully
        });
    });
}
