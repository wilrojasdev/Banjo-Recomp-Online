#include "net_chat_ui.h"
#include "net_manager.h"
#include "net_chat.h"

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_types.h"

#include <string>
#include <cstdio>
#include <chrono>

namespace bknet {

using namespace recompui;

// === Chat panel (Tab to open/close) ===
static ContextId chat_context;
static Label* chat_messages_label = nullptr;
static Label* chat_input_label = nullptr;
static bool chat_initialized = false;

// === Preview feed (always visible for new messages) ===
static ContextId preview_context;
static Label* preview_label = nullptr;
static bool preview_initialized = false;
static size_t last_msg_count = 0;
static double last_msg_time = 0.0;
static constexpr double PREVIEW_DURATION = 5.0;

static double get_time() {
    static auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void chat_ui_init() {
    chat_initialized = false;
    preview_initialized = false;
    last_msg_count = 0;
    last_msg_time = 0.0;
}

// --- Chat panel ---
static void ensure_chat_init() {
    if (chat_initialized) return;
    chat_initialized = true;

    chat_context = create_context();
    chat_context.open();

    Element* window = chat_context.create_element<Element>(chat_context.get_root_element());
    window->set_display(Display::Flex);
    window->set_flex_direction(FlexDirection::Column);
    window->set_background_color(Color{0, 0, 0, 0});

    Element* container = chat_context.create_element<Element>(window);
    container->set_position(Position::Absolute);
    container->set_bottom(40, Unit::Dp);
    container->set_left(20, Unit::Dp);
    container->set_width(400, Unit::Dp);
    container->set_max_height(250, Unit::Dp);
    container->set_display(Display::Flex);
    container->set_flex_direction(FlexDirection::Column);
    container->set_background_color(Color{0, 0, 0, 180});
    container->set_border_radius(8, Unit::Dp);
    container->set_padding(12, Unit::Dp);

    chat_messages_label = chat_context.create_element<Label>(container, "", LabelStyle::Small);
    chat_messages_label->set_margin_bottom(8, Unit::Dp);

    chat_input_label = chat_context.create_element<Label>(container, "> _", LabelStyle::Small);
    chat_input_label->set_padding(6, Unit::Dp);
    chat_input_label->set_background_color(Color{255, 255, 255, 25});
    chat_input_label->set_border_radius(4, Unit::Dp);

    chat_context.close();
}

// --- Preview feed ---
static void ensure_preview_init() {
    if (preview_initialized) return;
    preview_initialized = true;

    preview_context = create_context();
    preview_context.open();

    Element* window = preview_context.create_element<Element>(preview_context.get_root_element());
    window->set_display(Display::Flex);
    window->set_flex_direction(FlexDirection::Column);
    window->set_background_color(Color{0, 0, 0, 0});

    Element* container = preview_context.create_element<Element>(window);
    container->set_position(Position::Absolute);
    container->set_bottom(40, Unit::Dp);
    container->set_left(20, Unit::Dp);
    container->set_width(400, Unit::Dp);
    container->set_display(Display::Flex);
    container->set_flex_direction(FlexDirection::Column);

    preview_label = preview_context.create_element<Label>(container, "", LabelStyle::Small);
    preview_label->set_padding(8, Unit::Dp);
    preview_label->set_background_color(Color{0, 0, 0, 150});
    preview_label->set_border_radius(6, Unit::Dp);

    preview_context.close();
}

static std::string format_messages(const std::deque<NetworkManager::ChatEntry>& msgs, size_t max_count) {
    std::string text;
    size_t start = msgs.size() > max_count ? msgs.size() - max_count : 0;
    for (size_t i = start; i < msgs.size(); i++) {
        const auto& msg = msgs[i];
        const char* labels[] = {"Host", "P2", "P3", "P4"};
        const char* label = labels[msg.player_id < 4 ? msg.player_id : 3];
        text += "[" + std::string(label) + "] " + msg.message;
        if (i < msgs.size() - 1) text += "\n";
    }
    return text;
}

void chat_ui_update() {
    auto& net = NetworkManager::instance();
    auto& chat = ChatInput::instance();

    if (!net.is_connected()) {
        if (chat_initialized && is_context_shown(chat_context)) hide_context(chat_context);
        if (preview_initialized && is_context_shown(preview_context)) hide_context(preview_context);
        return;
    }

    ensure_chat_init();
    ensure_preview_init();

    double now = get_time();

    if (chat.is_active()) {
        // --- Full chat panel open ---

        // Hide preview when full chat is open
        if (is_context_shown(preview_context)) hide_context(preview_context);

        if (!is_context_shown(chat_context)) show_context(chat_context, "");

        auto all_msgs = net.get_all_chat_messages();
        std::string msgs_text = format_messages(all_msgs, 10);

        chat_context.open();
        chat_messages_label->set_text(msgs_text.empty() ? "No messages yet" : msgs_text);
        chat_input_label->set_text("> " + chat.get_input_buffer() + "_");
        chat_context.close();

        last_msg_count = all_msgs.size();

    } else {
        // --- Chat panel closed ---

        if (is_context_shown(chat_context)) hide_context(chat_context);

        // Check for new messages to show in preview
        auto all_msgs = net.get_all_chat_messages();
        if (all_msgs.size() > last_msg_count) {
            last_msg_time = now;
            last_msg_count = all_msgs.size();
        }

        // Show preview if recent messages exist
        if (now - last_msg_time < PREVIEW_DURATION && last_msg_time > 0.0) {
            if (!is_context_shown(preview_context)) show_context(preview_context, "");

            std::string preview_text = format_messages(all_msgs, 5);
            preview_context.open();
            preview_label->set_text(preview_text);
            preview_context.close();
        } else {
            if (is_context_shown(preview_context)) hide_context(preview_context);
        }
    }
}

} // namespace bknet
