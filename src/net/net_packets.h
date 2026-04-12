#ifndef NET_PACKETS_H
#define NET_PACKETS_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace bknet {

constexpr uint8_t MAX_PLAYERS = 4;
constexpr uint16_t DEFAULT_PORT = 7777;
constexpr uint8_t CHANNEL_UNRELIABLE = 0; // Position updates
constexpr uint8_t CHANNEL_RELIABLE = 1;   // Join/leave/world state
constexpr uint8_t NUM_CHANNELS = 2;

enum class PacketType : uint8_t {
    // Connection (reliable)
    PlayerJoin       = 0x01,
    PlayerLeave      = 0x02,
    PlayerAssignment = 0x03, // Server assigns player_id to new client
    Ping             = 0x04,
    Pong             = 0x05,

    // Phase 1: Position (unreliable)
    PlayerPosition   = 0x10,

    // Phase 2: Full player state (unreliable for frequent, reliable for infrequent)
    PlayerState      = 0x20,
    PlayerAnimation  = 0x21,

    // Chat
    ChatMessage      = 0x28,

    // Phase 3: World state (reliable)
    WorldCollectible = 0x30,
    WorldEnemy       = 0x31,
    WorldObject      = 0x32,
    MapChange        = 0x33,
    WorldStateFull   = 0x3F,
};

#pragma pack(push, 1)

struct PacketHeader {
    PacketType type;
    uint8_t player_id;
    uint16_t sequence;
};

// --- Connection packets ---

struct PlayerJoinPacket {
    PacketHeader header;
    char player_name[32];
};

struct PlayerLeavePacket {
    PacketHeader header;
    uint8_t reason; // 0=disconnect, 1=timeout, 2=kicked
};

struct PlayerAssignmentPacket {
    PacketHeader header;
    uint8_t assigned_player_id;
    uint8_t current_player_count;
};

struct PingPacket {
    PacketHeader header;
    uint64_t timestamp_us;
};

struct PongPacket {
    PacketHeader header;
    uint64_t ping_timestamp_us;
    uint64_t pong_timestamp_us;
};

// --- Position packets (Phase 1) ---

struct PlayerPositionPacket {
    PacketHeader header;
    float x, y, z;
    float yaw;
    uint32_t map_id;
};

// --- Full state packets (Phase 2) ---

enum PlayerStateDirtyFlags : uint32_t {
    DIRTY_POSITION      = 1 << 0,
    DIRTY_ROTATION      = 1 << 1,
    DIRTY_ANIMATION     = 1 << 2,
    DIRTY_HEALTH        = 1 << 3,
    DIRTY_ITEMS         = 1 << 4,
    DIRTY_TRANSFORMATION = 1 << 5,
    DIRTY_MAP           = 1 << 6,
};

struct PlayerStatePacket {
    PacketHeader header;
    uint32_t dirty_flags;
    float x, y, z;
    float yaw, pitch, roll;
    uint32_t map_id;
    uint16_t animation_id;
    float anim_progress;
    uint8_t health;
    uint8_t health_total;
    uint8_t lives;
    uint16_t eggs;
    uint16_t red_feathers;
    uint16_t gold_feathers;
    uint16_t mumbo_tokens;
    uint8_t transformation;
    uint32_t bs_state;
    float horizontal_velocity;
};

// --- World state packets (Phase 3) ---

struct MapChangePacket {
    PacketHeader header;
    uint32_t new_map_id;
    uint32_t exit_id;
};

struct WorldCollectiblePacket {
    PacketHeader header;
    uint16_t collectible_type;
    uint16_t collectible_id;
    uint8_t collected; // 1=collected, 0=uncollected
};

struct WorldEnemyPacket {
    PacketHeader header;
    uint16_t marker_type;
    uint16_t spawn_index;
    uint8_t alive;
    uint8_t health;
};

// --- Chat ---

constexpr size_t CHAT_MAX_LENGTH = 128;

struct ChatMessagePacket {
    PacketHeader header;
    uint8_t msg_length;
    char message[CHAT_MAX_LENGTH];
};

#pragma pack(pop)

// Serialization helpers
inline std::vector<uint8_t> serialize(const void* packet, size_t size) {
    std::vector<uint8_t> data(size);
    std::memcpy(data.data(), packet, size);
    return data;
}

template<typename T>
inline bool deserialize(const uint8_t* data, size_t size, T& out) {
    if (size < sizeof(T)) return false;
    std::memcpy(&out, data, sizeof(T));
    return true;
}

inline PacketType peek_type(const uint8_t* data, size_t size) {
    if (size < sizeof(PacketHeader)) return static_cast<PacketType>(0xFF);
    return static_cast<PacketType>(data[0]);
}

} // namespace bknet

#endif // NET_PACKETS_H
