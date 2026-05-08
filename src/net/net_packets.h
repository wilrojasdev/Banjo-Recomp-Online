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

// Application-level protocol version. Bumped whenever packet layout or
// semantics change. Sent on connect; a mismatch closes the peer immediately
// with a specific error instead of silently desyncing.
//
// v4: EnemyPositionEntry repurposes one trailing _pad byte as anim_direction
// (forward/backward playback) so Conga's idle rocking and other directional
// animations stay synced across peers.
// v5: EnemyPositionEntry repurposes the remaining _pad byte as `state`
// (actor->state cast to u8). Lets non-owner peers adopt the owner's state
// machine wholesale for bosses (Conga) instead of running their own.
// v6: PlayerState.anim_duration is Animation.duration (blend 0..1), not anctrl clip length.
// v7: WorldEnemyPacket carries actor->state captured AFTER the killer's
// dieFunc runs. Receivers stop calling die() locally (which corrupted Banjo
// state and froze the game thread) and instead apply the death state to
// actor->state — the local update function runs the death animation
// naturally. Mirrors sm64-coop-dx's oAction sync pattern.
constexpr uint32_t PROTOCOL_VERSION = 7;

// Optional features negotiated in VersionCheck. Both peers' reported bitmaps
// are ANDed; behaviour downgrades for features not common to both. This lets
// us add capability without bumping PROTOCOL_VERSION and breaking old builds.
constexpr uint32_t FEATURE_FRAGMENT_NACK = 1u << 0;
constexpr uint32_t FEATURE_DIRTY_DELTA   = 1u << 1;
constexpr uint32_t FEATURE_BW_CAP        = 1u << 2;
// Joiner echoes WorldStateFull receipt; host retries up to N times if no ack
// arrives within ~12s. Without it, a silent fragment-reassembly failure leaves
// the joiner stuck with no world state and the host eventually drops them by
// idle timeout. Negotiated, so old peers fall back to fire-and-forget.
constexpr uint32_t FEATURE_FULLSTATE_ACK = 1u << 3;
// zlib-deflate wrapping for payloads > COMPRESSION_THRESHOLD. Negotiated, so
// peers without the feature receive raw bytes. Reduces WorldStateFull (~600B)
// and HostEeprom (2 KB) burst on join — typical compression ratio 3-4× on
// our flag bitfields keeps the join packet single-fragment in most cases.
constexpr uint32_t FEATURE_COMPRESSION = 1u << 4;
constexpr uint32_t SUPPORTED_FEATURES =
    FEATURE_FRAGMENT_NACK | FEATURE_DIRTY_DELTA | FEATURE_BW_CAP |
    FEATURE_FULLSTATE_ACK | FEATURE_COMPRESSION;

// Don't bother compressing tiny packets — zlib adds ~6 bytes of overhead and
// PlayerPosition/PlayerState are mostly floats which compress poorly anyway.
constexpr size_t COMPRESSION_THRESHOLD = 256;

enum class PacketType : uint8_t {
    // Connection (reliable)
    PlayerJoin       = 0x01,
    PlayerLeave      = 0x02,
    PlayerAssignment = 0x03, // Server assigns player_id to new client
    Ping             = 0x04,
    Pong             = 0x05,
    KeepAlive        = 0x06, // Idle heartbeat: sent every ~10s by each peer
    VersionCheck     = 0x07, // First packet after peer connect; mismatch = unpeer

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
    CongaOrangeSpawn = 0x38,  // Conga orange projectile spawn (world owner → others)
    WorldStateFull   = 0x3F,
    HostEeprom       = 0x40,  // Host ships its full EEPROM (2 KB) to join on connect
    WorldStateFullAck= 0x41,  // Joiner → host: confirms WorldStateFull was applied

    // Fragmented transfer (for payloads > ~1200 bytes): splits reliable packets
    // into sequenced chunks so a single lost chunk triggers only one small
    // retransmit instead of the entire bulk.
    FragmentChunk    = 0x50,
    FragmentNack     = 0x51, // Receiver → sender: bitmap of missing chunks

    // Wrapper: original packet wrapped in zlib-deflate. Receiver inflates and
    // re-dispatches the inner packet through the normal handler.
    Compressed       = 0x52,
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

// Empty keepalive: header is enough to reset the receiver's idle timer.
struct KeepAlivePacket {
    PacketHeader header;
};

// Protocol compatibility handshake. Host sends after peer-connected; joiner
// echoes. Mismatch on protocol_version = unpeer with ProtocolMismatch.
// feature_flags is a bitmap (FEATURE_*) — peers honor only the intersection.
struct VersionCheckPacket {
    PacketHeader header;
    uint32_t protocol_version;
    uint32_t feature_flags;
};

// Fragmented transfer envelope. `group_id` groups chunks of a single logical
// payload; `chunk_index` is 0-based; `chunk_count` is total chunks. The last
// chunk may be shorter. `original_type` is the PacketType to materialize after
// reassembly (so receiver dispatches the result normally).
constexpr size_t FRAGMENT_CHUNK_PAYLOAD = 1200; // conservative, < typical MTU

struct FragmentChunkPacket {
    PacketHeader header;
    uint16_t group_id;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint16_t chunk_size;        // bytes valid in `data`
    uint8_t  original_type;     // PacketType of the reassembled packet
    uint8_t  _pad[3];
    uint32_t total_size;        // total bytes across all chunks
    uint8_t  data[FRAGMENT_CHUNK_PAYLOAD];
};

// Receiver → sender. If chunks_received < chunk_count after 2s of silence
// on this group, receiver tells sender exactly which indices are missing so
// only those get retransmitted (complements the reliable channel — catches
// the rare case where a chunk was dropped before reaching the wire, e.g. by
// our send_queue backpressure).
constexpr size_t FRAGMENT_NACK_BITMAP_BYTES = 32; // 256 bits → up to 256 chunks

struct FragmentNackPacket {
    PacketHeader header;
    uint16_t group_id;
    uint16_t chunk_count;
    uint8_t  missing_bitmap[FRAGMENT_NACK_BITMAP_BYTES];
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
    DIRTY_CARRY         = 1 << 7,  // Carried-object kind (e.g. orange)
};

// Carried-object discriminator. Mirrors the local bacarry_get_marker()
// state on each peer so ghosts render the visual prop in their hands.
// Add new kinds here as more carryable quest items get networked.
enum CarryKind : uint8_t {
    CARRY_KIND_NONE   = 0,
    CARRY_KIND_ORANGE = 1, // ACTOR_29_ORANGE_COLLECTIBLE / Chimpy
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
    uint8_t carry_kind;     // CarryKind enum — visual prop replicated to ghosts
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
    uint8_t state;              // actor->state captured AFTER killer's dieFunc
                                // returned. 0 = no state info (legacy / resync /
                                // poll-detected despawn) — receiver despawns
                                // directly instead of replaying the animation.
    uint8_t _pad;
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
    uint8_t  anim_direction;  // 0 = backward, 1 = forward (anctrl playback dir)
    uint8_t  state;           // actor->state low byte (0 = no state to apply)
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
    FLAG_HUT_ACTION     = 8,      // MM hut destruction sync (spawn_index in flag_index, smashCount in value)
    FLAG_JUJU_ACTION    = 9,      // MM Juju totem segment hit sync (hit_count in flag_index)
    FLAG_CONGA_HIT      = 10,     // Conga hit count sync (flag_index=unk38_31, value=unk10_12)
    FLAG_LEAKY_ACTION   = 11,     // TTC Leaky bucket egg-counter sync (value=egg count 0-2)
    FLAG_SANDCASTLE_ACTION = 12,  // TTC Sandcastle cheat-code progress (flag_index=code idx or 0xFE for BK state, value=new codeCharacterIdx)
    FLAG_NIPPER_ACTION     = 13,  // TTC Nipper state + lifetime sync (flag_index=sub-action, value=new state / lifetime/40 / has_met_before)
    FLAG_BLUBBER_ACTION    = 14,  // TTC Blubber delivery decrement + quest-complete despawn (flag_index=sub-action 0/1)
    FLAG_TREASUREHUNT_ACTION = 15, // TTC Treasure Hunt step counter sync (value=new chtreasureHunt_puzzleCurrentStep 0-6)
    FLAG_SHARED_ITEM       = 16,  // Shared inventory (eggs/red/gold feathers): flag_index = item_e, value = signed int8 diff
    FLAG_DIALOG_COMPLETE_ANIM = 17, // Generic NPC post-dialog animation cue (flag_index = npc tag id, value = optional u8 param)
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
    uint8_t picked_bullion_mask; // TTC gold-bullion spawn_index bitmask (was _pad2)
    uint16_t note_count;
    uint8_t lives;
    uint8_t bullions;               // ITEM_18_GOLD_BULLIONS — Blubber quest
    // Flag sync (Phase 8)
    uint8_t file_progress_flags[37]; // fileProgressFlags bitfield (0x25 bytes)
    uint8_t level_specific_flags[8]; // levelSpecificFlags bitfield
    uint8_t volatile_flags[25];      // volatileFlags bitfield (0x19 bytes)
    uint32_t map_specific_flags;     // mapSpecificFlags (single u32)
    uint8_t has_flags;               // 1 if flag data is present (backwards compat)
    uint8_t _pad4[3];
    uint8_t abilities[8];            // learnedAbilities (4 bytes) + usedAbilities (4 bytes)
    // Cross-world note sync (Phase 13):
    // - note_scores mirrors D_80385FF0 (per-level high scores fed to
    //   itemscore_noteScores_getTotal; gates note doors).
    // - level_notes mirrors loaded_file_extension_data.level_notes[9] (BK64's
    //   custom per-note bitfield; prevents already-collected notes from
    //   respawning when a player enters a world someone else cleared).
    uint8_t note_scores[11];
    uint8_t _pad5;
    uint8_t level_notes[9][32];      // matches SaveFileExtensionData.level_notes
    // Per-level collected-jinjo bitfield (bits 0..4 = B/G/O/P/Y). Shared
    // across all players, persisted in the save extension so death/exit
    // never respawn already-collected jinjos. Matches
    // SaveFileExtensionData.jinjos_collected.
    uint8_t jinjos_collected[9];
    uint8_t _pad6[3];                // align to 4-byte boundary
};

// --- Host EEPROM snapshot (sent to each joiner on connect) ---
//
// Mirrors the SM64 Coop DX pattern: instead of each client running off
// its own local save file, the host's full 2 KB EEPROM is shipped on
// join and the client redirects all EEPROM I/O to an in-memory buffer
// seeded with this data. Saves on the client side stay ephemeral —
// they never touch disk. See network_save_override.c on the MIPS side.
constexpr size_t HOST_EEPROM_SIZE = 2048;

// zlib wrapper. The outer header carries type=Compressed; payload is a
// deflate stream of the original packet (header + body). Receiver inflates
// up to `original_size` bytes and re-dispatches through handle_packet.
// Wire layout: [CompressedPacketHeader][deflate bytes...]
struct CompressedPacketHeader {
    PacketHeader header;
    uint32_t original_size;
    uint32_t compressed_size;
};

// Joiner → host. Sent when the receiver enqueues a WorldStateFull packet so
// the host knows the bulk transfer landed. Host gates resends on this ack.
struct WorldStateFullAckPacket {
    PacketHeader header;
    uint32_t map_id; // echoed for correlation/debugging
};

struct HostEepromPacket {
    PacketHeader header;
    // Current save slot the host is playing (0..2). Joiners load this exact
    // slot instead of guessing the first non-empty one — critical when the
    // host's EEPROM has multiple non-empty slots and the active one isn't 0.
    int16_t current_slot;
    uint8_t _pad[2];
    uint8_t eeprom[HOST_EEPROM_SIZE];
};

// --- Conga orange projectile sync (world owner broadcasts spawn events) ---

struct CongaOrangeSpawnPacket {
    PacketHeader header;
    uint32_t map_id;
    float spawn_x, spawn_y, spawn_z;
    float vel_x, vel_y, vel_z;
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
