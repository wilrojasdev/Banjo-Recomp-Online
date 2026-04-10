#include "net_chat.h"
#include "net_manager.h"
#include <cstdio>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

namespace bknet {

ChatInput& ChatInput::instance() {
    static ChatInput s_instance;
    return s_instance;
}

static SDL_EventFilter original_filter = nullptr;
static void* original_filter_data = nullptr;

static int chat_event_filter(void* userdata, SDL_Event* event) {
    auto& chat = ChatInput::instance();
    auto& net = NetworkManager::instance();

    if (!net.is_connected()) {
        if (original_filter) return original_filter(original_filter_data, event);
        return 1;
    }

    if (event->type == SDL_KEYDOWN && !event->key.repeat) {
        SDL_Keycode key = event->key.keysym.sym;

        // Tab toggles chat open/close
        if (key == SDLK_TAB) {
            if (!chat.is_active()) {
                chat.activate();
            } else {
                chat.cancel(); // close without sending
            }
            return 0;
        }

        if (chat.is_active()) {
            if (key == SDLK_RETURN) {
                chat.submit(); // send and keep open
                return 0;
            }
            if (key == SDLK_BACKSPACE) {
                chat.backspace();
                return 0;
            }
            if (key == SDLK_ESCAPE) {
                chat.cancel();
                return 0;
            }
            return 0; // consume all keys
        }
    }

    if (event->type == SDL_KEYUP) {
        if (event->key.keysym.sym == SDLK_TAB) return 0;
        if (chat.is_active()) return 0;
    }

    if (event->type == SDL_TEXTINPUT && chat.is_active()) {
        chat.append_text(event->text.text);
        return 0;
    }

    if (original_filter) return original_filter(original_filter_data, event);
    return 1;
}

void ChatInput::init() {
    SDL_GetEventFilter(&original_filter, &original_filter_data);
    SDL_SetEventFilter(chat_event_filter, nullptr);
    std::printf("[Chat] Initialized (press Tab to chat)\n");
}

void ChatInput::poll_events() {}

void ChatInput::activate() {
    active_ = true;
    input_buffer_.clear();
    SDL_StartTextInput();
}

void ChatInput::submit() {
    if (!input_buffer_.empty()) {
        NetworkManager::instance().send_chat(input_buffer_);
    }
    input_buffer_.clear();
    // Stay active after sending so user can keep chatting
}

void ChatInput::cancel() {
    input_buffer_.clear();
    active_ = false;
    SDL_StopTextInput();
}

void ChatInput::backspace() {
    if (!input_buffer_.empty()) {
        input_buffer_.pop_back();
    }
}

void ChatInput::append_text(const char* text) {
    if (input_buffer_.size() < CHAT_MAX_LENGTH - 1) {
        input_buffer_ += text;
    }
}

void ChatInput::update() {}

} // namespace bknet
