#ifndef NET_CHAT_H
#define NET_CHAT_H

#include <string>
#include <cstdint>
#include "net_packets.h"

namespace bknet {

class ChatInput {
public:
    static ChatInput& instance();

    void init();
    void update();
    void poll_events(); // Call BEFORE handle_events in update_gfx

    bool is_active() const { return active_; }
    const std::string& get_input_buffer() const { return input_buffer_; }

    void activate();
    void submit();
    void cancel();
    void backspace();
    void append_text(const char* text);

private:
    ChatInput() = default;

    bool active_ = false;
    std::string input_buffer_;
};

} // namespace bknet

#endif // NET_CHAT_H
