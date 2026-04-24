#pragma once

namespace banjo::ui {
    // 4pt baseline spacing scale — use these for any gap/padding/margin
    // instead of naked float literals. Named tokens keep the launcher
    // visually consistent across dialogs.
    namespace space {
        constexpr float xs  = 4.0f;
        constexpr float sm  = 8.0f;
        constexpr float md  = 12.0f;
        constexpr float lg  = 16.0f;
        constexpr float xl  = 20.0f;
        constexpr float xxl = 24.0f;
    }

    // Dialog / card layout — shared across every modal panel so they
    // all share the same shape and breathing room.
    namespace dialog {
        constexpr float padding_v           = 40.0f;  // card vertical padding
        constexpr float padding_h           = 48.0f;  // card horizontal padding
        constexpr float max_width           = 640.0f; // card max width
        constexpr float backdrop_padding    = 24.0f;  // backdrop edge gutter
        constexpr float card_gap            = 16.0f;  // gap between card children
        constexpr float section_gap         = 8.0f;   // gap within a labeled section
        constexpr float section_top_margin  = 12.0f;  // margin between sections
        constexpr float action_row_top      = 16.0f;  // margin above Back/CTA row
        constexpr float action_row_gap      = 20.0f;  // gap between action buttons
    }

    // Button sizing — minimum widths so buttons don't collapse to text width.
    namespace button {
        constexpr float cta_min_width       = 160.0f; // primary / Back
        constexpr float secondary_min_width = 120.0f; // Erase, Copy, etc.
        constexpr float list_item_min_width = 80.0f;  // inline list buttons
    }
}
