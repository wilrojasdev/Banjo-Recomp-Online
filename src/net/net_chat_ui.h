#ifndef NET_CHAT_UI_H
#define NET_CHAT_UI_H

namespace bknet {

// Initialize the chat overlay UI (call once after recompui is ready)
void chat_ui_init();

// Update the chat overlay each frame (call from update_gfx)
void chat_ui_update();

} // namespace bknet

#endif // NET_CHAT_UI_H
