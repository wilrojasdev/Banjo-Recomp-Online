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

// Player accent colors (same as playerlist)
static const Color player_colors[] = {
    {255, 215, 0,   255},  // Gold (host/P1)
    {92,  184, 255, 255},  // Sky blue (P2)
    {255, 107, 107, 255},  // Coral red (P3)
    {107, 255, 135, 255},  // Mint green (P4)
};

// === Chat panel (Tab to open/close) ===
static ContextId chat_context;
static Element* chat_container = nullptr;
static Label* chat_title_label = nullptr;
static Element* chat_messages_container = nullptr;
static Label* chat_input_label = nullptr;
static bool chat_initialized = false;

// Per-message labels (pre-allocated for max visible)
static constexpr size_t MAX_VISIBLE_MSGS = 8;
struct ChatMsgRow {
    Element* row = nullptr;
    Element* dot = nullptr;
    Label* name_label = nullptr;
    Label* msg_label = nullptr;
};
static ChatMsgRow chat_msg_rows[MAX_VISIBLE_MSGS];

// === Preview feed (always visible for new messages) ===
static ContextId preview_context;
static Element* preview_container = nullptr;
static constexpr size_t MAX_PREVIEW_MSGS = 3;
struct PreviewMsgRow {
    Element* row = nullptr;
    Element* dot = nullptr;
    Label* name_label = nullptr;
    Label* msg_label = nullptr;
};
static PreviewMsgRow preview_msg_rows[MAX_PREVIEW_MSGS];

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

// Helper: create a message row (dot + name + message)
static void create_msg_row_elements(ContextId& ctx, Element* parent, Element*& row, Element*& dot, Label*& name_lbl, Label*& msg_lbl) {
    row = ctx.create_element<Element>(parent);
    row->set_display(Display::Flex);
    row->set_flex_direction(FlexDirection::Row);
    row->set_align_items(AlignItems::FlexStart);
    row->set_width(100, Unit::Percent);
    row->set_padding_top(3, Unit::Dp);
    row->set_padding_bottom(3, Unit::Dp);
    row->display_hide();

    dot = ctx.create_element<Element>(row);
    dot->set_width(6, Unit::Dp);
    dot->set_height(6, Unit::Dp);
    dot->set_min_width(6, Unit::Dp);
    dot->set_border_radius(3, Unit::Dp);
    dot->set_margin_right(8, Unit::Dp);
    dot->set_margin_top(5, Unit::Dp);
    dot->set_background_color(Color{180, 180, 180, 200});

    name_lbl = ctx.create_element<Label>(row, "", theme::Typography::LabelXS);
    name_lbl->set_color(Color{200, 200, 220, 200});
    name_lbl->set_font_weight(600);
    name_lbl->set_margin_right(6, Unit::Dp);
    name_lbl->set_min_width(50, Unit::Dp);

    msg_lbl = ctx.create_element<Label>(row, "", theme::Typography::LabelXS);
    msg_lbl->set_color(Color{240, 240, 255, 220});
    msg_lbl->set_flex_grow(1.0f);
}

// --- Chat panel ---
static void ensure_chat_init() {
    if (chat_initialized) return;
    chat_initialized = true;

    chat_context = create_context();
    chat_context.set_captures_input(false);
    chat_context.set_captures_mouse(false);
    chat_context.open();

    Element* window = chat_context.create_element<Element>(chat_context.get_root_element());
    window->set_display(Display::Flex);
    window->set_flex_direction(FlexDirection::Column);
    window->set_background_color(Color{0, 0, 0, 0});

    chat_container = chat_context.create_element<Element>(window);
    chat_container->set_position(Position::Absolute);
    chat_container->set_bottom(30, Unit::Dp);
    chat_container->set_left(20, Unit::Dp);
    chat_container->set_width(420, Unit::Dp);
    chat_container->set_max_height(320, Unit::Dp);
    chat_container->set_display(Display::Flex);
    chat_container->set_flex_direction(FlexDirection::Column);
    chat_container->set_background_color(Color{10, 14, 28, 220});
    chat_container->set_border_radius(10, Unit::Dp);
    chat_container->set_padding(14, Unit::Dp);

    // Title bar
    Element* title_row = chat_context.create_element<Element>(chat_container);
    title_row->set_display(Display::Flex);
    title_row->set_flex_direction(FlexDirection::Row);
    title_row->set_align_items(AlignItems::Center);
    title_row->set_width(100, Unit::Percent);
    title_row->set_margin_bottom(8, Unit::Dp);

    // Accent dot
    Element* title_dot = chat_context.create_element<Element>(title_row);
    title_dot->set_width(8, Unit::Dp);
    title_dot->set_height(8, Unit::Dp);
    title_dot->set_border_radius(4, Unit::Dp);
    title_dot->set_background_color(Color{255, 215, 0, 200});
    title_dot->set_margin_right(8, Unit::Dp);

    chat_title_label = chat_context.create_element<Label>(title_row, "CHAT", theme::Typography::LabelSM);
    chat_title_label->set_color(Color{255, 255, 255, 200});
    chat_title_label->set_font_weight(700);

    // Separator
    Element* sep = chat_context.create_element<Element>(chat_container);
    sep->set_width(100, Unit::Percent);
    sep->set_height(1, Unit::Dp);
    sep->set_background_color(Color{255, 255, 255, 25});
    sep->set_margin_bottom(6, Unit::Dp);

    // Messages area
    chat_messages_container = chat_context.create_element<Element>(chat_container);
    chat_messages_container->set_display(Display::Flex);
    chat_messages_container->set_flex_direction(FlexDirection::Column);
    chat_messages_container->set_width(100, Unit::Percent);
    chat_messages_container->set_margin_bottom(8, Unit::Dp);

    for (size_t i = 0; i < MAX_VISIBLE_MSGS; i++) {
        create_msg_row_elements(chat_context, chat_messages_container,
            chat_msg_rows[i].row, chat_msg_rows[i].dot,
            chat_msg_rows[i].name_label, chat_msg_rows[i].msg_label);
    }

    // Separator before input
    Element* sep2 = chat_context.create_element<Element>(chat_container);
    sep2->set_width(100, Unit::Percent);
    sep2->set_height(1, Unit::Dp);
    sep2->set_background_color(Color{255, 255, 255, 20});
    sep2->set_margin_bottom(6, Unit::Dp);

    // Input area
    Element* input_row = chat_context.create_element<Element>(chat_container);
    input_row->set_display(Display::Flex);
    input_row->set_flex_direction(FlexDirection::Row);
    input_row->set_align_items(AlignItems::Center);
    input_row->set_width(100, Unit::Percent);
    input_row->set_padding(8, Unit::Dp);
    input_row->set_background_color(Color{255, 255, 255, 12});
    input_row->set_border_radius(6, Unit::Dp);

    Label* input_arrow = chat_context.create_element<Label>(input_row, ">", theme::Typography::LabelSM);
    input_arrow->set_color(Color{255, 215, 0, 180});
    input_arrow->set_font_weight(700);
    input_arrow->set_margin_right(8, Unit::Dp);

    chat_input_label = chat_context.create_element<Label>(input_row, "_", theme::Typography::LabelSM);
    chat_input_label->set_color(Color{255, 255, 255, 200});
    chat_input_label->set_flex_grow(1.0f);

    chat_context.close();
}

// --- Preview feed ---
static void ensure_preview_init() {
    if (preview_initialized) return;
    preview_initialized = true;

    preview_context = create_context();
    preview_context.set_captures_input(false);
    preview_context.set_captures_mouse(false);
    preview_context.open();

    Element* window = preview_context.create_element<Element>(preview_context.get_root_element());
    window->set_display(Display::Flex);
    window->set_flex_direction(FlexDirection::Column);
    window->set_background_color(Color{0, 0, 0, 0});

    preview_container = preview_context.create_element<Element>(window);
    preview_container->set_position(Position::Absolute);
    preview_container->set_bottom(30, Unit::Dp);
    preview_container->set_left(20, Unit::Dp);
    preview_container->set_width(380, Unit::Dp);
    preview_container->set_display(Display::Flex);
    preview_container->set_flex_direction(FlexDirection::Column);
    preview_container->set_background_color(Color{10, 14, 28, 190});
    preview_container->set_border_radius(8, Unit::Dp);
    preview_container->set_padding(10, Unit::Dp);

    for (size_t i = 0; i < MAX_PREVIEW_MSGS; i++) {
        create_msg_row_elements(preview_context, preview_container,
            preview_msg_rows[i].row, preview_msg_rows[i].dot,
            preview_msg_rows[i].name_label, preview_msg_rows[i].msg_label);
    }

    preview_context.close();
}

// Fill a set of msg rows from a message deque
static void fill_msg_rows(ContextId& ctx, auto* rows, size_t max_rows,
                           const std::deque<NetworkManager::ChatEntry>& msgs, size_t max_msgs) {
    ctx.open();

    size_t count = std::min(msgs.size(), max_msgs);
    size_t start = msgs.size() > max_msgs ? msgs.size() - max_msgs : 0;

    for (size_t i = 0; i < max_rows; i++) {
        if (i < count) {
            const auto& msg = msgs[start + i];
            rows[i].row->display_show();

            if (msg.player_id == 0xFF) {
                // System message (join/leave)
                rows[i].dot->set_background_color(Color{160, 160, 180, 150});
                rows[i].name_label->set_text("");
                rows[i].name_label->set_color(Color{160, 160, 180, 180});
                rows[i].msg_label->set_text(msg.message);
                rows[i].msg_label->set_color(Color{160, 160, 180, 180});
            } else {
                // Player message
                auto info = NetworkManager::instance().get_player_info(msg.player_id);
                std::string name = info.name.empty() ? ("P" + std::to_string(msg.player_id + 1)) : info.name;

                uint8_t ci = msg.player_id < 4 ? msg.player_id : 3;
                rows[i].dot->set_background_color(player_colors[ci]);

                rows[i].name_label->set_text(name);
                rows[i].name_label->set_color(Color{
                    player_colors[ci].r, player_colors[ci].g, player_colors[ci].b, 220
                });
                rows[i].msg_label->set_text(msg.message);
                rows[i].msg_label->set_color(Color{240, 240, 255, 220});
            }
        } else {
            rows[i].row->display_hide();
        }
    }

    ctx.close();
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
        if (is_context_shown(preview_context)) hide_context(preview_context);
        if (!is_context_shown(chat_context)) show_context(chat_context, "");

        auto all_msgs = net.get_all_chat_messages();
        fill_msg_rows(chat_context, chat_msg_rows, MAX_VISIBLE_MSGS, all_msgs, MAX_VISIBLE_MSGS);

        chat_context.open();
        std::string input_text = chat.get_input_buffer();
        chat_input_label->set_text(input_text.empty() ? "_" : input_text + "_");
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

            fill_msg_rows(preview_context, preview_msg_rows, MAX_PREVIEW_MSGS, all_msgs, MAX_PREVIEW_MSGS);
        } else {
            if (is_context_shown(preview_context)) hide_context(preview_context);
        }
    }
}

} // namespace bknet
