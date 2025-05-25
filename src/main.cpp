#include <seastar/core/seastar.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/print.hh>
#include <seastar/core/do_with.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <string>
#include <iostream>
#include <stdexcept>
#include <memory>

// Incluye tus definiciones de protocolo y codec
#include "sbb_rasta_protocol.hh" // Asume que has guardado el código del Paso 2 aquí

seastar::logger rasta_logger("rasta_server");

seastar::future<> handle_rasta_connection(seastar::connected_socket s, seastar::socket_address remote_address) {
    rasta_logger.info("Accepted connection from {}", remote_address);

    auto in = s.input();
    auto out = s.output();

    return seastar::do_with(std::move(in), std::move(out),
        [] (seastar::input_stream<char>& in, seastar::output_stream<char>& out) {
        return seastar::keep_doing([&] () {
            // Intenta parsear un mensaje RaSTA
            return sbb_rasta::rasta_protocol_codec::parse_rasta_message(in)
                .then([&] (std::unique_ptr<sbb_rasta::rasta_pdu_header> header) {
                    if (!header) { // End of stream or error
                        return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
                    }

                    rasta_logger.info("Received RaSTA message: type={}, length={}, seq={}",
                        (int)header->message_type, header->pdu_length, header->sequence_number);

 
                    sbb_rasta::rasta_pdu_header response_header = *header; // O crea uno nuevo
                    response_header.message_type = 0xFF; // Example: MEssage
                    response_header.sequence_number++; // Example: seq

                    auto response_buf = sbb_rasta::rasta_protocol_codec::serialize_rasta_message(response_header);
                    return out.write(response_buf.share()).then([&] {
                        return out.flush();
                    }).then([] {
                        return seastar::stop_iteration::no; // Go go go continue!!!
                    });
                })
                .handle_exception([&] (std::exception_ptr e) {
                    rasta_logger.error("Error processing connection: {}", e);
                    // Aquí puedes decidir cómo manejar el error (cerrar conexión, etc.)
                    return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
                });
        });
    }).finally([remote_address] {
        rasta_logger.info("Connection from {} closed.", remote_address);
    });
}

seastar::future<> rasta_server_main() {
    // Real Rasta With weed
    seastar::listen_options lo;
    lo.reuse_address = true; // Reuase Port
    auto listener = seastar::listen(seastar::make_ipv4_address({RASTA_TCP_PORT}), lo);
    rasta_logger.info("[INFO] RaSTA server listening on port 12345...");

    return seastar::keep_doing([listener = std::move(listener)] () mutable {
        return listener.accept().then([] (seastar::connected_socket s, seastar::socket_address remote_address) {
            // Manejar cada conexión en una nueva "fiber" asíncrona
            seastar::do_with(std::move(s), std::move(remote_address),
                [] (seastar::connected_socket& s_ref, seastar::socket_address& remote_addr_ref) {
                return handle_rasta_connection(std::move(s_ref), remote_addr_ref);
            });
            return seastar::stop_iteration::no; // Continuar aceptando conexiones
        });
    });
}

int main(int argc, char** argv) {
    seastar::app_template app;
    return app.run(argc, argv, [] {
        return rasta_server_main();
    });
}
