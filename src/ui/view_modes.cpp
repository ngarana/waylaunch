#include "waylaunch/view_modes.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace waylaunch {

std::unique_ptr<ViewRenderer> ViewRendererFactory::create(ViewMode mode) {
    switch (mode) {
        case ViewMode::List: return std::make_unique<ListViewRenderer>();
        case ViewMode::Icon: return std::make_unique<IconViewRenderer>();
        case ViewMode::Column: return std::make_unique<ColumnViewRenderer>();
        case ViewMode::Gallery: return std::make_unique<GalleryViewRenderer>();
        default: return std::make_unique<ListViewRenderer>();
    }
}

int ListViewRenderer::item_height(int, const LayoutMetrics& layout) const {
    return layout.item_height;
}

int ListViewRenderer::items_per_page(int, int height, const LayoutMetrics& layout) const {
    int available = height - layout.padding * 2 - layout.input_height;
    return std::max(1, available / layout.item_height);
}

int ListViewRenderer::hit_test(double, double y, int, int,
                              const LayoutMetrics& layout, int total_entries) const {
    int items_y = layout.padding + layout.input_height + layout.padding;
    int header_h = 28;
    int content_y = items_y + header_h;

    if (y < content_y || y > content_y + layout.max_visible_items * layout.item_height) return -1;

    int idx = static_cast<int>((y - content_y) / layout.item_height);
    if (idx >= 0 && idx < total_entries) return idx;
    return -1;
}

void ListViewRenderer::render_header(Renderer& renderer, int x, int y, int w, const Theme& theme) {
    renderer.fill_rect(x, y, w, 28, theme.background_alt);

    int col_x = x + 8;
    renderer.draw_text(col_x, y + 6, "Name", theme.result_detail_font, theme.text_muted);
    col_x += w * 40 / 100;
    renderer.draw_text(col_x, y + 6, "Date Modified", theme.result_detail_font, theme.text_muted);
    col_x += w * 25 / 100;
    renderer.draw_text(col_x, y + 6, "Size", theme.result_detail_font, theme.text_muted);
    col_x += w * 15 / 100;
    renderer.draw_text(col_x, y + 6, "Kind", theme.result_detail_font, theme.text_muted);
}

void ListViewRenderer::render_row(Renderer& renderer, int x, int y, int w, int h,
                                 const FileEntry& entry, bool selected, bool, const Theme& theme) {
    if (selected) {
        renderer.fill_rect(x, y, w, h, theme.selection);
    }

    int text_y = y + (h - 14) / 2;
    int col_x = x + 8;

    std::string icon = entry.is_directory ? "folder" : "file";
    renderer.draw_text(col_x, text_y, entry.name, theme.result_font, theme.foreground);
    col_x += w * 40 / 100;

    renderer.draw_text(col_x, text_y, entry.display_date(), theme.result_font, theme.text_muted);
    col_x += w * 25 / 100;

    renderer.draw_text(col_x, text_y, entry.display_size(), theme.result_font, theme.text_muted);
    col_x += w * 15 / 100;

    renderer.draw_text(col_x, text_y, entry.file_kind(), theme.result_font, theme.text_muted);
}

void ListViewRenderer::render(Renderer& renderer, uint8_t* buffer_data, int buffer_stride,
                             int width, int height, double,
                             const Theme& theme, const LayoutMetrics& layout,
                             const std::vector<FileEntry>& entries,
                             const ViewState& state) {
    renderer.render_into(buffer_data, buffer_stride, width, height, 1.0,
                        theme, layout, "", 0, {}, 0, -1);

    int items_y = layout.padding + layout.input_height + layout.padding;
    int header_h = 28;
    int content_y = items_y + header_h;
    int visible_items = std::min(layout.max_visible_items, static_cast<int>(entries.size()));
    if (visible_items < 1 && !entries.empty()) visible_items = 1;

    render_header(renderer, layout.padding, items_y, width - 2 * layout.padding, theme);

    if (visible_items > 0) {
        renderer.fill_rect(layout.padding, content_y, width - 2 * layout.padding,
                          visible_items * layout.item_height, theme.background_alt);
    }

    for (int i = 0; i < visible_items; i++) {
        int item_idx = state.scroll_offset + i;
        if (item_idx >= static_cast<int>(entries.size())) break;

        int item_y = content_y + i * layout.item_height;
        bool is_selected = (item_idx == state.selected_index) ||
                          std::find(state.selected_indices.begin(), state.selected_indices.end(), item_idx) != state.selected_indices.end();

        render_row(renderer, layout.padding, item_y, width - 2 * layout.padding,
                  layout.item_height, entries[item_idx], is_selected, false, theme);
    }

    if (static_cast<int>(entries.size()) > visible_items) {
        renderer.draw_scrollbar(width - layout.padding - 8, content_y,
                               visible_items * layout.item_height,
                               static_cast<int>(entries.size()), visible_items,
                                state.scroll_offset, theme.accent);
    }
}

int IconViewRenderer::item_height(int, const LayoutMetrics&) const {
    return ICON_SIZE + ICON_TEXT_HEIGHT + ICON_PADDING;
}

int IconViewRenderer::items_per_page(int width, int height, const LayoutMetrics& layout) const {
    int cols = std::max(1, (width - 2 * layout.padding) / (ICON_SIZE + ICON_PADDING));
    int rows = std::max(1, (height - 2 * layout.padding - layout.input_height) /
                      (ICON_SIZE + ICON_TEXT_HEIGHT + ICON_PADDING));
    return cols * rows;
}

int IconViewRenderer::hit_test(double x, double y, int width, int,
                              const LayoutMetrics& layout, int total_entries) const {
    int items_y = layout.padding + layout.input_height + layout.padding;
    int cols = std::max(1, (width - 2 * layout.padding) / (ICON_SIZE + ICON_PADDING));
    int cell_w = ICON_SIZE + ICON_PADDING;
    int cell_h = ICON_SIZE + ICON_TEXT_HEIGHT + ICON_PADDING;

    int col = static_cast<int>((x - layout.padding) / cell_w);
    int row = static_cast<int>((y - items_y) / cell_h);

    if (col < 0 || col >= cols || row < 0) return -1;

    int idx = row * cols + col;
    if (idx >= 0 && idx < total_entries) return idx;
    return -1;
}

void IconViewRenderer::render_icon(Renderer& renderer, int x, int y, int w, int h,
                                  const FileEntry& entry, bool selected, const Theme& theme) {
    if (selected) {
        renderer.rounded_rect(x, y, w, h, theme.corner_radius, theme.selection);
    }

    int icon_x = x + (w - ICON_SIZE) / 2;
    int icon_y = y + 8;

    renderer.rounded_rect(icon_x, icon_y, ICON_SIZE, ICON_SIZE,
                         theme.corner_radius, theme.background_alt);

    int text_y = icon_y + ICON_SIZE + 4;
    int text_x = x + 4;

    std::vector<TextSegment> segments;
    segments.push_back({entry.name, theme.foreground});
    renderer.draw_text_segments(text_x, text_y, segments, theme.result_detail_font);
}

void IconViewRenderer::render(Renderer& renderer, uint8_t* buffer_data, int buffer_stride,
                             int width, int height, double,
                             const Theme& theme, const LayoutMetrics& layout,
                             const std::vector<FileEntry>& entries,
                             const ViewState& state) {
    renderer.render_into(buffer_data, buffer_stride, width, height, 1.0,
                        theme, layout, "", 0, {}, 0, -1);

    int items_y = layout.padding + layout.input_height + layout.padding;
    int cols = std::max(1, (width - 2 * layout.padding) / (ICON_SIZE + ICON_PADDING));
    int cell_w = ICON_SIZE + ICON_PADDING;
    int cell_h = ICON_SIZE + ICON_TEXT_HEIGHT + ICON_PADDING;

    int total = static_cast<int>(entries.size());

    for (int i = 0; i < total; i++) {
        int col = i % cols;
        int row = i / cols;

        int item_x = layout.padding + col * cell_w;
        int item_y = items_y + row * cell_h;

        if (item_y + cell_h > height - layout.padding) break;

        bool is_selected = (i == state.selected_index) ||
                          std::find(state.selected_indices.begin(), state.selected_indices.end(), i) != state.selected_indices.end();

        render_icon(renderer, item_x, item_y, cell_w, cell_h, entries[i], is_selected, theme);
    }

    int total_rows = (total + cols - 1) / cols;
    int visible_rows = (height - items_y - layout.padding) / cell_h;
    if (total_rows > visible_rows) {
        renderer.draw_scrollbar(width - layout.padding - 8, items_y,
                               visible_rows * cell_h, total_rows, visible_rows,
                                state.scroll_offset / cols, theme.accent);
    }
}

int ColumnViewRenderer::item_height(int, const LayoutMetrics& layout) const {
    return layout.item_height;
}

int ColumnViewRenderer::items_per_page(int, int height, const LayoutMetrics& layout) const {
    return std::max(1, (height - 2 * layout.padding - layout.input_height) / layout.item_height);
}

int ColumnViewRenderer::hit_test(double x, double y, int, int,
                                const LayoutMetrics& layout, int) const {
    if (columns_.empty()) return -1;

    int items_y = layout.padding + layout.input_height + layout.padding;
    int col_idx = static_cast<int>((x - layout.padding) / (COLUMN_WIDTH + COLUMN_PADDING));

    if (col_idx < 0 || col_idx >= static_cast<int>(columns_.size())) return -1;

    int row = static_cast<int>((y - items_y) / layout.item_height);
    if (row < 0 || row >= static_cast<int>(columns_[col_idx].size())) return -1;

    return row;
}

void ColumnViewRenderer::set_columns(const std::vector<std::vector<FileEntry>>& columns) {
    columns_ = columns;
    column_selected_.resize(columns_.size(), -1);
}

void ColumnViewRenderer::set_column_selected(int column, int index) {
    if (column >= 0 && column < static_cast<int>(column_selected_.size())) {
        column_selected_[column] = index;
    }
}

void ColumnViewRenderer::render_column(Renderer& renderer, int x, int y, int w, int h,
                                      const std::vector<FileEntry>& entries, int selected_index,
                                      const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int visible = std::min(static_cast<int>(entries.size()), h / 24);
    for (int i = 0; i < visible; i++) {
        int item_y = y + i * 24;
        if (i == selected_index) {
            renderer.fill_rect(x, item_y, w, 24, theme.selection);
        }

        std::string prefix = entries[i].is_directory ? "▸ " : "  ";
        std::vector<TextSegment> segments;
        segments.push_back({prefix, theme.text_muted});
        segments.push_back({entries[i].name, theme.foreground});
        renderer.draw_text_segments(x + 4, item_y + 4, segments, theme.result_font);
    }
}

void ColumnViewRenderer::render(Renderer& renderer, uint8_t* buffer_data, int buffer_stride,
                               int width, int height, double,
                               const Theme& theme, const LayoutMetrics& layout,
                               const std::vector<FileEntry>& entries,
                               const ViewState& state) {
    renderer.render_into(buffer_data, buffer_stride, width, height, 1.0,
                        theme, layout, "", 0, {}, 0, -1);

    int items_y = layout.padding + layout.input_height + layout.padding;
    int available_w = width - 2 * layout.padding;
    int available_h = height - items_y - layout.padding;

    if (columns_.empty() && !entries.empty()) {
        columns_.push_back(entries);
        column_selected_.push_back(state.selected_index);
    }

    int x = layout.padding;
    for (size_t c = 0; c < columns_.size(); c++) {
        int col_w = std::min(COLUMN_WIDTH, available_w - x + layout.padding);
        if (col_w <= 0) break;

        render_column(renderer, x, items_y, col_w, available_h,
                      columns_[c], column_selected_[c], theme);
        x += col_w + COLUMN_PADDING;
    }
}

int GalleryViewRenderer::item_height(int, const LayoutMetrics&) const {
    return THUMB_SIZE + 40;
}

int GalleryViewRenderer::items_per_page(int width, int height, const LayoutMetrics& layout) const {
    int cols = std::max(1, (width - 2 * layout.padding) / (THUMB_SIZE + THUMB_PADDING));
    int rows = std::max(1, (height - 2 * layout.padding - layout.input_height - 200) /
                      (THUMB_SIZE + 40));
    return cols * rows;
}

int GalleryViewRenderer::hit_test(double x, double y, int width, int,
                                 const LayoutMetrics& layout, int total_entries) const {
    int items_y = layout.padding + layout.input_height + layout.padding + 200;
    int cols = std::max(1, (width - 2 * layout.padding) / (THUMB_SIZE + THUMB_PADDING));
    int cell_w = THUMB_SIZE + THUMB_PADDING;
    int cell_h = THUMB_SIZE + 40;

    int col = static_cast<int>((x - layout.padding) / cell_w);
    int row = static_cast<int>((y - items_y) / cell_h);

    if (col < 0 || col >= cols || row < 0) return -1;

    int idx = row * cols + col;
    if (idx >= 0 && idx < total_entries) return idx;
    return -1;
}

void GalleryViewRenderer::render_preview(Renderer& renderer, int x, int y, int w, int h,
                                        const FileEntry& entry, const Theme& theme) {
    renderer.fill_rect(x, y, w, h, theme.background_alt);

    int preview_h = h - 60;
    renderer.rounded_rect(x + 16, y + 16, w - 32, preview_h, theme.corner_radius, theme.background);

    std::vector<TextSegment> name_segments;
    name_segments.push_back({entry.name, theme.foreground});
    renderer.draw_text_segments(x + 16, y + preview_h + 24, name_segments, theme.result_font);

    std::vector<TextSegment> info_segments;
    info_segments.push_back({entry.display_size() + " - " + entry.file_kind(), theme.text_muted});
    renderer.draw_text_segments(x + 16, y + preview_h + 44, info_segments, theme.result_detail_font);
}

void GalleryViewRenderer::render_thumbnails(Renderer& renderer, int x, int y, int w, int h,
                                           const std::vector<FileEntry>& entries, int selected,
                                           const Theme& theme) {
    int cols = std::max(1, w / (THUMB_SIZE + THUMB_PADDING));
    int cell_w = THUMB_SIZE + THUMB_PADDING;
    int cell_h = THUMB_SIZE + 40;

    for (size_t i = 0; i < entries.size(); i++) {
        int col = i % cols;
        int row = i / cols;

        int thumb_x = x + col * cell_w;
        int thumb_y = y + row * cell_h;

        if (thumb_y + cell_h > y + h) break;

        bool is_selected = (static_cast<int>(i) == selected);

        if (is_selected) {
            renderer.rounded_rect(thumb_x, thumb_y, cell_w, cell_h,
                                 theme.corner_radius, theme.selection);
        }

        renderer.rounded_rect(thumb_x + 4, thumb_y + 4, THUMB_SIZE, THUMB_SIZE,
                             theme.corner_radius, theme.background_alt);

        std::vector<TextSegment> segments;
        segments.push_back({entries[i].name, theme.foreground});
        renderer.draw_text_segments(thumb_x + 4, thumb_y + THUMB_SIZE + 8,
                                   segments, theme.result_detail_font);
    }
}

void GalleryViewRenderer::render(Renderer& renderer, uint8_t* buffer_data, int buffer_stride,
                                int width, int height, double,
                                const Theme& theme, const LayoutMetrics& layout,
                                const std::vector<FileEntry>& entries,
                                const ViewState& state) {
    renderer.render_into(buffer_data, buffer_stride, width, height, 1.0,
                        theme, layout, "", 0, {}, 0, -1);

    int items_y = layout.padding + layout.input_height + layout.padding;

    int preview_h = 200;
    if (state.selected_index >= 0 && state.selected_index < static_cast<int>(entries.size())) {
        render_preview(renderer, layout.padding, items_y, width - 2 * layout.padding,
                      preview_h, entries[state.selected_index], theme);
    }

    int thumbnails_y = items_y + preview_h + 8;
    int thumbnails_h = height - thumbnails_y - layout.padding;

    render_thumbnails(renderer, layout.padding, thumbnails_y,
                     width - 2 * layout.padding, thumbnails_h,
                     entries, state.selected_index, theme);
}

} // namespace waylaunch
