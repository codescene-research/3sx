#if NETPLAY_ENABLED

#include "platform/netplay/netplay_stress.h"
#include "core/app.h"
#include "main.h"
#include "platform/netplay/netplay_base.h"
#include "sf33rd/AcrSDK/common/pad.h"
#include "sf33rd/Source/Game/debug/debug_config.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/menu/menu.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdbool.h>

#define STRESS_CHECK_DISTANCE_DEFAULT 8
#define GAME_STATE_FIGHT 2     // G_No[1] while a round is being played
#define STRESS_PAD_CONNECTED 2 // Interface_Type value keyConvert() uses for a present pad
#define BOOT_TRACE_NAME "boot-state-trace.csv"
#define STATE_TRACE_NAME "state-trace.csv"

static bool stress_pending = false;
static bool stress_running = false;
static bool stress_finished = false;
static bool stress_waiting_for_match = false;
static u32 stress_seed = 0;
static u32 stress_rng = 0;
static int stress_check_distance = 0;
static int stress_frame_limit = 0;
static int stress_frames_run = 0;
static int stress_desyncs = 0;
static int stress_boot_timer = 0;
static const char* stress_out_dir = ".";
static SDL_IOStream* boot_trace_io = NULL;
static SDL_IOStream* state_trace_io = NULL;

// Only the first re-simulation that disagrees is worth keeping.
static bool stress_pair_dumped = false;

static bool stress_motions = false;

#define MOTION_STEPS_MAX 8

/// One special-move motion, written for a player facing right. It is mirrored
/// at random when queued, because the generator does not look at which way the
/// player actually faces: half the motions come out backwards, which still
/// exercises the command parser.
typedef struct {
    u8 count;
    u8 charge_frames; // how long the first step is held, for charge moves
    u16 steps[MOTION_STEPS_MAX];
} StressMotion;

#define D SWK_DOWN
#define U SWK_UP
#define B SWK_LEFT
#define F SWK_RIGHT
static const StressMotion stress_motion_table[] = {
    { 3, 0, { D, D | F, F } },                     // quarter circle forward
    { 3, 0, { D, D | B, B } },                     // quarter circle back
    { 3, 0, { F, D, D | F } },                     // dragon punch
    { 5, 0, { B, D | B, D, D | F, F } },           // half circle forward
    { 5, 0, { F, D | F, D, D | B, B } },           // half circle back
    { 6, 0, { D, D | F, F, D, D | F, F } },        // double quarter circle forward (super arts)
    { 6, 0, { D, D | B, B, D, D | B, B } },        // double quarter circle back (super arts)
    { 2, 50, { D | B, F } },                       // back charge
    { 2, 50, { D, U } },                           // down charge
    { 7, 0, { F, D | F, D, D | B, B, U | B, U } }, // 360
    { 3, 0, { F, 0, F } },                         // dash
    { 3, 0, { B, 0, B } },                         // back dash
};
#undef D
#undef U
#undef B
#undef F

static const u16 stress_punches[] = { SWK_WEST, SWK_NORTH, SWK_RIGHT_SHOULDER };
static const u16 stress_kicks[] = { SWK_SOUTH, SWK_EAST, SWK_RIGHT_TRIGGER };

typedef struct {
    u16 inputs[MOTION_STEPS_MAX];
    u8 frames[MOTION_STEPS_MAX];
    int count;
    int next;
} QueuedMotion;

static QueuedMotion queued_motion[PLAYER_COUNT];

static void queue_motion(int player);
static bool next_motion_input(int player, u16* input);

void Stress_Path(char* dst, size_t size, const char* relative) {
    SDL_snprintf(dst, size, "%s/%s", stress_out_dir, relative);
}

/// A stress run is unattended, so its trace goes to a file rather than stdout.
void Stress_Trace(const char* fmt, ...) {
    char path[512];
    Stress_Path(path, sizeof(path), "stress-trace.log");

    SDL_IOStream* io = SDL_IOFromFile(path, "a");

    if (io == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    SDL_IOvprintf(io, fmt, args);
    va_end(args);

    SDL_IOprintf(io, "\n");
    SDL_CloseIO(io);
}

static u32 stress_random() {
    // xorshift32, so a seed reproduces the exact same run.
    stress_rng ^= stress_rng << 13;
    stress_rng ^= stress_rng >> 17;
    stress_rng ^= stress_rng << 5;
    return stress_rng;
}

/// Holds presses for a few frames at a time, which exercises moves and cancels
/// rather than one-frame noise.
u16 Stress_NextInput(int player) {
    static const u16 directions[] = { 0,
                                      SWK_UP,
                                      SWK_DOWN,
                                      SWK_LEFT,
                                      SWK_RIGHT,
                                      SWK_UP | SWK_LEFT,
                                      SWK_UP | SWK_RIGHT,
                                      SWK_DOWN | SWK_LEFT,
                                      SWK_DOWN | SWK_RIGHT };

    static const u16 attacks[] = { SWK_WEST,          SWK_NORTH, SWK_RIGHT_SHOULDER,
                                   SWK_LEFT_SHOULDER, SWK_SOUTH, SWK_RIGHT_TRIGGER };

    static u16 held[PLAYER_COUNT] = { 0, 0 };
    static int hold_frames[PLAYER_COUNT] = { 0, 0 };

    if (hold_frames[player] > 0) {
        hold_frames[player] -= 1;
        return held[player];
    }

    if (stress_motions && stress_running) {
        u16 motion_input;

        if (next_motion_input(player, &motion_input)) {
            return motion_input;
        }

        if (stress_random() % 4 == 0) {
            queue_motion(player);
            next_motion_input(player, &motion_input);
            return motion_input;
        }
    }

    u16 input = directions[stress_random() % SDL_arraysize(directions)];

    if (stress_random() % 3 == 0) {
        input |= attacks[stress_random() % SDL_arraysize(attacks)];
    }

    held[player] = input;
    hold_frames[player] = (int)(stress_random() % 6);

    return input;
}

static u16 mirror_directions(u16 input) {
    const u16 left = input & SWK_LEFT;
    const u16 right = input & SWK_RIGHT;
    input &= ~(SWK_LEFT | SWK_RIGHT);

    if (left) {
        input |= SWK_RIGHT;
    }

    if (right) {
        input |= SWK_LEFT;
    }

    return input;
}

/// The finishing button: one punch or kick, or two of a kind for an EX move.
static u16 motion_buttons() {
    const u16* buttons = (stress_random() % 2 == 0) ? stress_punches : stress_kicks;
    const u32 first = stress_random() % 3;
    u16 pressed = buttons[first];

    if (stress_random() % 4 == 0) {
        pressed |= buttons[(first + 1 + stress_random() % 2) % 3];
    }

    return pressed;
}

static void queue_motion(int player) {
    const StressMotion* motion = &stress_motion_table[stress_random() % SDL_arraysize(stress_motion_table)];
    const bool mirrored = stress_random() % 2 == 0;
    QueuedMotion* queue = &queued_motion[player];

    for (int i = 0; i < motion->count; i++) {
        const u16 direction = motion->steps[i];
        queue->inputs[i] = mirrored ? mirror_directions(direction) : direction;
        queue->frames[i] = (i == 0 && motion->charge_frames > 0) ? motion->charge_frames : 1 + stress_random() % 2;
    }

    // Dashes are two taps; everything else finishes on a button.
    const bool is_dash = motion->count == 3 && motion->steps[1] == 0;

    if (!is_dash) {
        queue->inputs[motion->count - 1] |= motion_buttons();
    }

    queue->count = motion->count;
    queue->next = 0;
}

/// The next frame of a queued motion, or false when none is in progress.
static bool next_motion_input(int player, u16* input) {
    QueuedMotion* queue = &queued_motion[player];

    if (queue->next >= queue->count) {
        return false;
    }

    *input = queue->inputs[queue->next];
    queue->frames[queue->next] -= 1;

    if (queue->frames[queue->next] == 0) {
        queue->next += 1;
    }

    return true;
}

void Stress_SetMotions(bool enabled) {
    stress_motions = enabled;
}

static const char* const stress_character_names[] = {
    "Gill", "Alex", "Ryu",  "Yun",   "Dudley", "Necro",   "Hugo",   "Ibuki", "Elena",  "Oro",
    "Yang", "Ken",  "Sean", "Urien", "Akuma",  "Chun-Li", "Makoto", "Q",     "Twelve", "Remy",
};

// The stages the game itself draws from in Random_Stage_Data: 0-19 without 17,
// which is Q's slot. Q has no stage of his own and fights on his opponent's.
#define STRESS_STAGE_COUNT 20
#define STRESS_STAGE_UNUSED 17

/// A name from stress_character_names, or a plain number below `limit`.
/// Returns -1 when the text is neither.
static int parse_stress_id(const char* text, const char* const* names, int name_count, int limit) {
    for (int i = 0; i < name_count; i++) {
        if (SDL_strcasecmp(text, names[i]) == 0) {
            return i;
        }
    }

    char* end = NULL;
    const long value = SDL_strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < 0 || value >= limit) {
        return -1;
    }

    return (int)value;
}

/// Forces the fight's characters and stage through the debug overrides the game
/// already honours, so a seed's matchup is chosen rather than left to the cursor.
bool Stress_SetMatchup(const char* p1, const char* p2, const char* stage) {
#if DEBUG
    const char* players[PLAYER_COUNT] = { p1, p2 };

    for (int i = 0; i < PLAYER_COUNT; i++) {
        if (players[i] == NULL) {
            continue;
        }

        const int id = parse_stress_id(players[i], stress_character_names, SDL_arraysize(stress_character_names), 20);

        if (id < 0) {
            SDL_Log("Unknown character for player %d: %s", i + 1, players[i]);
            return false;
        }

        // Gill is the boss, not on the select screen, and controlling him reads
        // past the end of the player command tables.
        if (id == 0) {
            SDL_Log("Gill can't be a player character.");
            return false;
        }

        debug_config.character_override[i] = id + 1;
    }

    if (stage != NULL) {
        const int id = parse_stress_id(stage, NULL, 0, STRESS_STAGE_COUNT);

        if (id < 0 || id == STRESS_STAGE_UNUSED) {
            SDL_Log("Unknown stage: %s", stage);
            return false;
        }

        debug_config.stage_override = id + 1;
    }

    return true;
#else
    if (p1 != NULL || p2 != NULL || stage != NULL) {
        SDL_Log("Forcing a matchup needs a Debug build.");
        return false;
    }

    return true;
#endif
}

void Stress_SetOutputDir(const char* directory) {
    if (directory != NULL) {
        stress_out_dir = directory;
    }
}

static void initialize_state_trace(const char* name, SDL_IOStream** stream) {
    char path[512];
    Stress_Path(path, sizeof(path), name);

    *stream = SDL_IOFromFile(path, "w");

    if (*stream != NULL) {
        SDL_IOprintf(*stream, "frame,checksum\n");
        SDL_FlushIO(*stream);
    }
}

static void append_state_trace(SDL_IOStream* stream, int frame, u32 checksum) {
    if (stream == NULL) {
        return;
    }

    SDL_IOprintf(stream, "%d,%08X\n", frame, checksum);
    SDL_FlushIO(stream);
}

static void close_state_traces() {
    if (boot_trace_io != NULL) {
        SDL_CloseIO(boot_trace_io);
        boot_trace_io = NULL;
    }

    if (state_trace_io != NULL) {
        SDL_CloseIO(state_trace_io);
        state_trace_io = NULL;
    }
}

void Stress_Begin(int seed, int check_distance, int frames) {
    // A zero seed would make xorshift produce nothing but zeroes.
    stress_seed = seed != 0 ? (u32)seed : 1;
    stress_rng = stress_seed;

    stress_check_distance = check_distance > 0 ? check_distance : STRESS_CHECK_DISTANCE_DEFAULT;
    stress_frame_limit = frames;
    stress_pending = true;

    initialize_state_trace(BOOT_TRACE_NAME, &boot_trace_io);
    initialize_state_trace(STATE_TRACE_NAME, &state_trace_io);

#if DEBUG
    // Both simulations of a frame have to still be in the buffer when the desync
    // is reported, which is check_distance frames after the fact.
    if (stress_check_distance >= STATE_BUFFER_MAX) {
        stress_check_distance = STATE_BUFFER_MAX - 1;
        SDL_Log("Clamped stress check distance to %d.", stress_check_distance);
    }
#endif
}

void Stress_RecordBootState(u32 checksum) {
    append_state_trace(boot_trace_io, stress_boot_timer, checksum);
}

void Stress_RecordState(int frame, u32 checksum) {
    if (!stress_running) {
        return;
    }

    append_state_trace(state_trace_io, frame, checksum);
}

bool Stress_IsRequested() {
    return stress_pending || stress_waiting_for_match || stress_running;
}

bool Stress_IsRunning() {
    return stress_running;
}

bool Stress_IsFinished() {
    return stress_finished;
}

void Stress_CreateSession(GekkoConfig* config, GekkoSession** session, int* player_handle) {
    config->check_distance = stress_check_distance;

    if (!gekko_create(session, GekkoStressSession)) {
        SDL_Log("Session is already running! probably incorrect.");
        return;
    }

    gekko_start(*session, config);

    for (int i = 0; i < PLAYER_COUNT; i++) {
        gekko_add_actor(*session, GekkoLocalPlayer, NULL);
    }

    *player_handle = 0;

    Stress_Trace(
        "session created (seed %u, check distance %d, frame limit %d)",
        stress_rng,
        stress_check_distance,
        stress_frame_limit
    );
}

static bool game_is_in_a_fight() {
    return G_No[1] == GAME_STATE_FIGHT;
}

void Stress_InjectBootInput() {
    if (!Stress_IsRequested()) {
        return;
    }

    // Both players are simulated, but only port 1 has real hardware behind it.
    // keyConvert() clears the other port every frame, which makes the game ask
    // for a controller to be reconnected mid-match.
    Interface_Type[0] = STRESS_PAD_CONNECTED;
    Interface_Type[1] = STRESS_PAD_CONNECTED;

    if (stress_running) {
        // The session drives the inputs from here on.
        return;
    }

    stress_boot_timer += 1;

    if (stress_boot_timer % 60 == 0) {
        Stress_Trace(
            "driving menus, frame %d: menu cond %d, G_No %d/%d/%d",
            stress_boot_timer,
            task[TASK_MENU].condition,
            G_No[0],
            G_No[1],
            G_No[2]
        );
    }

    if (stress_pending) {
        // Tap rather than hold: the screens react to a press, not to the button
        // being down, and holding it would only ever produce one edge.
        if ((stress_boot_timer / 4) % 2 == 0) {
            p1sw_buff |= SWK_START;
        }

        return;
    }

    p1sw_buff |= Stress_NextInput(0);
    p2sw_buff |= Stress_NextInput(1);

    if ((stress_boot_timer / 8) % 2 == 0) {
        p1sw_buff |= SWK_START;
        p2sw_buff |= SWK_START;
    }
}

void Stress_Tick() {
    if (stress_pending) {
        if (task[TASK_MENU].condition != 1) {
            return;
        }

        Stress_Trace("entering versus mode from G_No %d/%d/%d", G_No[0], G_No[1], G_No[2]);

        stress_pending = false;
        stress_waiting_for_match = true;
        NetplayBase_SetupVsMode();

        // Character select is driven by the pads, not by the session, so run it
        // as a local versus match.
        Mode_Type = MODE_VERSUS;
        return;
    }

    if (!stress_waiting_for_match || !game_is_in_a_fight()) {
        return;
    }

    // Starting at the fight keeps the rollback away from character select and
    // stage loading, which are far too expensive to re-simulate every frame.
    Stress_Trace("fight reached, creating session at G_No %d/%d/%d", G_No[0], G_No[1], G_No[2]);

    stress_waiting_for_match = false;
    stress_running = true;

    // Menu and asset-loading duration can vary between platforms and runs.
    // Start gameplay from the requested seed so those timing differences do
    // not change the generated fight inputs.
    stress_rng = stress_seed;

    // Both sides have to read their lever from the session. A CPU-controlled
    // player derives it from cpu_algorithm() instead, whose state isn't part of
    // the saved State, so it would diverge on every rollback.
    for (int i = 0; i < PLAYER_COUNT; i++) {
        Operator_Status[i] = 1;
        plw[i].wu.operator = 1;
    }

    NetplayBase_StartStressSession();
}

void Stress_OnFrameAdvanced() {
    stress_frames_run += 1;

    if (!game_is_in_a_fight()) {
        // The texture cache is not part of the saved State, and the screens after
        // a match release and reload it. Rolling back across that boundary makes
        // the game draw a player from an MTS slot that has since been freed.
        Stress_Trace("exiting: match ended after %d frames, %d desync(s)", stress_frames_run, stress_desyncs);
        stress_finished = true;
        close_state_traces();
        App_Exit();
        return;
    }

    if (stress_frames_run % 60 == 0) {
        Stress_Trace("stress frame %d (%d desync(s))", stress_frames_run, stress_desyncs);
    }

    if (stress_frame_limit > 0 && stress_frames_run >= stress_frame_limit) {
        Stress_Trace("exiting: frame limit reached after %d frames, %d desync(s)", stress_frames_run, stress_desyncs);
        stress_finished = true;
        close_state_traces();
        App_Exit();
    }
}

void Stress_OnDesync(int frame) {
    stress_desyncs += 1;
    Stress_Trace("desync at frame %d after %d frames", frame, stress_frames_run);

#if DEBUG
    if (!stress_pair_dumped) {
        // The divergence was outside the window the save hook watches.
        stress_pair_dumped = true;
        NetplayBase_DumpDesyncPair(frame);
    }
#endif

    // The dumps are left for compare_states.py.
    Stress_Trace("exiting: desync found after %d frames", stress_frames_run);
    stress_finished = true;
    close_state_traces();
    App_Exit();
}

void Stress_OnResimulationDiverged(int frame, u32 first_checksum, u32 resim_checksum) {
#if DEBUG
    if (stress_pair_dumped) {
        return;
    }

    // Catch the divergence here rather than when the session reports it: by then
    // this frame has been re-simulated several more times and the buffer may
    // hold a pass that happens to agree.
    stress_pair_dumped = true;
    NetplayBase_DumpDesyncPair(frame);
    Stress_Trace("re-simulation of frame %d diverged (0x%X vs 0x%X)", frame, first_checksum, resim_checksum);
#endif
}

#endif // NETPLAY_ENABLED
