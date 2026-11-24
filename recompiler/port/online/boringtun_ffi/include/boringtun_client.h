#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BtTunnel BtTunnel;

// Action codes returned in *out_action by boringtun_decapsulate / boringtun_update_timers.
enum {
    BT_ACTION_DONE = 0,
    BT_ACTION_WRITE_TO_NETWORK = 1,
    BT_ACTION_WRITE_TO_TUNNEL_IPV4 = 4,
    BT_ACTION_WRITE_TO_TUNNEL_IPV6 = 6,
};

// Generates a fresh X25519 keypair encoded as base64 NUL-terminated strings.
// Returns 0 on success, < 0 on error.
int boringtun_generate_keypair(char* priv_b64_out, size_t priv_cap, char* pub_b64_out, size_t pub_cap);

// Creates a WireGuard tunnel using base64-encoded client private key and server public key.
// Returns NULL on invalid keys.
BtTunnel* boringtun_tunnel_new(const char* priv_b64, const char* server_pub_b64, uint16_t keepalive_sec);

// Frees a tunnel created with boringtun_tunnel_new.
void boringtun_tunnel_free(BtTunnel* tun);

// Encapsulates a plaintext IPv4 packet into a WireGuard UDP packet.
// Returns > 0 (bytes written to dst), 0 (queued/no output), or < 0 (error).
int boringtun_encapsulate(BtTunnel* tun, const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap);

// Decapsulates an incoming WireGuard UDP packet.
// Sets *out_action to BT_ACTION_* and returns > 0 (bytes in dst), 0 (done), or < 0 (error).
int boringtun_decapsulate(BtTunnel* tun, const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap,
                          int* out_action);

// Advances WireGuard timers (keepalive / handshake retransmit).
// Returns > 0 (bytes in dst to send to server), 0 (nothing to send), or < 0 (error).
int boringtun_update_timers(BtTunnel* tun, uint8_t* dst, size_t dst_cap);

// Forces an immediate handshake initiation packet into dst.
// Returns > 0 (bytes in dst to send to server), 0 (handshake already in progress), or < 0 (error).
int boringtun_force_handshake(BtTunnel* tun, uint8_t* dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif
