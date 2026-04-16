# BK64-Online

**Online cooperative multiplayer for Banjo-Kazooie**, built on top of [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp).

Play through the entire game with a friend over the internet. Explore worlds together, collect items collaboratively, and watch each other's progress in real time.

> **This project does not contain game assets.** A US 1.0 Banjo-Kazooie ROM is required.

---

## Features

### Multiplayer
- **Online Co-op** via [CoopNet](https://github.com/coopnet) (NAT traversal, no port forwarding needed) or Direct IP
- **Ghost System** -- see your partner moving through the world in real time
- **Shared Progress** -- jiggies, notes, jinjos, mumbo tokens, empty honeycombs, and abilities sync between players
- **Collectible Deduplication** -- no double-counting when both players grab the same item
- **Enemy & NPC Sync** -- enemy kills, Bottles conversations, and Mumbo transformations are coordinated
- **World Object Sync** -- destructible huts, Juju totem, Conga encounters, Chimpy delivery
- **Jigsaw Puzzle Sync** -- cooperative pedestal solving with player locking
- **Flag & Switch Sync** -- doors, platforms, and world events stay consistent
- **Floating Nametags** -- see your partner's name above their ghost
- **Player List Overlay** -- press CTRL to see connected players

### Inherited from Banjo: Recompiled
- Native PC port via static recompilation (not emulation)
- High framerate, widescreen/ultrawide, dual analog camera
- Note saving, instant load times, mod support
- Windows, Linux, and macOS

---

## Getting Started

### Host a Game
1. Launch the game and select **Host**
2. Choose **CoopNet** (internet) or **Direct** (LAN/VPN)
3. Share the lobby code or your IP with your partner

### Join a Game
1. Launch the game and select **Join**
2. Enter the lobby code (CoopNet) or host IP (Direct)

---

## Building

### Requirements
- CMake 3.20+
- A C/C++ compiler (Clang recommended)
- MIPS cross-compiler for patches (Clang with MIPS target)
- US 1.0 Banjo-Kazooie ROM

### Build Steps

```bash
# Configure (first time only)
cd BanjoRecomp
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build (after any code change)
cd ..
./build.sh
```

The `build.sh` script handles the full pipeline:
1. Cross-compiles patches to MIPS (`patches.elf`)
2. Runs N64Recomp to generate recompiled C code
3. Builds PatchesLib and links the final executable

> **Important:** Always use `./build.sh` instead of building individual targets. The N64Recomp step is required to convert patch changes into host code.

---

## Architecture

```
Patches (MIPS C)          Host (C++)
+-----------------------+ +---------------------------+
| network_world_sync.c  | | net_manager.cpp           |
| network_conga_sync.c  | | net_recomp_api.cpp        |
| network_flag_sync.c   | | net_packets.h             |
| network_bottles_sync.c| | NetworkManager            |
| network_mumbo_sync.c  | | CoopNetTransport / ENet   |
| network_remote_player | +---------------------------+
| network_jigsaw_sync.c |
| network_hut_sync.c    |
| network_juju_sync.c   |
| note_saving.c         |
+-----------------------+
```

**Patches** run inside the recompiled N64 game engine (MIPS → host via N64Recomp). They intercept game functions to detect state changes and broadcast them.

**Host code** manages networking (CoopNet/ENet), packet serialization, and bridges events between the network layer and the patch callbacks.

---

## Synchronized Systems

| System | Method | Scope |
|--------|--------|-------|
| Jiggies | Bitfield polling + `despawn_jiggy_by_id` | Global |
| Notes | Collision callback + `is_note_collected` dedup | Per-level |
| Jinjos | Bit-mask + remote jiggy spawn on completion | Per-level |
| Mumbo Tokens | Bitfield polling + UID-based actor despawn | Global |
| Empty Honeycombs | Bitfield polling + UID-based actor despawn | Global |
| Enemies | Death callback + spawn index tracking | Per-map |
| Flags | RECOMP_PATCH on all 4 flag systems | Per-type |
| Bottles | NPC lock + ability broadcast | Per-actor |
| Mumbo | Transformation sync + token deduction | Per-level |
| Jigsaw Puzzles | Lock/place/complete protocol | Per-pedestal |
| MM Objects | Hut smash, Juju hits, Conga oranges | Per-actor |

---

## Credits

- [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp) -- the base project
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) -- static recompilation framework
- [RT64](https://github.com/rt64/rt64) -- rendering engine
- [CoopNet](https://github.com/coopnet) -- P2P networking with NAT traversal
- [BK Decompilation](https://github.com/bombsquad-community/banern) -- reverse engineering headers and functions

---

## License

This project is a modification of Banjo: Recompiled. See the original project for license terms. This repository does not contain any game assets or proprietary code from the original game.
