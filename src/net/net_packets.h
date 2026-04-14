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
    EnemyPositionBulk = 0x34,  // Unreliable, ~20Hz from world owner
    WorldOwnership   = 0x35,  // Host broadcasts ownership assignments
    WorldOwnerTransfer = 0x36, // State handoff when owner leaves world
    WorldKillResync  = 0x37,  // Request owner to re-broadcast killed enemies
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
    uint32_t level_id;
    uint16_t animation_id;
    float anim_progress;
    float anim_duration;
    float anim_subrange_start;
    float anim_subrange_end;
    uint8_t anim_playback_type;
    uint8_t kazooie_flags;
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

// Collectible types for WorldCollectiblePacket
enum CollectibleType : uint8_t {
    COLLECTIBLE_JIGGY       = 0,
    COLLECTIBLE_NOTE        = 1,
    COLLECTIBLE_JINJO       = 2,
    COLLECTIBLE_MUMBO_TOKEN = 3,
};

struct WorldCollectiblePacket {
    PacketHeader header;
    uint8_t collectible_type;   // CollectibleType enum
    uint16_t collectible_id;    // jiggy_e, note_index, jinjo bitmask, mumbotoken_e
    uint8_t collected;          // 1=collected
    uint32_t map_id;            // Which map
    uint8_t level_id;           // Which level
    float pos_x, pos_y, pos_z;  // Player position at time of collection (for proximity despawn)
};

struct WorldEnemyPacket {
    PacketHeader header;
    uint16_t marker_type;       // Enemy marker type enum
    uint16_t spawn_index;       // Which instance of this enemy type
    uint32_t map_id;            // Which map the enemy is on
    uint8_t alive;              // 0=dead
    uint8_t health;
    uint16_t _pad;
    float pos_x, pos_y, pos_z;  // Enemy position at time of death
};

// --- Enemy position sync (host-authoritative) ---

constexpr uint8_t MAX_ENEMIES_PER_PACKET = 64;

struct EnemyPositionEntry {
    uint16_t spawn_index;
    uint16_t marker_type;
    float x, y, z;
    float yaw;
    uint16_t anim_id;
    uint16_t _pad;
    float anim_timer;
};  // 28 bytes

struct EnemyPositionBulkPacket {
    PacketHeader header;
    uint32_t map_id;
    uint8_t enemy_count;
    uint8_t _pad[3];
    EnemyPositionEntry enemies[MAX_ENEMIES_PER_PACKET];
};

// Flag types for WorldFlagPacket
enum WorldFlagType : uint8_t {
    FLAG_MAP_SPECIFIC   = 0,      // Per-map actor states (doors, switches within a map)
    FLAG_LEVEL_SPECIFIC = 1,      // Per-level progress flags
    FLAG_FILE_PROGRESS  = 2,      // Global file/save progress (jiggy doors, note doors, witch switches)
    FLAG_VOLATILE       = 3,      // Runtime volatile flags (witch switch pressed, sandcastle doors)
    FLAG_JIGSAW_ACTION  = 4,      // Jigsaw puzzle pedestal sync (lock/unlock/add/remove/complete)
    FLAG_ABILITY        = 5,      // Ability learned/unlearned sync
    FLAG_BOTTLES_ACTION = 6,      // Bottles NPC lock/unlock (one player at a time)
};

struct WorldFlagPacket {
    PacketHeader header;
    uint8_t flag_type;          // WorldFlagType enum
    uint16_t flag_index;
    uint8_t value;
    uint32_t map_id;            // Context: which map (for map-specific flags)
};

// --- World ownership (host broadcasts to all) ---

struct WorldOwnershipPacket {
    PacketHeader header;
    uint32_t level_id;          // Which level (stable across sub-areas)
    uint8_t owner_player_id;    // Who owns this world (0xFF = no owner / reset)
    uint8_t _pad[3];
};

// --- World owner transfer (state handoff when owner leaves) ---

constexpr uint8_t MAX_KILLED_TRANSFER = 64;

struct KilledEnemyEntry {
    uint16_t marker_type;
    uint16_t spawn_index;
    float pos_x, pos_y, pos_z;
    uint32_t map_id;
};  // 20 bytes

struct WorldOwnerTransferPacket {
    PacketHeader header;
    uint32_t level_id;
    uint8_t killed_count;
    uint8_t _pad[3];
    KilledEnemyEntry killed[MAX_KILLED_TRANSFER];
};

// --- Kill resync request (ask owner to re-broadcast kills) ---

struct WorldKillResyncPacket {
    PacketHeader header;
    uint32_t level_id;
};

// --- Full world state sync (sent to joiner) ---

struct WorldStateFullPacket {
    PacketHeader header;
    uint32_t map_id;
    uint8_t level_id;
    uint8_t _pad1[3];
    uint8_t jiggy_score[13];
    uint8_t mumbo_score[16];
    uint8_t honeycomb_score[3];
    uint8_t jinjo_bits;         // ITEM_12_JINJOS bitmask
    uint8_t _pad2;
    uint16_t note_count;
    uint8_t lives;
    uint8_t _pad3;
    // Flag sync (Phase 8)
    uint8_t file_progress_flags[37]; // fileProgressFlags bitfield (0x25 bytes)
    uint8_t level_specific_flags[8]; // levelSpecificFlags bitfield
    uint8_t volatile_flags[25];      // volatileFlags bitfield (0x19 bytes)
    uint32_t map_specific_flags;     // mapSpecificFlags (single u32)
    uint8_t has_flags;               // 1 if flag data is present (backwards compat)
    uint8_t _pad4[3];
    uint8_t abilities[8];            // learnedAbilities (4 bytes) + usedAbilities (4 bytes)
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
