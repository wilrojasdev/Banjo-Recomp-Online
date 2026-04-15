#ifndef NET_PLAYERLIST_UI_H
#define NET_PLAYERLIST_UI_H

namespace bknet {

// Initialize the player list overlay (call once after recompui is ready)
void playerlist_ui_init();

// Update the player list overlay each frame (call from update_gfx)
void playerlist_ui_update();

// Toggle visibility (called from CTRL key handler)
void playerlist_ui_set_visible(bool visible);
bool playerlist_ui_is_visible();

} // namespace bknet

#endif // NET_PLAYERLIST_UI_H
