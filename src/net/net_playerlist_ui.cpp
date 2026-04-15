#include "net_playerlist_ui.h"
#include "net_manager.h"
#include "net_packets.h"

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_types.h"

#include <string>
#include <cstdio>

namespace bknet {

using namespace recompui;

static ContextId playerlist_context;
static bool playerlist_initialized = false;
static bool playerlist_visible = false;

// UI elements
static Label* title_label = nullptr;
static Label* subtitle_label = nullptr;

// Per-player row elements
struct PlayerRow {
    Element* row = nullptr;
    Element* dot = nullptr;
    Label* name_label = nullptr;
    Label* tag_label = nullptr;
};
static PlayerRow player_rows[MAX_PLAYERS];

// Player accent colors (RGBA)
static const Color player_dot_colors[] = {
    {255, 215, 0,   255},  // Gold (host/P1)
    {92,  184, 255, 255},  // Sky blue (P2)
    {255, 107, 107, 255},  // Coral red (P3)
    {107, 255, 135, 255},  // Mint green (P4)
};

void playerlist_ui_init() {
    playerlist_initialized = false;
    playerlist_visible = false;
}

static void ensure_init() {
    if (playerlist_initialized) return;
    playerlist_initialized = true;

    playerlist_context = create_context();
    playerlist_context.set_captures_input(false);
    playerlist_context.set_captures_mouse(false);
    playerlist_context.open();

    // Root window (transparent, fills screen)
    Element* window = playerlist_context.create_element<Element>(playerlist_context.get_root_element());
    window->set_display(Display::Flex);
    window->set_flex_direction(FlexDirection::Column);
    window->set_background_color(Color{0, 0, 0, 0});

    // Main container - top left
    Element* container = playerlist_context.create_element<Element>(window);
    container->set_position(Position::Absolute);
    container->set_top(20, Unit::Dp);
    container->set_left(20, Unit::Dp);
    container->set_width(340, Unit::Dp);
    container->set_display(Display::Flex);
    container->set_flex_direction(FlexDirection::Column);
    container->set_align_items(AlignItems::Center);
    container->set_background_color(Color{10, 14, 28, 220});
    container->set_border_radius(12, Unit::Dp);
    container->set_padding_top(16, Unit::Dp);
    container->set_padding_bottom(16, Unit::Dp);
    container->set_padding_left(20, Unit::Dp);
    container->set_padding_right(20, Unit::Dp);

    // Top accent line
    Element* accent_line = playerlist_context.create_element<Element>(container);
    accent_line->set_width(60, Unit::Dp);
    accent_line->set_height(3, Unit::Dp);
    accent_line->set_background_color(Color{255, 215, 0, 200});
    accent_line->set_border_radius(2, Unit::Dp);
    accent_line->set_margin_bottom(12, Unit::Dp);

    // Title
    title_label = playerlist_context.create_element<Label>(container, "PLAYERS", theme::Typography::LabelMD);
    title_label->set_color(Color{255, 255, 255, 230});
    title_label->set_font_weight(700);
    title_label->set_text_align(TextAlign::Center);
    title_label->set_margin_bottom(4, Unit::Dp);

    // Subtitle (player count)
    subtitle_label = playerlist_context.create_element<Label>(container, "", theme::Typography::LabelXS);
    subtitle_label->set_color(Color{180, 180, 200, 160});
    subtitle_label->set_text_align(TextAlign::Center);
    subtitle_label->set_margin_bottom(12, Unit::Dp);

    // Separator
    Element* separator = playerlist_context.create_element<Element>(container);
    separator->set_width(100, Unit::Percent);
    separator->set_height(1, Unit::Dp);
    separator->set_background_color(Color{255, 255, 255, 30});
    separator->set_margin_bottom(8, Unit::Dp);

    // Player rows
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        auto& pr = player_rows[i];

        // Row container
        pr.row = playerlist_context.create_element<Element>(container);
        pr.row->set_display(Display::Flex);
        pr.row->set_flex_direction(FlexDirection::Row);
        pr.row->set_align_items(AlignItems::Center);
        pr.row->set_width(100, Unit::Percent);
        pr.row->set_padding_top(6, Unit::Dp);
        pr.row->set_padding_bottom(6, Unit::Dp);
        pr.row->set_padding_left(8, Unit::Dp);
        pr.row->set_padding_right(8, Unit::Dp);
        pr.row->set_border_radius(6, Unit::Dp);
        pr.row->display_hide();

        // Color dot
        pr.dot = playerlist_context.create_element<Element>(pr.row);
        pr.dot->set_width(8, Unit::Dp);
        pr.dot->set_height(8, Unit::Dp);
        pr.dot->set_min_width(8, Unit::Dp);
        pr.dot->set_border_radius(4, Unit::Dp);
        pr.dot->set_background_color(player_dot_colors[i]);
        pr.dot->set_margin_right(10, Unit::Dp);

        // Name label
        pr.name_label = playerlist_context.create_element<Label>(pr.row, "", theme::Typography::LabelSM);
        pr.name_label->set_color(Color{255, 255, 255, 220});
        pr.name_label->set_flex_grow(1.0f);

        // Tag (Host / You)
        pr.tag_label = playerlist_context.create_element<Label>(pr.row, "", theme::Typography::LabelXS);
        pr.tag_label->set_color(Color{180, 180, 200, 140});
        pr.tag_label->set_margin_left(8, Unit::Dp);
    }

    playerlist_context.close();
}

void playerlist_ui_set_visible(bool visible) {
    playerlist_visible = visible;
}

bool playerlist_ui_is_visible() {
    return playerlist_visible;
}

void playerlist_ui_update() {
    auto& net = NetworkManager::instance();

    if (!net.is_connected()) {
        playerlist_visible = false;
        if (playerlist_initialized && is_context_shown(playerlist_context)) {
            hide_context(playerlist_context);
        }
        return;
    }

    ensure_init();

    if (!playerlist_visible) {
        if (is_context_shown(playerlist_context)) {
            hide_context(playerlist_context);
        }
        return;
    }

    if (!is_context_shown(playerlist_context)) {
        show_context(playerlist_context, "");
    }

    uint8_t local_id = net.local_player_id();
    uint8_t connected_count = 0;

    playerlist_context.open();

    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
        auto info = net.get_player_info(i);
        auto& pr = player_rows[i];

        if (!info.connected) {
            pr.row->display_hide();
            continue;
        }

        connected_count++;
        pr.row->display_show();

        // Highlight local player row
        if (i == local_id) {
            pr.row->set_background_color(Color{255, 255, 255, 15});
        } else {
            pr.row->set_background_color(Color{0, 0, 0, 0});
        }

        // Name
        pr.name_label->set_text(info.name.empty() ? "Player" : info.name);

        // Tags
        std::string tag;
        if (i == 0) tag += "HOST";
        if (i == local_id) {
            if (!tag.empty()) tag += " · ";
            tag += "YOU";
        }
        pr.tag_label->set_text(tag);
    }

    // Update subtitle
    std::string sub = std::to_string(connected_count) + " / " + std::to_string(MAX_PLAYERS) + " connected";
    subtitle_label->set_text(sub);

    playerlist_context.close();
}

} // namespace bknet
