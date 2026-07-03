#include "game_tetris.h"

#if defined(NRL_HAS_DISPLAY) && NRL_HAS_DISPLAY && NRL_BOARD == NRL_BOARD_S31_KORVO

#include <esp_random.h>

#include <stdio.h>
#include <string.h>

namespace {

constexpr int kCols = 10;
constexpr int kRows = 20;
constexpr int kCell = 21;           // 10*21=210 wide, 20*21=420 tall
constexpr int kBoardX = 60;
constexpr int kBoardY = 56;
constexpr uint32_t kDropStartMs = 700;
constexpr uint32_t kDropMinMs = 120;

// The 7 tetrominoes as 4 rotations x 4 cells (x,y offsets in a 4x4 box).
struct Piece {
    uint8_t cells[4][4][2];
    uint32_t color;
};

const Piece kPieces[7] = {
    // I
    {{{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}},
      {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}}, 0x22D3EE},
    // O
    {{{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}},
      {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}}, 0xFACC15},
    // T
    {{{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}},
      {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}}, 0xA78BFA},
    // S
    {{{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}},
      {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}}, 0x4ADE80},
    // Z
    {{{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}},
      {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}}, 0xF87171},
    // J
    {{{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}},
      {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}}, 0x60A5FA},
    // L
    {{{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}},
      {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}}, 0xFB923C},
};

constexpr uint32_t kColorEmpty = 0x101A2A;
constexpr uint32_t kColorGrid = 0x0B1220;

// Board state: 0 = empty, otherwise piece index + 1.
uint8_t s_board[kRows][kCols];
lv_obj_t *s_tiles[kRows][kCols];
lv_obj_t *s_preview[4][4];
// Colors currently painted on each tile: redraw() skips unchanged tiles, so a
// piece step repaints <10 tiles instead of restyling all 216 (each style set
// costs an LVGL style-refresh walk even when the render itself is batched).
uint32_t s_tile_shown[kRows][kCols];
uint32_t s_preview_shown[4][4];
lv_obj_t *s_lbl_score = nullptr;
lv_obj_t *s_lbl_over = nullptr;
lv_timer_t *s_timer = nullptr;
void (*s_exit_cb)(void) = nullptr;

int s_cur = 0, s_next = 0, s_rot = 0, s_x = 0, s_y = 0;
uint32_t s_score = 0;
uint32_t s_lines = 0;
bool s_game_over = false;

bool cellFree(const int px, const int py, const int rot, const int piece)
{
    for (int i = 0; i < 4; ++i) {
        const int x = px + kPieces[piece].cells[rot][i][0];
        const int y = py + kPieces[piece].cells[rot][i][1];
        if (x < 0 || x >= kCols || y >= kRows) {
            return false;
        }
        if (y >= 0 && s_board[y][x] != 0u) {
            return false;
        }
    }
    return true;
}

void paintTile(lv_obj_t *tile, uint32_t *shown, const uint32_t color)
{
    if (*shown == color) {
        return;
    }
    *shown = color;
    lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
}

void redraw()
{
    // Desired board colors: settled cells plus the falling piece.
    uint32_t want[kRows][kCols];
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            const uint8_t v = s_board[y][x];
            want[y][x] = (v != 0u) ? kPieces[v - 1u].color : kColorEmpty;
        }
    }
    if (!s_game_over) {
        for (int i = 0; i < 4; ++i) {
            const int x = s_x + kPieces[s_cur].cells[s_rot][i][0];
            const int y = s_y + kPieces[s_cur].cells[s_rot][i][1];
            if (y >= 0) {
                want[y][x] = kPieces[s_cur].color;
            }
        }
    }
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            paintTile(s_tiles[y][x], &s_tile_shown[y][x], want[y][x]);
        }
    }
    // Next-piece preview.
    uint32_t preview_want[4][4];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            preview_want[y][x] = kColorGrid;
        }
    }
    for (int i = 0; i < 4; ++i) {
        const int x = kPieces[s_next].cells[0][i][0];
        const int y = kPieces[s_next].cells[0][i][1];
        preview_want[y][x] = kPieces[s_next].color;
    }
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            paintTile(s_preview[y][x], &s_preview_shown[y][x], preview_want[y][x]);
        }
    }
    if (s_lbl_score != nullptr) {
        static uint32_t s_shown_score = 0xFFFFFFFFu;
        if (s_shown_score != s_score) {
            s_shown_score = s_score;
            char text[48];
            snprintf(text, sizeof(text), "Score %lu\nLines %lu",
                     static_cast<unsigned long>(s_score), static_cast<unsigned long>(s_lines));
            lv_label_set_text(s_lbl_score, text);
        }
    }
}

void spawn()
{
    s_cur = s_next;
    s_next = static_cast<int>(esp_random() % 7u);
    s_rot = 0;
    s_x = 3;
    s_y = -1;
    if (!cellFree(s_x, s_y + 1, s_rot, s_cur)) {
        s_game_over = true;
        if (s_lbl_over != nullptr) {
            lv_obj_remove_flag(s_lbl_over, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_timer != nullptr) {
            lv_timer_pause(s_timer);
        }
    }
}

void lockPiece()
{
    for (int i = 0; i < 4; ++i) {
        const int x = s_x + kPieces[s_cur].cells[s_rot][i][0];
        const int y = s_y + kPieces[s_cur].cells[s_rot][i][1];
        if (y >= 0) {
            s_board[y][x] = static_cast<uint8_t>(s_cur + 1);
        }
    }
    // Clear complete lines.
    int cleared = 0;
    for (int y = kRows - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < kCols; ++x) {
            if (s_board[y][x] == 0u) {
                full = false;
                break;
            }
        }
        if (full) {
            ++cleared;
            memmove(&s_board[1][0], &s_board[0][0], static_cast<size_t>(y) * kCols);
            memset(&s_board[0][0], 0, kCols);
            ++y; // re-check the same row after the shift
        }
    }
    if (cleared > 0) {
        static const uint16_t kLineScore[5] = {0, 100, 300, 500, 800};
        s_lines += static_cast<uint32_t>(cleared);
        s_score += kLineScore[(cleared > 4) ? 4 : cleared];
        // Speed up every 10 lines.
        if (s_timer != nullptr) {
            const uint32_t period = (kDropStartMs > s_lines / 10u * 60u)
                                        ? (kDropStartMs - s_lines / 10u * 60u)
                                        : kDropMinMs;
            lv_timer_set_period(s_timer, (period > kDropMinMs) ? period : kDropMinMs);
        }
    }
    spawn();
}

void stepDown()
{
    if (s_game_over) {
        return;
    }
    if (cellFree(s_x, s_y + 1, s_rot, s_cur)) {
        ++s_y;
    } else {
        lockPiece();
    }
    redraw();
}

void gravityTick(lv_timer_t *)
{
    stepDown();
}

void restart()
{
    memset(s_board, 0, sizeof(s_board));
    s_score = 0;
    s_lines = 0;
    s_game_over = false;
    if (s_lbl_over != nullptr) {
        lv_obj_add_flag(s_lbl_over, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_timer != nullptr) {
        lv_timer_set_period(s_timer, kDropStartMs);
        lv_timer_resume(s_timer);
    }
    s_next = static_cast<int>(esp_random() % 7u);
    spawn();
    redraw();
}

enum class Ctl : intptr_t { Left = 1, Right, Rotate, Down, Drop, Restart, Exit };

void controlEvent(lv_event_t *event)
{
    const Ctl id = static_cast<Ctl>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (id == Ctl::Exit) {
        void (*exit_cb)(void) = s_exit_cb;
        GAME_TETRIS_Teardown();
        if (exit_cb != nullptr) {
            exit_cb();
        }
        return;
    }
    if (id == Ctl::Restart) {
        restart();
        return;
    }
    if (s_game_over) {
        return;
    }
    switch (id) {
        case Ctl::Left:
            if (cellFree(s_x - 1, s_y, s_rot, s_cur)) { --s_x; }
            break;
        case Ctl::Right:
            if (cellFree(s_x + 1, s_y, s_rot, s_cur)) { ++s_x; }
            break;
        case Ctl::Rotate: {
            const int rot = (s_rot + 1) % 4;
            // Simple wall kick: try in place, then one step left/right.
            if (cellFree(s_x, s_y, rot, s_cur)) { s_rot = rot; }
            else if (cellFree(s_x - 1, s_y, rot, s_cur)) { s_rot = rot; --s_x; }
            else if (cellFree(s_x + 1, s_y, rot, s_cur)) { s_rot = rot; ++s_x; }
            break;
        }
        case Ctl::Down:
            stepDown();
            return; // stepDown already redraws
        case Ctl::Drop:
            while (cellFree(s_x, s_y + 1, s_rot, s_cur)) { ++s_y; }
            lockPiece();
            break;
        default:
            break;
    }
    redraw();
}

lv_obj_t *gameButton(lv_obj_t *parent, int x, int y, int w, int h, const char *text, Ctl id)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x101A2A), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1D4E63), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x24364D), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    // React on touch-down, not on release (CLICKED): release-triggered moves
    // feel like input lag in a game. Movement keys also auto-repeat on hold.
    void *user_data = reinterpret_cast<void *>(static_cast<intptr_t>(id));
    lv_obj_add_event_cb(btn, controlEvent, LV_EVENT_PRESSED, user_data);
    if (id == Ctl::Left || id == Ctl::Right || id == Ctl::Down) {
        lv_obj_add_event_cb(btn, controlEvent, LV_EVENT_LONG_PRESSED_REPEAT, user_data);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xE6EDF3), 0);
    lv_obj_center(lbl);
    lv_label_set_text(lbl, text);
    return btn;
}

} // namespace

extern "C" void GAME_TETRIS_Build(lv_obj_t *screen, void (*exit_cb)(void))
{
    s_exit_cb = exit_cb;

    // Playfield tiles.
    lv_obj_t *board = lv_obj_create(screen);
    lv_obj_set_pos(board, kBoardX, kBoardY);
    lv_obj_set_size(board, kCols * kCell + 4, kRows * kCell + 4);
    lv_obj_set_style_bg_color(board, lv_color_hex(kColorGrid), 0);
    lv_obj_set_style_border_color(board, lv_color_hex(0x24364D), 0);
    lv_obj_set_style_border_width(board, 2, 0);
    lv_obj_set_style_pad_all(board, 0, 0);
    lv_obj_set_style_radius(board, 0, 0);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            lv_obj_t *tile = lv_obj_create(board);
            lv_obj_set_pos(tile, x * kCell, y * kCell);
            lv_obj_set_size(tile, kCell - 1, kCell - 1);
            lv_obj_set_style_radius(tile, 2, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(kColorEmpty), 0);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            s_tiles[y][x] = tile;
            s_tile_shown[y][x] = kColorEmpty;
        }
    }

    // Next-piece preview + score.
    lv_obj_t *side = lv_obj_create(screen);
    lv_obj_set_pos(side, 320, kBoardY);
    lv_obj_set_size(side, 4 * kCell + 4, 4 * kCell + 4);
    lv_obj_set_style_bg_color(side, lv_color_hex(kColorGrid), 0);
    lv_obj_set_style_border_color(side, lv_color_hex(0x24364D), 0);
    lv_obj_set_style_border_width(side, 2, 0);
    lv_obj_set_style_pad_all(side, 0, 0);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            lv_obj_t *tile = lv_obj_create(side);
            lv_obj_set_pos(tile, x * kCell, y * kCell);
            lv_obj_set_size(tile, kCell - 1, kCell - 1);
            lv_obj_set_style_radius(tile, 2, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_set_style_bg_color(tile, lv_color_hex(kColorGrid), 0);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            s_preview[y][x] = tile;
            s_preview_shown[y][x] = kColorGrid;
        }
    }

    s_lbl_score = lv_label_create(screen);
    lv_obj_set_style_text_font(s_lbl_score, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_score, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_pos(s_lbl_score, 320, kBoardY + 4 * kCell + 20);
    lv_label_set_text(s_lbl_score, "Score 0\nLines 0");

    // Touch controls, thumb-sized on the right half.
    gameButton(screen, 470, 90, 150, 90, LV_SYMBOL_LEFT, Ctl::Left);
    gameButton(screen, 636, 90, 150, 90, LV_SYMBOL_RIGHT, Ctl::Right);
    gameButton(screen, 470, 196, 150, 90, LV_SYMBOL_REFRESH, Ctl::Rotate);
    gameButton(screen, 636, 196, 150, 90, LV_SYMBOL_DOWN, Ctl::Down);
    gameButton(screen, 470, 302, 316, 70, "DROP", Ctl::Drop);
    gameButton(screen, 470, 392, 150, 64, "Restart", Ctl::Restart);
    gameButton(screen, 636, 392, 150, 64, "Back", Ctl::Exit);

    s_lbl_over = lv_label_create(screen);
    lv_obj_set_style_text_font(s_lbl_over, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_over, lv_color_hex(0xF87171), 0);
    lv_obj_set_pos(s_lbl_over, kBoardX + 30, kBoardY + 180);
    lv_label_set_text(s_lbl_over, "GAME OVER");
    lv_obj_add_flag(s_lbl_over, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(gravityTick, kDropStartMs, nullptr);
    restart();
}

extern "C" void GAME_TETRIS_Teardown(void)
{
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    // Widgets are children of the screen; clearScreen()/lv_obj_clean owns
    // their deletion. Just forget the pointers.
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_preview, 0, sizeof(s_preview));
    s_lbl_score = nullptr;
    s_lbl_over = nullptr;
    s_exit_cb = nullptr;
}

#endif // S31
