#pragma once

#include "banjo_config.h"

namespace banjo::embedded_mods {

// Registers all bundled translation packs (and their hard dependencies) with
// librecomp so they get loaded during scan_mods(). Then writes the locale-driven
// force-enable overrides so the right pack is active for the launcher's chosen
// language. Must be called before recomp::start().
//
// Mod IDs registered (filenames in BanjoRecomp/embedded_mods/ may differ):
//   bk_spanish_translation         (Spanish UI)
//   lucaspec72_french_loc          (French UI)
//   de_v10lator_ger                (German UI)
//   bk_recomp_asset_expansion_pak  (hard dep of ES + DE)
//   font_plus_latin_1              (Latin-1 font atlas, used by FR + DE)
//
// Per-language activation:
//   English -> none (base game)
//   Spanish -> bk_spanish_translation + bk_recomp_asset_expansion_pak
//   French  -> lucaspec72_french_loc + font_plus_latin_1
//   German  -> de_v10lator_ger + bk_recomp_asset_expansion_pak + font_plus_latin_1
void initialize(banjo::Language launcher_language);

} // namespace banjo::embedded_mods
