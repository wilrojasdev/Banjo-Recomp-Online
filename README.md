# BK64-Online

**Play Banjo-Kazooie in online co-op with a friend.**
Built on top of [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp) — this is not an emulator, it's the game recompiled natively for PC.

> ⚠️ **You need a Banjo-Kazooie ROM (NTSC-U 1.0).** This project does not bundle any game assets.

---

## 📥 Download

Grab the latest build from **[Releases](../../releases)**:

| Platform | File | Instructions |
|---|---|---|
| 🪟 **Windows** | `BK64-Online-Windows.zip` | Extract the zip and run `BanjoRecompiled.exe` |
| 🍎 **macOS** (Intel + Apple Silicon) | `BK64-Online-macOS.zip` | Extract the zip and open `BanjoRecompiled.app` |
| 🐧 **Linux** | `BK64-Online-Linux-X64.tar.gz` | `tar -xzf` and run `./BanjoRecompiled` |

On first launch the game will ask for your ROM. Drop your NTSC-U 1.0 ROM where it tells you and you're done.

---

## 🎮 How to play

### Hosting (one of you starts the game)

1. Open the game → **Host** menu
2. Pick a connection mode:
   - **CoopNet** (recommended) — connects over the internet without port forwarding. Share the lobby password with your friend.
   - **Direct (LAN/VPN)** — uses your IP directly. Useful when you're on the same network or using a VPN like Hamachi / ZeroTier.
3. Pick the save slot you want to play. That's it — you're now waiting for the other player.

### Joining

1. Open the game → **Join** menu
2. **Private Lobbies** → enter the host's password, or **Direct Connection** → enter the host's IP.
3. Your save is replaced **only for the session** by the host's save. Your local save on disk is left untouched.

---

## ✨ What the co-op mode does

- **Live ghost of the other player** — you see their animations, jumps, attacks, and transformations (termite, pumpkin, walrus, crocodile, bee, wishy-washy).
- **Shared collectibles** — jiggies, musical notes, jinjos, empty honeycombs, Mumbo tokens and extra lives count for both players.
- **Synced enemies** — if one of you kills something, it counts for both; enemies move identically on everyone's screen.
- **Bosses and NPCs** — Bottles, Mumbo, Nipper, Blubber, Conga, Juju, MM huts, Clanker and more respect the shared state.
- **World puzzles** — jigsaw pedestals (Lair), sandcastle letter codes (TTC), Leaky the bucket, Treasure Hunt (X + chest), switches and doors.
- **Floating nametags** above each player + **player list overlay** (hold `CTRL`).
- **Chat** — press `Tab` to type.
- **NPC locking** — when someone is talking to Bottles or transforming with Mumbo, the other player has to wait their turn.

Detailed per-world progress: [PROGRESS.md](../../blob/main/PROGRESS.md)

### World coverage

| World | Status |
|---|---|
| 🏠 Spiral Mountain | ✅ 90% |
| 🏰 Gruntilda's Lair (hub) | ✅ 85% |
| ⛰️ Mumbo's Mountain | ✅ 70% |
| 🏖️ Treasure Trove Cove | ✅ 95% |
| 🐙 Clanker's Cavern | 🔧 60% |
| 🐸 Bubblegloop Swamp | 🔧 50% |
| ❄️ Freezeezy Peak | 🔧 40% |
| 🏜️ Gobi's Valley | 🔧 50% |
| 👻 Mad Monster Mansion | 🔧 45% |
| 🚢 Rusty Bucket Bay | 🔧 45% |
| 🌳 Click Clock Wood | 🔧 35% |
| 🧙 Final Boss | 🔧 30% |

---

## ❓ FAQ

**Do I need the ROM?**
Yes, required. The game does not bundle assets — copyright reasons. Use your own legal copy of the NTSC-U 1.0 cartridge.

**Does it work with emulators or ROMs from other regions?**
No, only with the decompressed NTSC-U 1.0 ROM. This is a native recompilation, not an emulator.

**Is my save file affected when I join a session?**
No. When you join, the game uses an in-memory copy of the host's save. Your local save on disk stays untouched and only gets written when you play solo.

**Can I play with more than 2 players?**
The sync is tested with 2 players. Technically up to 4 is supported, but not guaranteed to be stable.

**Do I need to open ports?**
Not if you use **CoopNet** (recommended). The Direct mode does require opening port 7777 or being on the same LAN / VPN.

**The connection fails or is slow**
If CoopNet won't connect, try **Direct** mode with Hamachi / ZeroTier (free virtual LAN services). Also make sure both players are on the same game version.

---

## 🛠️ For developers

- How to build from source: [BUILDING.md](BUILDING.md)
- Per-world sync progress tracker: [PROGRESS.md](../../blob/main/PROGRESS.md)
- Architecture: MIPS patches in `patches/` intercept game functions, and the C++ side in `src/net/` handles networking (CoopNet / ENet).

The `build.sh` script in the project root automates the full build pipeline (MIPS cross-compile → N64Recomp → PatchesLib → BanjoRecompiled).

---

## 🙏 Credits

- [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp) — base project
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — static recompilation framework
- [RT64](https://github.com/rt64/rt64) — rendering engine
- [CoopNet](https://github.com/djoslin0/coopnet) — P2P networking with NAT traversal (ported from SM64 Coop DX)
- [Banjo-Kazooie Decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie) — headers and symbols for the original game

---

## ⚖️ License

This project is a modification of Banjo: Recompiled. See the base project for license terms. This repository **does not contain any assets or proprietary code from the original game** — you need to provide your own ROM.

Banjo-Kazooie™ is a trademark of Nintendo / Rare. This project is not affiliated with or endorsed by either company.
