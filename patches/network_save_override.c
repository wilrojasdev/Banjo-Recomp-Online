#include "patches.h"
#include "functions.h"
#include "enums.h"
#include "save.h"
#include "core1/eeprom.h"

// EEPROM layout for BK64:
//   vanilla save data: 4 slots x 15 blocks = 60 blocks (bytes 0..479)
//   save extension data (BK64 custom): 4 slots x 40 blocks (bytes 512..1791)
//   global extension data: bytes 1792..2047
// Total: 2048 bytes = 256 blocks of 8 bytes = EEPROM16K.
// Buffer is sized to hold the entire address space the game can touch.
#define EEPROM_OVERRIDE_SIZE 2048

// Overrides all EEPROM I/O to an in-memory buffer. Activated on the join
// side after receiving the host's full EEPROM snapshot, so every
// read/write the game issues during the session hits this buffer instead
// of the player's own save file on disk. Mirrors the pattern used by
// SM64 Coop DX (gOverrideEeprom) — saves are ephemeral on the join side.
static u8   g_override_buffer[EEPROM_OVERRIDE_SIZE];
static bool g_override_enabled = FALSE;
static bool g_override_ready   = FALSE;  // populated by host EEPROM packet

// Core helpers used by the fall-through path (host / solo)
extern void func_8024F35C(s32);
extern OSMesgQueue *pfsManager_getFrameReplyQ(void);

// Packet bridge (C++ side queues a HostEeprom packet; MIPS pops and applies)
extern u32 recomp_net_pop_host_eeprom(void *out_buffer, s32 size);
extern u32 recomp_net_should_send_host_eeprom(u8 *out_target_player);
extern void recomp_net_send_host_eeprom(void *data, s32 size, u32 target_player);
extern u32 recomp_net_is_host(void);

// SaveData is 0x78 (120 bytes) -> 15 blocks per slot in vanilla layout.
#define EEPROM_OVERRIDE_BLOCKS (EEPROM_OVERRIDE_SIZE / EEPROM_BLOCK_SIZE)

// RECOMP_PATCH replaces the engine-level EEPROM entry points. The same
// address/offset math vanilla uses is preserved; only the backing store
// changes when the override is active.
RECOMP_PATCH s32 eeprom_writeBlocks(s32 file, s32 offset, void *buffer, s32 count) {
    s32 blocks_per_file = (sizeof(SaveData) + EEPROM_BLOCK_SIZE - 1) / EEPROM_BLOCK_SIZE;
    s32 block_addr = file * blocks_per_file + offset;

    if (g_override_enabled) {
        s32 byte_addr = block_addr * EEPROM_BLOCK_SIZE;
        s32 byte_count = count * EEPROM_BLOCK_SIZE;
        if (byte_addr < 0 || byte_addr + byte_count > EEPROM_OVERRIDE_SIZE) {
            return -1;
        }
        memcpy(&g_override_buffer[byte_addr], buffer, byte_count);
        return 0;
    }

    s32 ret;
    func_8024F35C(3);
    ret = osEepromLongWrite(pfsManager_getFrameReplyQ(), block_addr, buffer, count * EEPROM_BLOCK_SIZE);
    func_8024F35C(0);
    return ret;
}

RECOMP_PATCH s32 eeprom_readBlocks(s32 file, s32 offset, void *buffer, s32 count) {
    s32 blocks_per_file = (sizeof(SaveData) + EEPROM_BLOCK_SIZE - 1) / EEPROM_BLOCK_SIZE;
    s32 block_addr = file * blocks_per_file + offset;

    if (g_override_enabled) {
        s32 byte_addr = block_addr * EEPROM_BLOCK_SIZE;
        s32 byte_count = count * EEPROM_BLOCK_SIZE;
        if (byte_addr < 0 || byte_addr + byte_count > EEPROM_OVERRIDE_SIZE) {
            // Fill with zeros on out-of-range read to avoid returning garbage.
            bzero(buffer, byte_count);
            return 0;
        }
        memcpy(buffer, &g_override_buffer[byte_addr], byte_count);
        return 0;
    }

    s32 ret;
    func_8024F35C(3);
    ret = osEepromLongRead(pfsManager_getFrameReplyQ(), block_addr, buffer, count * EEPROM_BLOCK_SIZE);
    func_8024F35C(0);
    return ret;
}

// === Exports called by other patches / C++ bridge ===

// Copy local EEPROM (entire 2048-byte address space) into `out` for the
// host to ship to joining clients. Runs with the override DISABLED so the
// read reaches actual hardware / host file. Returns size written.
RECOMP_EXPORT s32 bkrecomp_net_save_read_full_eeprom(u8 *out) {
    if (!out) return 0;
    bool prev = g_override_enabled;
    g_override_enabled = FALSE;  // force real I/O for this dump

    s32 i;
    for (i = 0; i < EEPROM_OVERRIDE_BLOCKS; i++) {
        // eeprom_readBlocks indexes by (file, offset) where block_addr =
        // file * blocks_per_file + offset. Easier to linearize: use file=0
        // and offset=i since the vanilla read is just a flat EEPROM access.
        func_8024F35C(3);
        osEepromLongRead(pfsManager_getFrameReplyQ(), i, &out[i * EEPROM_BLOCK_SIZE], EEPROM_BLOCK_SIZE);
        func_8024F35C(0);
    }

    g_override_enabled = prev;
    return EEPROM_OVERRIDE_SIZE;
}

// Join: fill the override buffer from the host's EEPROM snapshot.
// Called from the packet receive path.
RECOMP_EXPORT void bkrecomp_net_save_populate_override(u8 *data, s32 size) {
    s32 copy = size;
    if (copy > EEPROM_OVERRIDE_SIZE) copy = EEPROM_OVERRIDE_SIZE;
    if (copy > 0 && data) {
        memcpy(g_override_buffer, data, copy);
    }
    // Any untouched bytes beyond `copy` stay zeroed from BSS init.
    g_override_ready = TRUE;
}

// Pull the HostEeprom packet from the C++ queue (if any) and populate
// the override buffer. Returns TRUE when a packet was consumed.
RECOMP_EXPORT bool bkrecomp_net_save_try_pop_host_eeprom(void) {
    static u8 scratch[EEPROM_OVERRIDE_SIZE];
    if (!recomp_net_pop_host_eeprom(scratch, EEPROM_OVERRIDE_SIZE)) {
        return FALSE;
    }
    bkrecomp_net_save_populate_override(scratch, EEPROM_OVERRIDE_SIZE);
    return TRUE;
}

RECOMP_EXPORT void bkrecomp_net_save_override_enable(void) {
    g_override_enabled = TRUE;
}

RECOMP_EXPORT void bkrecomp_net_save_override_disable(void) {
    g_override_enabled = FALSE;
}

RECOMP_EXPORT bool bkrecomp_net_save_override_is_ready(void) {
    return g_override_ready;
}

RECOMP_EXPORT bool bkrecomp_net_save_override_is_enabled(void) {
    return g_override_enabled;
}

// Per-frame hook driven from bkrecomp_net_process_world_events.
// Host: checks the pending-send flag from C++ and ships its EEPROM when
//       a new peer has connected.
// Join: drains any queued HostEeprom packets into the override buffer.
RECOMP_EXPORT void bkrecomp_net_save_override_tick(void) {
    if (recomp_net_is_host()) {
        u8 target = 0;
        if (recomp_net_should_send_host_eeprom(&target)) {
            static u8 snapshot[EEPROM_OVERRIDE_SIZE];
            bkrecomp_net_save_read_full_eeprom(snapshot);
            recomp_net_send_host_eeprom(snapshot, EEPROM_OVERRIDE_SIZE, (u32)target);
            recomp_printf("[SAVE-OVERRIDE] host shipped EEPROM to player %d\n", target);
        }
    } else {
        // Pop any queued snapshot. Once the buffer is ready, the title
        // screen patch flips g_override_enabled and runs gameFile_load.
        if (!g_override_ready) {
            if (bkrecomp_net_save_try_pop_host_eeprom()) {
                recomp_printf("[SAVE-OVERRIDE] join buffer populated from host\n");
            }
        }
    }
}
