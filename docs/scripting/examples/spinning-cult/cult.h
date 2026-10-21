// Mod spinning-cult: what Palm Brinks' patch (m01.c) and the Fish Room's script (fishroom.c) share.
//
// The cult's whole state is one save counter (NEW_COUNTER: 16 bits the save keeps), read and
// written through a union with a struct of bit-fields, so each part has a name.

// Program numbers build gives the mod (kept in the mods folder's mods.lock).
NEW_PROGRAM(ENTER_FISH_ROOM);     // m01: the new door's program (mod.json gives it to point door_fish)
NEW_PROGRAM(BACK_FROM_FISH_ROOM); // m01: arriving from the Fish Room, in front of that door
NEW_PROGRAM(LEAVE_FISH_ROOM);     // fishroom: the room's door (point door_out, by "programs")

NEW_COUNTER(CULT_COUNTER);

// What happened so far (stage).
#define STAGE_NONE 0      // nobody has met Brother Gill
#define STAGE_MET 1       // Gill gave Max the camera and the scrap
#define STAGE_GYROS 2     // Max has made the gŷros; Gill told him to show it

struct cult_bits {
    unsigned int turned : 5;      // one bit a convert, as CONVERTS below
    unsigned int stage : 3;       // STAGE_*
    unsigned int town_shown : 1;  // the door was shown on the first arrival
    unsigned int dark : 1;        // Palm Brinks has gone dark (three turned)
    unsigned int count : 3;       // how many have turned
};

// The same fields as plain bits, for Palm Brinks' script, which has little room (a map's script
// shares a 320,000-byte buffer with the interpreter's tables, and Palm Brinks' is near it): bit-
// fields are packed from the lowest bit, so these match struct cult_bits.
#define CULT_STAGE(cult) (((cult) >> 5) & 7)
#define CULT_DARK_BIT (1 << 9)
#define CULT_COUNT(cult) (((cult) >> 10) & 7)
#define CULT_ONE_MORE (1 << 10)

union cult_state {
    int raw;
    struct cult_bits bits;
};

static union cult_state read_cult(void)
{
    union cult_state state;

    state.raw = get_counter(CULT_COUNTER);
    return state;
}

static void write_cult(union cult_state state)
{
    set_counter(CULT_COUNTER, state.raw);
}

#define ALL_TURNED 31

// The five who can be turned, in bit order: their townsperson id and the pack they wear (the
// Fish Room loads the packs for the ending; make_villagers.py gave each a "spin" and a "stare").
#define CONVERT_COUNT 5

// The new item the invention makes (mod.json's data:items and data:inventions).
#define ITEM_GYROS 429
// The fish's ideas (mod.json's ideas; the Fish Room's invent boxes fish_idea_1 to fish_idea_3,
// which a photo of the fish gives by where it is in its turn).
#define IDEA_TURNING 251
#define IDEA_COUNT 3

// Where make_mod.py puts Palm Brinks' new door: the closed door ("door10") of the house nearest to
// where Palm Brinks puts the player, whose point it takes over. Its front faces -Z.
#define TOWN_DOOR_X 139.869
#define TOWN_DOOR_Y 6.49539
#define TOWN_DOOR_Z 142.74

// The Fish Room (fish_room.py builds it): floor at 0, walls at +-900, the door in the +Z
// wall, the fish floating 230 over the middle.
#define ROOM_ARRIVE_Z 790.0
#define FISH_HEIGHT 230.0

// Music the mod adds: the Fish Room's tune gone wrong.
#define MUSIC_TURNING 201

// Palm Brinks' extra light set (make_mod.py adds it to m01's scene): dark, a dried-blood fog.
#define TOWN_DARK_LIGHT 8
// The Fish Room's light sets: 0 the dark room, 1 red, for the end.
#define ROOM_RED_LIGHT 1
