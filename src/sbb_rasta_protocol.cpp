/*
 La Jenny va de abuso!!

*/

#include <seastar/core/seastar.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/print.hh>
#include <seastar/core/do_with.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>

namespace sbb_rasta {


struct rasta_pdu_header {
    uint16_t pdu_length;
    uint8_t  message_type;
    uint32_t sequence_number;
};






#pragma pack(push, 1) 
struct rasta_pdu_header {
    // 1. Length
    uint16_t pdu_length;

    // 2. Rasta Messages
    uint8_t  message_type;
    uint8_t  protocol_version;

    // 4. Número de secuencia del mensaje
    uint32_t sequence_number;

    // 5. Número de confirmación (Acknowledgement Number)
    uint32_t acknowledgement_number;

    // 6. ID de origen (Sender ID)
    uint32_t sender_id;

    // 7. ID de destino (Receiver ID)
    uint32_t receiver_id;

    // 8. Timestamp (Marca de tiempo)
    uint64_t timestamp; // Ejemplo: milisegundos desde la época Unix

    // 9. Valor de Integridad (Integrity Check Value - ICV) / Checksum
    uint32_t integrity_check_value;

    // 10. Campos de control/flags (opcional)
    // uint16_t control_flags;
};

#pragma pack(pop) 







// Clase para parsear y serializar mensajes RaSTA
class rasta_protocol_codec {
public:
    // Función para parsear un buffer en un objeto de mensaje RaSTA
    // Esto es muy simplificado; el RaSTA real es mucho más complejo.
    static seastar::future<std::unique_ptr<rasta_pdu_header>> parse_rasta_message(seastar::input_stream<char>& in) {
        return in.read_exactly(sizeof(rasta_pdu_header)).then([] (seastar::temporary_buffer<char> buf) {
            if (buf.empty()) {
                return seastar::make_exception_future<std::unique_ptr<rasta_pdu_header>>(
                    std::runtime_error("Unexpected end of stream while reading Rasta header"));
            }
            //parser
            auto header = std::make_unique<rasta_pdu_header>();
            std::memcpy(header.get(), buf.data(), sizeof(rasta_pdu_header));

            // endianness 
            //(network byte order 
            //vs host 
            // byte order)
            header->pdu_length = ntohs(header->pdu_length);
            header->sequence_number = ntohl(header->sequence_number);

            return seastar::make_ready_future<std::unique_ptr<rasta_pdu_header>>(std::move(header));
        });
    }

    // Función para serializar un objeto de mensaje RaSTA en un buffer
    static seastar::temporary_buffer<char> serialize_rasta_message(const rasta_pdu_header& header) {
        seastar::temporary_buffer<char> buf(sizeof(rasta_pdu_header));
        rasta_pdu_header h_net_order = header;
        h_net_order.pdu_length = htons(header.pdu_length);
        h_net_order.sequence_number = htonl(header.sequence_number);
        std::memcpy(buf.data(), &h_net_order, sizeof(rasta_pdu_header));
        return buf;
    }
};

} // namespace sbb_rasta
