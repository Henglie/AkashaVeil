// SPDX-License-Identifier: Apache-2.0
#include "av/watermark/packet.h"

#include <cstring>

#include "av/watermark/ecc.h"

namespace av::watermark {

namespace {

void store_be32(std::uint8_t* out, std::uint32_t value) noexcept
{
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t load_be32(const std::uint8_t* in) noexcept
{
    return (static_cast<std::uint32_t>(in[0]) << 24)
        | (static_cast<std::uint32_t>(in[1]) << 16)
        | (static_cast<std::uint32_t>(in[2]) << 8)
        | static_cast<std::uint32_t>(in[3]);
}

} // namespace

Status encode_packet(const std::uint8_t* key, std::size_t key_length,
    std::uint8_t algorithm, const std::uint8_t* payload, std::size_t payload_length,
    std::uint8_t out_packet[kPacketSize]) noexcept
{
    if (out_packet == nullptr) {
        return Status::invalid_argument;
    }
    if (!valid_key(key, key_length)) {
        return Status::invalid_argument;
    }
    if (!known_algorithm(algorithm)) {
        return Status::unsupported_algorithm;
    }
    if (payload_length > kPayloadMax) {
        return Status::payload_too_large;
    }
    if (payload == nullptr && payload_length != 0) {
        return Status::invalid_argument;
    }

    std::memset(out_packet, 0, kPacketSize);
    std::memcpy(out_packet, kMagic, 4);
    out_packet[4] = kProtocolVersion;
    out_packet[5] = algorithm;
    out_packet[6] = 0;
    out_packet[7] = static_cast<std::uint8_t>(payload_length);
    derive_key_id(key, key_length, out_packet + 8);

    std::uint8_t padded[kPayloadField];
    std::memset(padded, 0, sizeof(padded));
    if (payload_length > 0) {
        std::memcpy(padded, payload, payload_length);
    }
    std::uint8_t stream[kPayloadField];
    derive_payload_stream(key, key_length, stream);
    for (std::size_t i = 0; i < kPayloadField; ++i) {
        out_packet[12 + i] = static_cast<std::uint8_t>(padded[i] ^ stream[i]);
    }

    const std::uint32_t crc = crc32(out_packet, 44);
    store_be32(out_packet + 44, crc);

    std::uint8_t mac_key[32];
    derive_mac_key(key, key_length, mac_key);
    std::uint8_t mac[32];
    hmac_sha256(mac_key, sizeof(mac_key), out_packet, 48, mac);
    std::memcpy(out_packet + 48, mac, 4);
    std::memset(mac_key, 0, sizeof(mac_key));
    std::memset(mac, 0, sizeof(mac));
    std::memset(stream, 0, sizeof(stream));
    std::memset(padded, 0, sizeof(padded));
    return Status::success;
}

Status decode_packet(const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t packet[kPacketSize], std::uint8_t expected_algorithm,
    DecodeResult& result) noexcept
{
    result = DecodeResult{};
    if (packet == nullptr || !valid_key(key, key_length)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    if (expected_algorithm != 0 && !known_algorithm(expected_algorithm)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    if (std::memcmp(packet, kMagic, 4) != 0) {
        result.status = Status::not_detected;
        return result.status;
    }
    result.version = packet[4];
    result.algorithm = packet[5];
    result.flags = packet[6];
    result.payload_length = packet[7];
    std::memcpy(result.key_id, packet + 8, 4);

    if (result.version != kProtocolVersion) {
        result.status = Status::unsupported_version;
        return result.status;
    }
    if (!known_algorithm(result.algorithm)) {
        result.status = Status::unsupported_algorithm;
        return result.status;
    }
    if (expected_algorithm != 0 && result.algorithm != expected_algorithm) {
        result.status = Status::unsupported_algorithm;
        return result.status;
    }
    if (result.payload_length > kPayloadMax) {
        result.status = Status::corrupted;
        return result.status;
    }

    std::uint8_t expected_id[4];
    derive_key_id(key, key_length, expected_id);
    if (std::memcmp(expected_id, packet + 8, 4) != 0) {
        result.status = Status::wrong_key;
        return result.status;
    }
    const std::uint32_t expected_crc = load_be32(packet + 44);
    if (crc32(packet, 44) != expected_crc) {
        result.status = Status::corrupted;
        return result.status;
    }
    std::uint8_t mac_key[32];
    derive_mac_key(key, key_length, mac_key);
    std::uint8_t mac[32];
    hmac_sha256(mac_key, sizeof(mac_key), packet, 48, mac);
    const bool mac_ok = std::memcmp(mac, packet + 48, 4) == 0;
    std::memset(mac_key, 0, sizeof(mac_key));
    std::memset(mac, 0, sizeof(mac));
    if (!mac_ok) {
        result.status = Status::corrupted;
        return result.status;
    }

    std::uint8_t stream[kPayloadField];
    derive_payload_stream(key, key_length, stream);
    for (std::size_t i = 0; i < kPayloadField; ++i) {
        result.payload[i] = static_cast<std::uint8_t>(packet[12 + i] ^ stream[i]);
    }
    std::memset(stream, 0, sizeof(stream));
    result.status = Status::success;
    return result.status;
}

Status make_codeword(const std::uint8_t* key, std::size_t key_length,
    std::uint8_t algorithm, const std::uint8_t* payload, std::size_t payload_length,
    std::uint8_t out_codeword[kCodewordSize]) noexcept
{
    if (out_codeword == nullptr) {
        return Status::invalid_argument;
    }
    std::uint8_t packet[kPacketSize];
    const Status status = encode_packet(key, key_length, algorithm,
        payload, payload_length, packet);
    if (status != Status::success) {
        std::memset(packet, 0, sizeof(packet));
        return status;
    }
    rs_encode(packet, out_codeword);
    std::memset(packet, 0, sizeof(packet));
    return Status::success;
}

Status read_codeword(const std::uint8_t* key, std::size_t key_length,
    const std::uint8_t codeword[kCodewordSize], std::uint8_t expected_algorithm,
    DecodeResult& result) noexcept
{
    result = DecodeResult{};
    if (codeword == nullptr || !valid_key(key, key_length)) {
        result.status = Status::invalid_argument;
        return result.status;
    }
    std::uint8_t message[kCodewordDataSize];
    std::uint8_t corrected = 0;
    if (!rs_decode(codeword, message, corrected)) {
        std::memset(message, 0, sizeof(message));
        result.status = Status::corrupted;
        return result.status;
    }
    const Status status = decode_packet(key, key_length, message, expected_algorithm, result);
    std::memset(message, 0, sizeof(message));
    if (status == Status::success) {
        result.corrected_bytes = corrected;
    }
    return result.status;
}

} // namespace av::watermark
