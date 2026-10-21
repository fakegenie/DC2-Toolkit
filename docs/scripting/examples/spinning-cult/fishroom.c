// Mod spinning-cult: the Fish Room's script (the new map fishroom; mod.json gives it as the map's
// "script"). Brother Gill waits under the fish, turning with it; he gives Max the camera and a
// scrap of metal, asks him to take the fish's likeness and make a shape the town can hold (the
// gŷros, an invention), and to show it to them. When all five have turned they are here, turning
// round the fish, and when Max speaks to Gill he turns with them, and the fish turns faster, for
// good.

#include <event.h>
#include <disc.h>
#include "modkit/input.c"
#include "modkit/text.c"
#include "cult.h"
#include "speech.h"

PROGRAM(map_start, arrive_in_room);
PROGRAM(talk_or_dungeon_start, talk_in_room);
PROGRAM(LEAVE_FISH_ROOM, leave_room);
MAIN(do_nothing);

#define GILL 40
#define GILL_X 0.0
#define GILL_Z 70.0
#define FIRST_CONVERT 41

void do_nothing(void)
{
}

static void murmur_parn(void)
{
    modkit_say("The colours go round.");
}

static void murmur_blinkhorn(void)
{
    modkit_say("Move along. Move along.\nMove along.");
}

static void murmur_giles(void)
{
    modkit_say("Can you hear it?\nIt's getting faster.");
}

static void murmur_aegira(void)
{
    modkit_say("He's waiting for you.");
}

static void murmur_fournet(void)
{
    modkit_say("...");
}

// The five, when all have turned: in a ring round the fish, 250 from the middle, the side towards
// the door left open; their packs, where they stand, and what they murmur if Max speaks to them.
struct ring_place {
    string pack;
    float x;
    float z;
    void (*murmur)(void);
};

struct ring_place ring[CONVERT_COUNT] = {
    {"chara/p01_10a.chr", 216.5, 125.0, murmur_parn},
    {"chara/p01_05a.chr", 191.5, -160.7, murmur_blinkhorn},
    {"chara/p01_19a.chr", 0.0, -250.0, murmur_giles},
    {"chara/p01_20a.chr", -191.5, -160.7, murmur_aegira},
    {"chara/p01_22a.chr", -216.5, 125.0, murmur_fournet},
};

static void place_player_at_door(void)
{
    place_chara(CHARA_PLAYER, 0.0, 0.0, ROOM_ARRIVE_Z, 3.14);
    set_chara_motion(CHARA_PLAYER, MOTION_STAND_2, CHARA_MOTION_RESTART);
    update_chara_motion(CHARA_PLAYER);
    reset_chara_cloth(CHARA_PLAYER);
    reset_camera(CAMERA_RESET_BEHIND, 0.0, 10.0);
}

// Gill as he waits: under the fish, his back to the door, turning slowly with it.
static void load_gill(void)
{
    load_chara(MEMORY_STACK_EVENT, MOD_DIR "gill.chr", "info.cfg", GILL);
    set_chara_active(GILL);
    place_chara(GILL, GILL_X, 0.0, GILL_Z, 3.14);
    set_chara_motion(GILL, "spin");
}

// The five in their ring, turning.
static void load_ring(void)
{
    int which;

    for (which = 0; which < CONVERT_COUNT; which++) {
        load_chara(MEMORY_STACK_EVENT, ring[which].pack, "info.cfg", FIRST_CONVERT + which);
        set_chara_active(FIRST_CONVERT + which);
        place_chara(FIRST_CONVERT + which, ring[which].x, 0.0, ring[which].z, 0.0);
        set_chara_motion(FIRST_CONVERT + which, "spin");
    }
}

// Arriving: the room as the cult has it. Once all five have turned they are here too.
void arrive_in_room(void)
{
    union cult_state state;

    state = read_cult();
    init_event();
    reset_memory_stack(MEMORY_STACK_EVENT);
    place_player_at_door();
    stop_bgm(0);
    allow_bgm_load();
    load_bgm(MUSIC_TURNING);
    play_bgm(0);
    load_gill();
    if (state.bits.turned == ALL_TURNED) {
        load_ring();
    }
    fade_in(60);
    finish_event();
}

// --- Gill ------------------------------------------------------------------------------------

// He turns his head before he turns round: too far, and too slowly.
static void gill_turns_round(void)
{
    set_chara_motion(GILL, "turn_head", CHARA_MOTION_HOLD);
    modkit_wait(70);
    face_chara(GILL, CHARA_PLAYER);
    face_chara(CHARA_PLAYER, GILL);
    set_chara_motion(CHARA_PLAYER, MOTION_STAND_2);
    set_chara_motion(GILL, "idle");
    close_on(GILL, CHARA_PLAYER, 80.0, 30.0);
    modkit_wait(30);
}

static void gill_turns_back(void)
{
    set_chara_rot(GILL, 0.0, 3.14, 0.0);
    set_chara_motion(GILL, "spin");
    reset_camera(CAMERA_RESET_BEHIND, 0.0, 10.0);
}

static void meet_gill(void)
{
    union cult_state state;

    gill_turns_round();
    say(GILL, "You came to watch it too.");
    say(GILL, "It was turning before\nthe town was built.\nIt will be turning after.");
    say(GILL, "They have all forgotten\nhow to look at it.");
    set_chara_motion(GILL, "beckon", CHARA_MOTION_HOLD);
    say(GILL, "Help them remember.");
    set_chara_motion(GILL, "idle");
    say(GILL, "Take its likeness first.\nThen give it a shape\nthey can hold.");
    add_item(ITEM_CAMERA, 1);
    add_item(ITEM_INSIDE_SCOOP_MEMO, 1);
    add_item(ITEM_PHOTO_ALBUM, 1);
    add_item(ITEM_SCRAP_OF_METAL, 1);
    modkit_say("Received a Camera, a notebook,\na photo album and a Scrap of Metal.");
    modkit_say("Set the Camera in an item slot\nto take photos with it.");
    state = read_cult();
    state.bits.stage = STAGE_MET;
    write_cult(state);
}

// What Gill would say while Max has no gŷros: the likeness, then the shape.
static void gill_hints(void)
{
    int turning = 0;
    int more = 0;
    int endless = 0;

    count_photos_of(IDEA_TURNING, PHOTO_SUBJECT_IDEA, &turning);
    count_photos_of(IDEA_TURNING + 1, PHOTO_SUBJECT_IDEA, &more);
    count_photos_of(IDEA_TURNING + 2, PHOTO_SUBJECT_IDEA, &endless);
    gill_turns_round();
    if (turning == 0 || more == 0 || endless == 0) {
        say(GILL, "Its likeness.\nAll of it. Watch it turn.");
    } else {
        say(GILL, "Yes. You have it now.");
        say(GILL, "Give it a shape they can hold.\nSomething that turns.");
    }
}

// What Gill says as they turn.
static void gill_counts(int count)
{
    gill_turns_round();
    switch (count) {
    case 0:
        say(GILL, "Show them.\nOne at a time.");
        break;
    case 1:
        say(GILL, "One remembers.");
        break;
    case 2:
        say(GILL, "Two.\nCan you feel the town\nleaning towards us?");
        break;
    case 3:
        say(GILL, "The lights have gone out\nup there. Good.");
        break;
    default:
        say(GILL, "One more.\nThen bring them to me.");
        break;
    }
}

// --- the end -----------------------------------------------------------------------------------

// Max turns with them. The camera leaves him turning and rises to the fish, which turns faster and
// faster, and stays there. It does not end.
static void turn_with_us(void)
{
    float yaw = 3.14;
    float max_speed = 0.0;
    float fish_speed = 0.026;  // radians a frame: the 1.5 degrees its point turned it
    float x;
    float y;
    float z;
    float px;
    float py;
    float pz;
    int frame;

    gill_turns_round();
    say(GILL, "They are all here now.");
    say(GILL, "All but one.");
    set_chara_motion(GILL, "beckon", CHARA_MOTION_HOLD);
    say(GILL, "Turn with us.");
    set_chara_motion(GILL, "raise", CHARA_MOTION_HOLD);
    get_chara_pos(CHARA_PLAYER, &px, &py, &pz);
    // the fish: its turning point off, turned from here on by this script
    set_func_point_enabled("fr_fish", "fish_spin", 0);
    bind_handle_map_part(0, "fr_fish");
    get_handle_rot(0, &x, &y, &z);
    set_camera_pos(px + 45.0, py + 26.0, pz - 50.0);
    set_camera_target(px, py + 18.0, pz);
    start_screen_wave(1.0, 0.03, 6.0, 600);
    for (frame = 0; ; frame++) {
        // Max, slowly at first
        if (max_speed < 0.22) {
            max_speed = max_speed + 0.0009;
        }
        yaw = yaw + max_speed;
        set_chara_rot(CHARA_PLAYER, 0.0, yaw, 0.0);
        // the fish, faster and faster
        if (fish_speed < 0.52) {
            fish_speed = fish_speed * 1.006;
        }
        y = y + fish_speed;
        if (y > 6.2832) {
            y = y - 6.2832;
        }
        set_handle_rot(0, x, y, z);
        if (frame == 240) {
            // the light goes red; the camera leaves him and rises to the fish
            set_map_light(ROOM_RED_LIGHT);
            move_camera_to(0.0, 40.0, 430.0, 0.0, FISH_HEIGHT, 0.0, 360);
        }
        WAIT();
    }
}

// Talking in the room: Gill, or one of the five once they are here.
void talk_in_room(void)
{
    int slot;
    union cult_state state;

    get_event_data(EVENT_DATA_CHARA_SLOT, &slot);
    state = read_cult();
    init_event();
    if (slot >= FIRST_CONVERT && slot < FIRST_CONVERT + CONVERT_COUNT) {
        ring[slot - FIRST_CONVERT].murmur();
        finish_event();
        return;
    }
    if (slot != GILL) {
        finish_event();
        return;
    }
    if (state.bits.turned == ALL_TURNED) {
        turn_with_us();
    } else if (state.bits.stage == STAGE_NONE) {
        meet_gill();
    } else if (get_item_count(ITEM_GYROS) == 0) {
        gill_hints();
    } else if (state.bits.stage == STAGE_MET) {
        gill_turns_round();
        say(GILL, "Yes.\nThat is its shape.");
        say(GILL, "Show it to them.\nThey will know what to do.");
        state.bits.stage = STAGE_GYROS;
        write_cult(state);
    } else {
        gill_counts(state.bits.count);
    }
    gill_turns_back();
    finish_event();
}

// The room's door (point door_out, given this program by mod.json's "programs").
void leave_room(void)
{
    jump_to_map(-1, "m01", BACK_FROM_FISH_ROOM);
    WAIT();
}
