# BK64-Online

**Juega Banjo-Kazooie en cooperativo online con un amigo.**
Basado en [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp) — no es un emulador, es el juego recompilado nativamente para PC.

> ⚠️ **Necesitas un ROM de Banjo-Kazooie (versión NTSC-U 1.0).** Este proyecto no incluye archivos del juego.

---

## 📥 Descargar

Descarga la versión más reciente desde **[Releases](../../releases)**:

| Plataforma | Archivo | Instrucciones |
|---|---|---|
| 🪟 **Windows** | `BK64-Online-Windows.zip` | Extrae el zip y ejecuta `BanjoRecompiled.exe` |
| 🍎 **macOS** (Intel + Apple Silicon) | `BK64-Online-macOS.zip` | Extrae el zip y abre `BanjoRecompiled.app` |
| 🐧 **Linux** | `BK64-Online-Linux-X64.tar.gz` | `tar -xzf` y ejecuta `./BanjoRecompiled` |

En el primer arranque el juego te pedirá el ROM. Coloca tu ROM NTSC-U 1.0 donde te indique y listo.

---

## 🎮 Cómo jugar

### Anfitrión (uno de los dos crea la partida)

1. Abre el juego → menú **Host**
2. Elige modo de conexión:
   - **CoopNet** (recomendado) — conecta por internet sin abrir puertos. Comparte la contraseña del lobby con tu amigo.
   - **Direct (LAN/VPN)** — usa tu IP directamente. Útil si están en la misma red o con VPN como Hamachi/ZeroTier.
3. Selecciona tu partida guardada. Listo, estás esperando al otro jugador.

### Invitado (el que se une)

1. Abre el juego → menú **Join**
2. **Private Lobbies** → introduce la contraseña del anfitrión, o **Direct Connection** → introduce la IP.
3. Tu partida guardada se reemplaza **solo durante la sesión** por la del anfitrión. Tu save local queda intacto.

---

## ✨ Qué hace el modo cooperativo

- **Fantasma del otro jugador en tiempo real** — ves su animación, saltos, ataques, transformaciones (termita, calabaza, morsa, cocodrilo, abeja, wishy-washy).
- **Coleccionables compartidos** — jiggies, notas, jinjos, panales vacíos, tokens de Mumbo y vidas cuentan para los dos.
- **Enemigos sincronizados** — el que mate uno cuenta para ambos; los enemigos se mueven igual en la pantalla de ambos.
- **Jefes y NPCs** — Bottles, Mumbo, Nipper, Blubber, Conga, Juju, cabañas MM, Clanker y más respetan el estado compartido.
- **Puzles de mundo** — piezas de rompecabezas (Lair), códigos del castillo de arena (TTC), baldes de leche (Leaky), caza del tesoro (X + cofre), switches y puertas.
- **Nombres flotantes** sobre cada jugador + **lista de jugadores** (mantén `CTRL`).
- **Chat** — presiona `Tab` para escribir.
- **Bloqueo de NPC** — cuando alguien habla con Bottles o se transforma con Mumbo, el otro espera su turno.

Progreso detallado por mundo: [PROGRESS.md](../../blob/main/PROGRESS.md)

### Mundos cubiertos

| Mundo | Estado |
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

## ❓ Preguntas frecuentes

**¿Necesita el ROM?**
Sí, obligatorio. El juego no incluye assets — por copyright. Usa tu copia legal del cartucho NTSC-U 1.0.

**¿Funciona con emuladores o roms de otras regiones?**
No, solo con el ROM NTSC-U 1.0 decompilado. No es un emulador.

**¿Mi partida guardada se ve afectada al unirme a una sesión?**
No. Al unirte, el juego usa una copia en memoria del save del anfitrión. Tu save local en disco queda igual y solo se escribe cuando juegas en solitario.

**¿Se puede jugar con más de 2 jugadores?**
El sync está probado con 2 jugadores. Técnicamente soporta hasta 4, pero no está garantizado.

**¿Necesito abrir puertos?**
No si usas **CoopNet** (recomendado). El modo Direct sí requiere abrir puerto 7777 o estar en la misma LAN/VPN.

**La conexión falla o es lenta**
Si CoopNet no conecta, intenta el modo **Direct** con Hamachi/ZeroTier (redes virtuales gratuitas). También revisa que ambos estén en la misma versión del juego.

---

## 🛠️ Para desarrolladores

- Cómo compilar desde el código fuente: [BUILDING.md](BUILDING.md)
- Seguimiento de progreso por mundo: [PROGRESS.md](../../blob/main/PROGRESS.md)
- Arquitectura: los patches MIPS en `patches/` interceptan el código del juego y el lado C++ en `src/net/` maneja la red (CoopNet/ENet).

El script `build.sh` en la raíz del proyecto automatiza todo el pipeline de compilación (cross-compile MIPS → N64Recomp → PatchesLib → BanjoRecompiled).

---

## 🙏 Créditos

- [Banjo: Recompiled](https://github.com/BanjoRecomp/BanjoRecomp) — proyecto base
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — framework de recompilación estática
- [RT64](https://github.com/rt64/rt64) — motor de renderizado
- [CoopNet](https://github.com/djoslin0/coopnet) — red P2P con NAT traversal (portada de SM64 Coop DX)
- [Banjo-Kazooie Decompilation](https://gitlab.com/banjo.decomp/banjo-kazooie) — headers y símbolos del juego original

---

## ⚖️ Licencia

Este proyecto es una modificación de Banjo: Recompiled. Consulta el proyecto base para los términos de licencia. Este repositorio **no contiene ningún asset ni código propietario del juego original** — debes proporcionar tu propio ROM.

Banjo-Kazooie™ es marca registrada de Nintendo / Rare. Este proyecto no está afiliado ni respaldado por ninguna de esas compañías.
