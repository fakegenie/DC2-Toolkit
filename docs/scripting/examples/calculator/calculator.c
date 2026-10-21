// Mod calculator: a desk calculator stands in Barga's Valley. Walk up to it, press the action
// button at the "!" and it opens: a calculator face with a cursor on its keys, an LCD of our own
// digits, and sound effects for every key.
//
// It is a patch of every language of map s17's script. It adds the start program the mod's
// "dev": {"start_at"} warps a --dev build's new game to, and replaces program 500, which runs
// when the player presses the action button at the map's "talk" point (the Spheda trial's, unused
// while the mod's game starts here). Its files come from mod.json's "files" (MOD_DIR "calc.chr",
// the model; MOD_DIR "calc_ui.img", the face, cursor and digits) and the sounds, added to bank
// SP_042, which nothing else loads.
#pragma target map s17

#include <event.h>
#include <pal.h>
#include <disc.h>
#include "modkit/input.c"
#include "modkit/text.c"

// The logic: a cursor on the key grid of ui/calc_body.png, the keys, and an 8-digit integer
// display. It only changes the state; use_calculator below draws it and plays the sound each
// press asks for.

#define CALC_MAX 99999999

// Keys: the digits are their own values.
#define KEY_ADD 10
#define KEY_SUB 11
#define KEY_MUL 12
#define KEY_DIV 13
#define KEY_EQUALS 14
#define KEY_CLEAR 15
#define KEY_ERASE 16
#define KEY_NEG 17
#define KEY_CLOSE 18

#define OP_NONE 0
#define OP_ADD 1
#define OP_SUB 2
#define OP_MUL 3
#define OP_DIV 4

// The state, one array so the functions can change it.
#define CALC_TOTAL 0
#define CALC_ENTRY 1
#define CALC_OP 2
#define CALC_ERROR 3
#define CALC_TYPED 4
#define CALC_ROW 5
#define CALC_COL 6
#define CALC_STATE 7

#define CALC_ROWS 5
#define CALC_COLUMNS 4

// What a press did, so the caller knows what to redraw and which sound to play.
#define CALC_IGNORED 0
#define CALC_MOVED 1
#define CALC_KEYED 2
#define CALC_EQUALED 3
#define CALC_FAILED 4
#define CALC_CLEARED 5
#define CALC_CLOSE 6

// The key at a grid cell, as ui/calc_layout.json lays them out; the 0 key spans the first two
// columns of the last row.
int calc_key_at(int row, int col)
{
    switch (row) {
    case 0:
        return col == 0 ? KEY_CLOSE : col == 1 ? KEY_CLEAR : col == 2 ? KEY_ERASE : KEY_DIV;
    case 1:
        return col == 3 ? KEY_MUL : 7 + col;
    case 2:
        return col == 3 ? KEY_SUB : 4 + col;
    case 3:
        return col == 3 ? KEY_ADD : 1 + col;
    }
    return col <= 1 ? 0 : col == 2 ? KEY_NEG : KEY_EQUALS;
}

void calc_reset(int state[])
{
    state[CALC_TOTAL] = state[CALC_ENTRY] = state[CALC_ERROR] = state[CALC_TYPED] = 0;
    state[CALC_OP] = OP_NONE;
}

// A new calculator: cleared, the cursor on the 7 key.
void calc_start(int state[])
{
    calc_reset(state);
    state[CALC_ROW] = 1;
    state[CALC_COL] = 0;
}

// The number the display shows: the one being typed, else the total.
int calc_shown(int state[])
{
    return state[CALC_TYPED] ? state[CALC_ENTRY] : state[CALC_TOTAL];
}

// Moves the cursor one step, wrapping at the edges. The wide 0 key is one cell: moving onto its
// second column lands on its first, and moving right from it skips its second.
void calc_move(int state[], int pressed)
{
    int row = state[CALC_ROW];
    int col = state[CALC_COL];

    if (pressed & PAD_UP) {
        row = (row + CALC_ROWS - 1) % CALC_ROWS;
    } else if (pressed & PAD_DOWN) {
        row = (row + 1) % CALC_ROWS;
    } else if (pressed & PAD_LEFT) {
        col = (col + CALC_COLUMNS - 1) % CALC_COLUMNS;
        if (row == 4 && col == 1) {
            col = 0;
        }
    } else if (pressed & PAD_RIGHT) {
        col = (row == 4 && col == 0) ? 2 : (col + 1) % CALC_COLUMNS;
    }
    if (row == 4 && col == 1) {
        col = 0;
    }
    state[CALC_ROW] = row;
    state[CALC_COL] = col;
}

// The total after applying op to total and entry; with no op pending the entry becomes the total.
// error[0] is set on dividing by zero (which would stop the game's VM) or a result too long for
// the display.
int calc_apply(int total, int entry, int op, int error[])
{
    int result = entry;
    int size;
    int factor;

    switch (op) {
    case OP_ADD:
        result = total + entry;
        break;
    case OP_SUB:
        result = total - entry;
        break;
    case OP_MUL:
        // checked before multiplying: the game's ints are 32 bits, and the product could wrap
        size = total < 0 ? -total : total;
        factor = entry < 0 ? -entry : entry;
        if (factor > 1 && size > CALC_MAX / factor) {
            error[0] = 1;
            return total;
        }
        result = total * entry;
        break;
    case OP_DIV:
        if (entry == 0) {
            error[0] = 1;
            return total;
        }
        result = total / entry;
        break;
    }
    if (result > CALC_MAX || result < -CALC_MAX) {
        error[0] = 1;
        return total;
    }
    return result;
}

// Presses a key. Returns what happened, as CALC_KEYED for a plain key.
int calc_key(int state[], int key)
{
    int op;

    if (key == KEY_CLOSE) {
        return CALC_CLOSE;
    }
    if (key == KEY_CLEAR) {
        calc_reset(state);
        return CALC_CLEARED;
    }
    if (state[CALC_ERROR]) {
        return CALC_FAILED;
    }
    if (key <= 9) {
        if (!state[CALC_TYPED]) {
            state[CALC_ENTRY] = 0;
        }
        if (state[CALC_ENTRY] > CALC_MAX / 10 || state[CALC_ENTRY] < -CALC_MAX / 10) {
            return CALC_FAILED;
        }
        state[CALC_ENTRY] = state[CALC_ENTRY] < 0 ? state[CALC_ENTRY] * 10 - key : state[CALC_ENTRY] * 10 + key;
        state[CALC_TYPED] = 1;
        return CALC_KEYED;
    }
    if (key == KEY_ERASE) {
        if (!state[CALC_TYPED]) {
            return CALC_FAILED;
        }
        state[CALC_ENTRY] /= 10;
        return CALC_KEYED;
    }
    if (key == KEY_NEG) {
        if (state[CALC_TYPED]) {
            state[CALC_ENTRY] = -state[CALC_ENTRY];
        } else {
            state[CALC_TOTAL] = -state[CALC_TOTAL];
        }
        return CALC_KEYED;
    }
    op = key == KEY_ADD ? OP_ADD : key == KEY_SUB ? OP_SUB : key == KEY_MUL ? OP_MUL : key == KEY_DIV ? OP_DIV : OP_NONE;
    // an operator pressed again before typing only changes the pending one, so 7 + = keeps 7
    if (state[CALC_TYPED]) {
        int error[1] = {0};

        state[CALC_TOTAL] = calc_apply(state[CALC_TOTAL], state[CALC_ENTRY], state[CALC_OP], error);
        state[CALC_ERROR] = error[0];
        state[CALC_ENTRY] = state[CALC_TYPED] = 0;
    }
    state[CALC_OP] = op;
    if (state[CALC_ERROR]) {
        return CALC_FAILED;
    }
    return key == KEY_EQUALS ? CALC_EQUALED : CALC_KEYED;
}

// Applies one frame's pressed buttons (a get_pad_pressed bitmask): the D-pad moves the cursor,
// (X) presses the key under it, (O) and [select] close, [start] clears, (#) erases, and the
// shoulder buttons are the operators ([r1] + [l1] - [r2] x [l2] /), (^) is =.
int calc_press(int state[], int pressed)
{
    if (pressed & (PAD_CIRCLE | PAD_SELECT)) {
        return CALC_CLOSE;
    }
    if (pressed & PAD_START) {
        return calc_key(state, KEY_CLEAR);
    }
    if (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) {
        calc_move(state, pressed);
        return CALC_MOVED;
    }
    if (pressed & PAD_CROSS) {
        return calc_key(state, calc_key_at(state[CALC_ROW], state[CALC_COL]));
    }
    if (pressed & PAD_SQUARE) {
        return calc_key(state, KEY_ERASE);
    }
    if (pressed & PAD_TRIANGLE) {
        return calc_key(state, KEY_EQUALS);
    }
    if (pressed & PAD_R1) {
        return calc_key(state, KEY_ADD);
    }
    if (pressed & PAD_L1) {
        return calc_key(state, KEY_SUB);
    }
    if (pressed & PAD_R2) {
        return calc_key(state, KEY_MUL);
    }
    if (pressed & PAD_L2) {
        return calc_key(state, KEY_DIV);
    }
    return CALC_IGNORED;
}

// The program a --dev build's new game starts at (mod.json's "dev": {"start_at"}), numbered by build.
NEW_PROGRAM(CALC_START);
PROGRAM(CALC_START, start_calculator_valley);

// The map's "talk" point, where the calculator stands.
#define KIOSK_X 68.04
#define KIOSK_Y 41.85
#define KIOSK_Z 1180.61
#define KIOSK_SLOT 20

// The face is drawn at 1.5 times its 256x256 picture, centred on the 512x448 screen: picture
// pixel (x, y) is screen (FACE_LEFT + x * 3 / 2, FACE_TOP + y * 3 / 2).
#define FACE_LEFT 64
#define FACE_TOP 32
#define SPRITE_FACE 0
#define SPRITE_CURSOR 1
#define SPRITE_DIGIT 2
#define DISPLAY_CELLS 9
#define GLYPH_MINUS 10
#define GLYPH_E 11
#define GLYPH_BLANK 12
#define UI_IMAGE 1

// Program CALC_START: a new game with this mod begins at the calculator. The model is loaded on
// the stack that outlives the event, so it stays standing after finish_event.
void start_calculator_valley(void)
{
    init_event();
    set_func_point_enabled("", "exit", 0);
    set_func_point_enabled("", "branch", 0);
    set_func_point_enabled("p01_s17", "invent_30001-0", 0);
    reset_memory_stack(MEMORY_STACK_SUB_CHARA);
    load_chara(MEMORY_STACK_SUB_CHARA, MOD_DIR "calc.chr", "info.cfg", KIOSK_SLOT);
    set_chara_active(KIOSK_SLOT);
    place_chara(KIOSK_SLOT, KIOSK_X, KIOSK_Y, KIOSK_Z, 0.0);
    place_chara(0, KIOSK_X, KIOSK_Y, KIOSK_Z + 80.0, 3.14);
    set_camera_pos(KIOSK_X - 0.16, KIOSK_Y + 48.0, KIOSK_Z + 180.0);
    set_camera_target(KIOSK_X, KIOSK_Y + 20.0, KIOSK_Z + 60.0);
    reset_camera(CAMERA_RESET_ACTIVE);
    fade_in(30);
    modkit_wait(30);
    modkit_say("Welcome to Barga's Valley.\nThere is a calculator ahead.\nWalk up to it and press [cross] at the !");
    finish_event();
}

// Screen x and y of a point of the face picture.
int face_x(int x)
{
    return FACE_LEFT + x * 3 / 2;
}

int face_y(int y)
{
    return FACE_TOP + y * 3 / 2;
}

// Shows width by height of the UI picture texture at u, v, 1.5 times as large, with its top left
// at face picture point (x, y).
void place_sprite(int sprite, int u, int v, int width, int height, int x, int y)
{
    set_sprite_uv(sprite, u, v, width, height);
    set_sprite_size(sprite, width * 3 / 2, height * 3 / 2);
    set_sprite_pos(sprite, face_x(x) + width * 3 / 4, face_y(y) + height * 3 / 4);
}

void show_glyph(int cell, int glyph)
{
    place_sprite(SPRITE_DIGIT + cell, glyph * 16, 0, 16, 32, 40 + cell * 20, 37);
}

// The display: the number right-aligned in the cells, a minus before it, or E on an error.
void draw_display(int state[])
{
    int value = calc_shown(state);
    int negative = value < 0;
    int cell = DISPLAY_CELLS - 1;

    if (negative) {
        value = -value;
    }
    do {
        show_glyph(cell--, value % 10);
        value /= 10;
    } while (value > 0 && cell >= 0);
    if (negative && cell >= 0) {
        show_glyph(cell--, GLYPH_MINUS);
    }
    while (cell >= 0) {
        show_glyph(cell--, GLYPH_BLANK);
    }
    if (state[CALC_ERROR]) {
        show_glyph(0, GLYPH_E);
    }
}

// The cursor's frame around the key it is on (the wide one on the 0 key).
void draw_cursor(int state[], int block)
{
    int row = state[CALC_ROW];
    int col = state[CALC_COL];
    int x = 20 + col * 56;
    int y = 92 + row * 31;

    if (row == 4 && col == 0) {
        set_sprite_texture(SPRITE_CURSOR, "calc_cursor_wide", block);
        place_sprite(SPRITE_CURSOR, 0, 0, 128, 32, x - 2, y - 2);
    } else {
        set_sprite_texture(SPRITE_CURSOR, "calc_cursor", block);
        place_sprite(SPRITE_CURSOR, 0, 0, 64, 32, x - 2, y - 2);
    }
}

void open_sprite(int sprite, string texture, int block)
{
    init_screen_sprite(sprite, texture, block);
    set_sprite_alpha(sprite, 128);
    set_sprite_draw(sprite, EVENT_SPRITE2_DRAW_NORMAL);
}

// The sound for what a press did.
void play_press(int sound, int result)
{
    switch (result) {
    case CALC_MOVED:
    case CALC_KEYED:
        play_se(sound, SE_MOD_CALC_KEY);
        break;
    case CALC_EQUALED:
        play_se(sound, SE_MOD_CALC_EQUALS);
        break;
    case CALC_FAILED:
        play_se(sound, SE_MOD_CALC_ERROR);
        break;
    case CALC_CLEARED:
        play_se(sound, SE_MOD_CALC_CLEAR);
        break;
    }
}

// The calculator, until (O), [select] or its power key closes it.
void use_calculator(void)
{
    int state[CALC_STATE];
    int sound;
    int block;
    int result;
    int cell;

    init_event();
    show_status_bar(0, 1.0);
    load_sound(SND_PORT_MENU, "snd2/sp/sp_042.snd", &sound);
    reset_memory_stack(MEMORY_STACK_EVENT);
    load_image(MEMORY_STACK_EVENT, MOD_DIR "calc_ui.img", UI_IMAGE, &block);
    open_sprite(SPRITE_FACE, "calc_body", block);
    place_sprite(SPRITE_FACE, 0, 0, 256, 256, 0, 0);
    open_sprite(SPRITE_CURSOR, "calc_cursor", block);
    for (cell = 0; cell < DISPLAY_CELLS; cell++) {
        open_sprite(SPRITE_DIGIT + cell, "calc_digits", block);
    }
    calc_start(state);
    draw_display(state);
    draw_cursor(state, block);
    play_se(sound, SE_MOD_CALC_OPEN);
    // the press that opened it is not a key
    modkit_wait(10);
    while ((result = calc_press(state, get_pad_pressed())) != CALC_CLOSE) {
        if (result != CALC_IGNORED) {
            play_press(sound, result);
            draw_display(state);
            draw_cursor(state, block);
        }
        WAIT();
    }
    play_se(sound, SE_MOD_CALC_CLOSE);
    modkit_wait(10);
    delete_image(UI_IMAGE);
    show_status_bar(1, 1.0);
    finish_event();
}

// Program 500 (the "talk" point): the calculator instead of the Spheda trial.
#pragma override
int talk_to_monica_spheda_trial(void)
{
    use_calculator();
    return 0;
}
