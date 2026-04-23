#ifndef __SAVE_EXTENSION_H__
#define __SAVE_EXTENSION_H__

typedef struct {
    u8 bytes[32]; // 32*8 = 256 bits per level, enough to account for mods with unused vanilla rooms.
} LevelNotes;

// This struct must be 256 bytes to add up to 1536 bytes, which adds to the original 512 bytes of save data to equal exactly 2048 bytes.
typedef struct {
    LevelNotes level_notes[9];
    // Per-level collected-jinjo bitfield. 1 byte per level, bits 0..4 =
    // Blue/Green/Orange/Pink/Yellow. Promoted to persistent state so
    // jinjos don't respawn on death or world re-entry, matching the
    // sharing semantics already used for jiggies/mumbos/notes. Old
    // saves have zero here (from the former padding), which is also
    // the "none collected" initial state — no migration needed.
    u8 jinjos_collected[9];
    u8 padding[23]; // Reserved for future use (was 32 before jinjos).
} SaveFileExtensionData;

_Static_assert(sizeof(SaveFileExtensionData) == 320, "SaveExtensionData must be 320 bytes");

typedef struct {
    u8 padding[256];
} SaveGlobalExtensionData;

_Static_assert(sizeof(SaveGlobalExtensionData) == 256, "save_global_extension_data must be 512 bytes");

extern SaveFileExtensionData loaded_file_extension_data;

#endif
