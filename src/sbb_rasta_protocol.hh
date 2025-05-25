#pragma once

#include <cstdint>
#include <memory>
#include <seastar/core/future.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/input_stream.hh>

namespace sbb_rasta {

// PDU RaSTA
#pragma pack(push, 1)
struct rasta_pdu_header {
    uint16_t pdu_length;
    uint8_t  message_type;
    uint8_t  protocol_version;
    uint32_t sequence_number;
    uint32_t acknowledgement_number;
    uint32_t sender_id;
    uint32_t receiver_id;
    uint64_t timestamp;
    uint32_t integrity_check_value;
    // uint16_t control_flags;
};
#pragma pack(pop)

// Codec
class rasta_protocol_codec {
public:
    static seastar::future<std::unique_ptr<rasta_pdu_header>> parse_rasta_message(seastar::input_stream<char>& in);
    static seastar::temporary_buffer<char> serialize_rasta_message(const rasta_pdu_header& header);
};

} // namespace sbb_rasta
