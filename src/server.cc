#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <iostream>

#define DEFAULT_TCP_PORT_VALUE 12345

// Logger para el servidor
seastar::logger slogger("server");

namespace bpo = boost::program_options;

// Clase para manejar cada conexión de cliente
class ConnectionHandler {
    seastar::connected_socket _socket;
    seastar::input_stream<char> _read_buf;
    seastar::output_stream<char> _write_buf;

public:
    ConnectionHandler(seastar::connected_socket&& socket)
        : _socket(std::move(socket)),
          _read_buf(_socket.input()),
          _write_buf(_socket.output()) {}

    // WorkFlow
    seastar::future<> handle() {
        slogger.info("Nueva conexión desde {}", _socket.remote_address());

        // Read Message From Client
        return _read_buf.read_exactly(10).then([this](seastar::temporary_buffer<char> client_msg_buf) {
            if (client_msg_buf.empty()) {
                slogger.info("Cliente {} desconectado (mensaje vacío)", _socket.remote_address());
                return seastar::make_ready_future<>();
            }
            std::string client_msg(client_msg_buf.get(), client_msg_buf.size());
            slogger.info("[MSG-RECV]  {}: '{}'", _socket.remote_address(), client_msg);

            // Enviar una respuesta al cliente
            std::string response_msg = "HelloKitty!";
            slogger.info("Enviando respuesta a {}: '{}'", _socket.remote_address(), response_msg);
            return _write_buf.write(response_msg).then([this]() {
                return _write_buf.flush();
            });
        }).then([this]() {
            slogger.info("Cerrando conexión con {}", _socket.remote_address());
            return _write_buf.close().then([this]() {
                 return _read_buf.close();
            });
        }).handle_exception_type([](const std::ios_base::failure& e) {
            slogger.warn("[WARN] I/O: {}. Close by Pair .", e.what());
        }).handle_exception([this](std::exception_ptr e) {
            slogger.error("[ERROR] Conection refuse {}: {}", _socket.remote_address(), e);
            // On errro
            return _write_buf.close().then_wrapped([this](auto f_close_write){
                // Non overhead over exception exceed
                return _read_buf.close().then_wrapped([](auto f_close_read){
                    // On error no error ( descentralizaciones!.. )
                });
            });
        });
    }
};

// Clase del servidor
class TcpServer {
    seastar::lw_shared_ptr<seastar::server_socket> _listener;
    uint16_t _port;
    seastar::sharded<ConnectionHandler> _handlers; // Sharing Example JSON-JD

public:
    TcpServer(uint16_t port) : _port(port) {}

    seastar::future<> listen() {
        seastar::listen_options lo;
        lo.reuse_address = true; // Permitir reutilizar la dirección rápidamente
        _listener = seastar::listen(seastar::make_ipv4_address({_port}), lo);
        slogger.info("[INFO] Listening On Port {}", _port);

        // Accept Server Loop
        return seastar::keep_doing([this] {
            return _listener->accept().then([this](seastar::accept_result ar) {
                seastar::connected_socket socket = std::move(ar.connection);
                auto handler = seastar::make_lw_shared<ConnectionHandler>(std::move(socket));
                // Non-Blocking
                // The future handle
                (void)handler->handle().finally([handler]{
                    // Non undereference
                });
                // The future exception
            }).handle_exception_type([](const std::system_error& e) {
                 if (e.code().value() == EBADF) { // Socket cerrado
                    slogger.info("[INFO] Listen socket closed... , stopping accept().");
                    // Loop keep_doing in to stop status
                    throw seastar::stop_iteration::now;
                }
                slogger.error("[ERROR] TCPServer::listen: Exception error accept(): {}", e.what());
                // Pass if not EBADF
            }).handle_exception([](std::exception_ptr e) {
                slogger.error("[ERROR] TCPServer::listen: Unknow error on accept(): {}", e);
                // Pass not assertion
            });
        });
    }

    // Detener el servidor
    seastar::future<> stop() {
        slogger.info("Deteniendo el servidor...");
        if (_listener) {
            _listener->abort_accept(); // Stop it
        }
        return seastar::make_ready_future<>();
    }
};

int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(DEFAULT_TCP_PORT_VALUE), "TCP Server Port");

    return app.run(argc, argv, [&app]() -> seastar::future<> {
        auto& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();

        // Create and start the server
        // We use seastar::sharded so that each core has its own instance of TcpServer,
        // although in this simple example only core 0 actually listens.
        // For a server that listens on all cores, more complex logic would be required,
        // or you could use seastar::smp::submit_to(0, ...) to start the listener on core 0.
        // Here, for simplicity, only core 0 will execute listen().
        // In a real application, you might want to distribute accepted connections to other cores.

        // Seastar uses a sharded model. We initialize the server in each shard (core).
        // But only core 0 actually calls listen().
        // This is a simplification. A real server might want each core to listen
        // on the same port (using SO_REUSEPORT) or distribute connections.
        auto server = seastar::make_lw_shared<TcpServer>(port);

        // SIGINT (Ctrl+C) y SIGTERM this the end....
        seastar::engine().handle_signal(SIGINT, [server] {
            slogger.info("[INFO] SIGINT event recv, stop...");
            (void)server->stop().then([] {
                seastar::engine().exit(0);
            });
        });
        seastar::engine().handle_signal(SIGTERM, [server] {
            slogger.info("[INFO ]SIGTERM event recv, stop...");
            (void)server->stop().then([] {
                seastar::engine().exit(0);
            });
        });
        
        slogger.info("Iniciando servidor en el core {}", seastar::this_shard_id());
        // If core 0 listen in other case no
        if (seastar::this_shard_id() == 0) {
            (void)server->listen();
        }
        
        // The future returned by app.run() must complete for the app to exit; otherwise, it ends immediately.
        // Here, since the server runs indefinitely until a signal is received, we return a future
        // that never completes or completes when the server is stopped.
        // In this example, the server stops on signals, so we can return a pending future.
        return seastar::make_ready_future<>(); // Or a future that resolves when the server should stop.
                                               // In this case, the app stays alive thanks to Seastar's reactor
                                               // and exits via engine().exit() in the signal handlers.

    });
}
