#include <pebble.h>
#include <stdlib.h> // rand()/srand() for battle coin flips
#include <string.h> // strlen() for picking the name font

#define MAX_STAT 100
#define DECAY_PER_HOUR 4          // hunger up, energy down, per real DAYTIME hour passed
#define MESS_HOURS_THRESHOLD 6    // hours since last cleaning before a mess appears

// Day/night cycle - awake hours 6:00 to 22:00 (10pm).
#define DAY_START_HOUR 6
#define NIGHT_START_HOUR 22
#define NIGHT_ENERGY_REGEN_PER_HOUR 8
#define NIGHT_HUNGER_PER_HOUR 1

#define ATTENTION_HUNGER_THRESHOLD 70
#define ATTENTION_ENERGY_THRESHOLD 25

// Care Mistakes redesign: a mistake only happens if the main screen is
// open, a problem (needs_attention()) is visibly showing, AND it's still
// unresolved after this much real time with the app actually open -
// simply closing the app doesn't count, only genuinely ignoring a visible
// issue while looking at it does. Since hunger/energy/mess can only ever
// change via the launch-time decay pass or your own button presses (never
// while the app just sits open), the grace period only ever needs to
// start/stop at those two moments - see check_mistake_grace().
#define CARE_MISTAKE_GRACE_MS (90 * 1000)

// Evolution timing is per TIER (Baby I, Baby II, Child, Adult, Perfect,
// Mega), not per species - every form in a tier takes the same time. The
// tier's timer only decides WHEN an evolution is attempted; WHICH form you
// become is decided by the requirement table (EVO_EDGES) further down.
// Perfect's hours/steps are a first guess - tune freely.
static const int TIER_HOURS[6] = { 1, 23, 48, 96, 96, -1 };
static const int TIER_STEPS[6] = { 500, 5000, 15000, 30000, 30000, -1 };

// "PP" from the evolution tree = successful Trains + PP_PER_WIN per battle
// win, so wins feed it just like training does.
#define PP_PER_WIN 2
#define CARE_MISTAKES_CAP 3           // going OVER this at the Child stage forces Numemon
#define SUPER_ACTIVE_STEPS_MULTIPLIER 3 // tier steps * this = evolve early on steps alone (requirements still apply)

#define IDLE_BOB_MS 900
#define IDLE_BOB_OFFSET 2

// Evolution transition - full black screen with "Evolving..." text.
#define EVOLVE_DURATION_MS 10000

// Vital Points - a separate resource from hunger/energy/strength, gained
// purely from steps (not tied to feeding/training), inspired by the Vital
// Bracelet BE's Vital Value system. No spending mechanic yet - this is
// groundwork for a future battle feature, not a complete system on its own.
#define STEPS_PER_VITAL_POINT 100  // rough conversion, tune once you see real gain rates
#define MAX_VITAL_POINTS 100

// Manual sleep - real Vital Bracelet devices trigger this by holding two
// buttons together; we only have 3 buttons total (UP/DOWN already cycle
// the menu, SELECT already confirms), so this is adapted to a long-press
// on SELECT instead. Behavior is BE-style: while manually asleep,
// EVERYTHING freezes - hunger, energy, mess, evolution progress (time and
// steps), and Vital Point gain all pause completely until woken again.
// 1200ms (not a shorter value) deliberately, so it doesn't get triggered
// accidentally by a normal confirm tap running slightly long.
#define MANUAL_SLEEP_HOLD_MS 1200

// Stats screen - long-press on DOWN (the bottom button) opens a dedicated
// detail screen. Same 1200ms threshold as SELECT's manual-sleep hold, for
// the same reason: short enough to feel deliberate, long enough that a
// normal quick tap won't accidentally trigger it.
#define STATS_HOLD_MS 1200

// Procedural bezel margins - still used on Gabbro (and any platform
// without a custom border image). Emery uses the real DIGIVICE_BORDER
// art instead, with its own square cutout coordinates below.
#define BEZEL_TOP 16
#define BEZEL_SIDE 6
#define BEZEL_BOTTOM 6

// Real coordinates of the white square cutout in digisquare3.png
// (200x228 image, Emery-only).
#define SQUARE_LEFT 25
#define SQUARE_RIGHT 174
#define SQUARE_TOP 54
#define SQUARE_BOTTOM 174

// Estimated cutout coordinates for the round border (digiviceSmallRound.png,
// Gabbro-only, 260x260). Read by eye from a screenshot proportionally, NOT
// measured exact pixels like the Emery square above - expect this to need
// another real adjustment pass once you see it rendered.
#define GABBRO_SQUARE_LEFT 68
#define GABBRO_SQUARE_RIGHT 196
#define GABBRO_SQUARE_TOP 55
#define GABBRO_SQUARE_BOTTOM 183

// Fixed-height band reserved at the bottom of the content area for
// progress label + bar + stats + action text.
#define BOTTOM_UI_HEIGHT 56
#define TOP_UI_HEIGHT 20

// Battle system - same tier fights same tier (Botamon vs Botamon, etc.),
// resolved as round-by-round 50/50 coin flips rather than Rock-Paper-
// Scissors: with no type/element system yet to ground RPS choices in
// anything meaningful, a plain coin flip lets HP be the entire deciding
// factor instead - which is the point, since your own Vital Points
// (earned from steps) become bonus HP your opponent doesn't get.
#define BATTLE_DAMAGE_PER_HIT 25
#define BATTLE_VP_STAKE 15          // gained on a win, lost on a loss - symmetric, since the HP bonus is now capped and fair
// Each round now plays out in two visible phases: a projectile travels
// from attacker to defender (PROJECTILE_STEPS sub-frames), then on impact
// the defender flickers (FLICKER_STEPS sub-frames) before a brief pause
// and the next round begins. Damage is only actually applied at impact,
// so the HP bars visibly drop right as the projectile lands.
#define PROJECTILE_STEPS 5
#define PROJECTILE_STEP_MS 60
#define FLICKER_STEPS 4
#define FLICKER_STEP_MS 80
#define POST_ROUND_PAUSE_MS 150

// Species IDs 0-4 are deliberately the SAME numbers the old Stage enum
// used (Botamon, Koromon, Agumon, Greymon, MetalGreymon), so a pet saved
// by the previous version loads as the same Digimon (an old MetalGreymon
// is now a Perfect that can go on to BlitzGreymon). Never reorder these -
// only append.
typedef enum {
  SP_BOTA = 0,
  SP_KORO = 1,
  SP_AGU = 2,
  SP_GREY = 3,
  SP_METALGREY_VI = 4,
  SP_DOKI,
  SP_BIBI,
  SP_BETA,
  SP_PULSE,
  SP_TYRANO,
  SP_AIRDRA,
  SP_SEADRA,
  SP_MERA,
  SP_DEVI,
  SP_NUME,
  SP_KAZUCHI,
  SP_BLITZGREY,
  SP_MAME,
  SP_MONZAE,
  SP_BOUT,
  SP_BANCHOMAME,
  SP_SHINMONZAE,
  SP_BULK,
  SP_COUNT
} Species;

typedef enum {
  TIER_BABY1,
  TIER_BABY2,
  TIER_CHILD,
  TIER_ADULT,
  TIER_PERFECT,
  TIER_MEGA
} Tier;

typedef struct {
  const char *name;
  Tier tier;
} SpeciesInfo;

static const SpeciesInfo SPECIES[SP_COUNT] = {
  [SP_BOTA]        = { "Botamon",       TIER_BABY1 },
  [SP_DOKI]        = { "Dokimon",       TIER_BABY1 },
  [SP_KORO]        = { "Koromon",       TIER_BABY2 },
  [SP_BIBI]        = { "Bibimon",       TIER_BABY2 },
  [SP_AGU]         = { "Agumon",        TIER_CHILD },
  [SP_BETA]        = { "Betamon",       TIER_CHILD },
  [SP_PULSE]       = { "Pulsemon",      TIER_CHILD },
  [SP_GREY]        = { "Greymon",       TIER_ADULT },
  [SP_TYRANO]      = { "Tyranomon",     TIER_ADULT },
  [SP_AIRDRA]      = { "Airdramon",     TIER_ADULT },
  [SP_SEADRA]      = { "Seadramon",     TIER_ADULT },
  [SP_MERA]        = { "Meramon",       TIER_ADULT },
  [SP_DEVI]        = { "Devimon",       TIER_ADULT },
  [SP_NUME]        = { "Numemon",       TIER_ADULT },
  [SP_BULK]         = { "Bulkmon",       TIER_ADULT },
  [SP_METALGREY_VI]= { "MetalGreymon",  TIER_PERFECT },
  [SP_MAME]        = { "Mamemon",       TIER_PERFECT },
  [SP_MONZAE]      = { "Monzaemon",     TIER_PERFECT },
  [SP_BOUT]        = { "Boutmon",       TIER_PERFECT },
  [SP_BLITZGREY]   = { "BlitzGreymon",  TIER_MEGA },
  [SP_BANCHOMAME]  = { "BanchoMamemon", TIER_MEGA },
  [SP_SHINMONZAE]  = { "ShinMonzaemon", TIER_MEGA },
  [SP_KAZUCHI]     = { "Kazuchimon",    TIER_MEGA },
};

// One evolution path: from -> to, with the requirements read off the
// evolution tree. A value of 0 means N/A (always satisfied). For each
// "from", edges are listed STRICTEST FIRST - the first edge you qualify
// for wins, so a pet that clears Greymon's bar never lands on a lesser
// form. An all-zero edge is the fallback (the Numemon / Betamon cases).
// Not implemented yet: the tree's "Stage N Cleared" gates (Bulkmon,
// Boutmon, Kazuchimon) - those paths only exist on the Dokimon
// line, so skipping them never overlaps with anything else.
typedef struct {
  Species from;
  Species to;
  int pp;       // successful Trains + PP_PER_WIN * wins, this stage
  int steps;    // steps this stage
  int battles;  // wins + losses this stage
  int win_pct;  // wins / battles, this stage
} EvoEdge;

static const EvoEdge EVO_EDGES[] = {
  // Baby I -> Baby II (no requirements - just the timer)
  { SP_BOTA, SP_KORO, 0, 0, 0, 0 },
  { SP_DOKI, SP_BIBI, 0, 0, 0, 0 },
  // Baby II -> Child
  { SP_KORO, SP_AGU,  5, 500, 0, 0 },
  { SP_KORO, SP_BETA, 0, 0, 0, 0 },
  { SP_BIBI, SP_PULSE, 0, 0, 0, 0 },          // tree: Stage 3 Cleared (skipped)
  // Child -> Adult
  { SP_AGU, SP_GREY,   12, 1500, 10, 0 },
  { SP_AGU, SP_TYRANO, 10, 1000,  8, 0 },
  { SP_AGU, SP_DEVI,    8, 1000,  5, 0 },
  { SP_AGU, SP_MERA,    5,  500,  3, 0 },
  { SP_AGU, SP_NUME,    0,    0,  0, 0 },
  { SP_BETA, SP_AIRDRA, 15, 1500, 10, 0 },
  { SP_BETA, SP_DEVI,   10, 1000,  8, 0 },
  { SP_BETA, SP_SEADRA,  8, 1000,  5, 0 },
  { SP_BETA, SP_NUME,    0,    0,  0, 0 },
  { SP_PULSE, SP_BULK, 12, 1500, 10, 0 },     // tree: Stage 6 Cleared (skipped)
  // Adult -> Perfect
  { SP_GREY,    SP_METALGREY_VI, 12, 2500, 15, 50 },
  { SP_AIRDRA,  SP_METALGREY_VI, 12, 2500, 15, 50 },
  { SP_DEVI,    SP_METALGREY_VI, 12, 2500, 15, 50 },
  { SP_TYRANO,  SP_MAME,         15, 3000, 15, 50 },
  { SP_SEADRA,  SP_MAME,         15, 3000, 15, 50 },
  { SP_MERA,    SP_MAME,         15, 3000, 15, 50 },
  { SP_NUME,    SP_MONZAE,       15, 3000, 20, 50 },
  { SP_BULK,    SP_BOUT,         12, 2500, 10, 40 },  // tree: Stage 9 Cleared (skipped)
  // Perfect -> Mega
  { SP_METALGREY_VI, SP_BLITZGREY,   15, 3500, 15, 50 },
  { SP_MAME,         SP_BANCHOMAME,  15, 3500, 15, 50 },
  { SP_MONZAE,       SP_SHINMONZAE,  20, 4000, 20, 60 },
  { SP_BOUT,         SP_KAZUCHI,     20, 3500, 20, 50 }, // tree: Stage 12 Cleared (skipped)
};
#define EVO_EDGE_COUNT ((int)(sizeof(EVO_EDGES) / sizeof(EVO_EDGES[0])))

typedef struct {
  int hunger;
  int energy;
  int strength;
  int age_hours;
  int hours_in_stage;
  int steps_in_stage;
  int training_count;
  Species species;
  bool alive;
  bool has_mess;
  int hours_since_clean;
  int care_mistakes;
  time_t last_check;
  int steps_baseline_today;
  int last_check_day_marker;
  bool manually_asleep;  // BE-style forced sleep, independent of the night cycle
  int vital_points;      // separate resource, gained from steps only
  int win_count;          // per-stage battle wins - resets on evolution, groundwork for future branching evolution gates
  int loss_count;         // per-stage battle losses
  int last_steps_shortfall;  // how far short of the steps requirement you were at your LAST evolution, if any - groundwork for future branching
  int last_effort_shortfall; // same, for the Effort requirement
  int journey_stage;         // Journey Together: next stage to play, 0-4 (stage 1-5)
  bool journey_complete;     // finished all 5 stages once: star on the main screen, +1 PP already given
} PetState;

static Window *s_window;
static Window *s_battle_window;
static Window *s_stats_window;
static Layer *s_canvas_layer;
static Layer *s_battle_canvas_layer;
static Layer *s_stats_canvas_layer;
static PetState s_pet;
static GBitmap *s_pet_bitmap = NULL;
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
static GBitmap *s_border_bitmap = NULL;
#endif
static AppTimer *s_idle_timer = NULL;
static bool s_idle_bob_up = false;
static bool s_battery_saver = false; // when true, idle bob is fully disabled
static int s_daily_steps_display = 0; // captured once at launch, same as every other stat - not persisted, always re-derived
static bool s_evolving = false;
static bool s_choosing_egg = false; // post-reset "pick Botamon or Dokimon" screen
#define HATCH_DURATION_MS 4000
static const char *s_transition_text = "Evolving..."; // shared black-screen text (evolving / hatching)
static AppTimer *s_evolve_timer = NULL;
static AppTimer *s_mistake_grace_timer = NULL;
static bool s_mistake_grace_active = false; // true once a grace period has started for the CURRENT unresolved issue

// Battle screen state - transient, not persisted (just "what to show right now")
typedef enum {
  BATTLE_IDLE,
  BATTLE_FIGHTING,
  BATTLE_RESULT,
  JOURNEY_INTRO,
  JOURNEY_PLAYING,
  JOURNEY_RESULT
} BattleScreenState;

// ---- Journey Together: a side-scrolling jump game, reached from the same
// setup screen as Battle (UP/DOWN switches mode). Blocks slide in from the
// right; SELECT makes the Digimon jump; touching a block ends the attempt.
// Five stages, each with more blocks and tighter spacing. Clearing a stage
// advances to the next one (saved); failing just lets you retry the same
// stage. Clearing stage 5 for the first time gives +1 PP and a star on the
// main screen.
#define JOURNEY_FRAME_MS 40         // ~25 frames per second
#define JOURNEY_BLOCK_W 12
#define JOURNEY_BLOCK_H 14
#define JOURNEY_JUMP_FRAMES 22      // total airtime in frames (~0.9 s)
#define JOURNEY_JUMP_PEAK 30        // pixels at the top of the jump
#define JOURNEY_INTRO_MS 1500       // how long "Stage One" shows before it starts
#define JOURNEY_STAGE_COUNT 5
#define JOURNEY_MAX_BLOCKS 13       // must be >= the largest blocks value below
#define JOURNEY_REWARD_PP 1

// Collision box: half-width in px around the Digimon's centre. Deliberately
// NOT derived from the sprite size - a 48 px sprite would make a 60 px-wide
// danger zone that a jump can barely clear. This is the body/feet area only.
#define JOURNEY_HITBOX_HALF 12

// Per-stage tuning. Spacing between blocks (left edge to left edge, px) is
// random between gap_min and gap_min + range, where gap_min is NOT arbitrary:
// airtime distance (JUMP_FRAMES * speed) + block width + the Digimon's hitbox
// + a reaction margin. Any gap rolled is therefore always jumpable; harder
// stages just shrink the margin and the random range and add blocks.
typedef struct {
  int blocks;
  int speed;   // pixels per frame
  int margin;  // extra breathing room beyond the bare minimum, px
  int range;   // random extra spacing, px
} JourneyStageCfg;

static const JourneyStageCfg JOURNEY_STAGES[JOURNEY_STAGE_COUNT] = {
  {  5, 3, 30, 90 },
  {  7, 3, 26, 80 },
  {  9, 3, 22, 70 },
  { 11, 4, 22, 70 },
  { 13, 4, 18, 60 }
};
static const char *const JOURNEY_STAGE_NAMES[JOURNEY_STAGE_COUNT] = {
  "Stage One", "Stage Two", "Stage Three", "Stage Four", "Stage Five"
};

typedef enum { JR_CRASH, JR_STAGE_CLEAR, JR_ALL_DONE } JourneyResultKind;

static bool s_journey_mode = false; // setup screen: false = Battle, true = Journey
static int s_journey_block_x[JOURNEY_MAX_BLOCKS];
static bool s_journey_block_done[JOURNEY_MAX_BLOCKS];
static int s_journey_blocks = 5;    // blocks in the stage being played
static int s_journey_speed = 3;
static int s_journey_played = 0;    // stage index being / just played
static int s_journey_cleared = 0;
static int s_journey_jump_t = -1;   // frames since jump started, -1 = on the ground
static JourneyResultKind s_journey_result = JR_CRASH;
static bool s_journey_pp_awarded = false; // the final-stage reward was given THIS run
static AppTimer *s_journey_timer = NULL;
static BattleScreenState s_battle_state = BATTLE_IDLE;
static bool s_last_battle_won = false;
static int s_last_battle_rounds = 0;
static int s_last_player_hp_remaining = 0;

// Live battle animation state - current HP totals as the fight plays out
// round by round, plus which side landed the most recent hit (drives the
// bounce/lunge animation).
static int s_battle_player_hp = 0;
static int s_battle_opponent_hp = 0;
static int s_battle_player_max_hp = 0;
static int s_battle_opponent_max_hp = 0;
static bool s_last_hit_was_player = false; // true = opponent was the attacker this round
static AppTimer *s_battle_round_timer = NULL;

// Two-phase per-round animation: projectile travel, then defender flicker.
typedef enum { BATTLE_ANIM_PROJECTILE, BATTLE_ANIM_FLICKER } BattleAnimPhase;
static BattleAnimPhase s_battle_anim_phase = BATTLE_ANIM_PROJECTILE;
static int s_battle_anim_step = 0;
static bool s_projectile_toward_opponent = true; // this round's attacker: true = player, false = opponent
static bool s_flicker_visible = true; // toggles during the flicker phase - defender sprite hidden when false

enum {
  KEY_HUNGER = 0,
  KEY_ENERGY,
  KEY_STRENGTH,
  KEY_AGE_HOURS,
  KEY_TRAINING_COUNT,
  KEY_STAGE,
  KEY_ALIVE,
  KEY_LAST_CHECK,
  KEY_HAS_MESS,
  KEY_HOURS_SINCE_CLEAN,
  KEY_CARE_MISTAKES,
  KEY_HOURS_IN_STAGE,
  KEY_STEPS_IN_STAGE,
  KEY_STEPS_BASELINE_TODAY,
  KEY_LAST_CHECK_DAY_MARKER,
  KEY_MANUALLY_ASLEEP,
  KEY_VITAL_POINTS,
  KEY_WIN_COUNT,
  KEY_LOSS_COUNT,
  KEY_LAST_STEPS_SHORTFALL,
  KEY_LAST_EFFORT_SHORTFALL,
  KEY_BATTERY_SAVER,
  KEY_JOURNEY_STAGE,    // appended after the others on purpose: existing saves keep working
  KEY_JOURNEY_COMPLETE
};

static bool is_night_hour(int hour) {
  return hour >= NIGHT_START_HOUR || hour < DAY_START_HOUR;
}

static void queue_snapshot(void); // defined with the save export/import code near the bottom

static void save_pet(void) {
  persist_write_int(KEY_HUNGER, s_pet.hunger);
  persist_write_int(KEY_ENERGY, s_pet.energy);
  persist_write_int(KEY_STRENGTH, s_pet.strength);
  persist_write_int(KEY_AGE_HOURS, s_pet.age_hours);
  persist_write_int(KEY_TRAINING_COUNT, s_pet.training_count);
  persist_write_int(KEY_STAGE, (int)s_pet.species);
  persist_write_bool(KEY_ALIVE, s_pet.alive);
  persist_write_int(KEY_LAST_CHECK, (int32_t)s_pet.last_check);
  persist_write_bool(KEY_HAS_MESS, s_pet.has_mess);
  persist_write_int(KEY_HOURS_SINCE_CLEAN, s_pet.hours_since_clean);
  persist_write_int(KEY_CARE_MISTAKES, s_pet.care_mistakes);
  persist_write_int(KEY_HOURS_IN_STAGE, s_pet.hours_in_stage);
  persist_write_int(KEY_STEPS_IN_STAGE, s_pet.steps_in_stage);
  persist_write_int(KEY_STEPS_BASELINE_TODAY, s_pet.steps_baseline_today);
  persist_write_int(KEY_LAST_CHECK_DAY_MARKER, s_pet.last_check_day_marker);
  persist_write_bool(KEY_MANUALLY_ASLEEP, s_pet.manually_asleep);
  persist_write_int(KEY_VITAL_POINTS, s_pet.vital_points);
  persist_write_int(KEY_WIN_COUNT, s_pet.win_count);
  persist_write_int(KEY_LOSS_COUNT, s_pet.loss_count);
  persist_write_int(KEY_LAST_STEPS_SHORTFALL, s_pet.last_steps_shortfall);
  persist_write_int(KEY_LAST_EFFORT_SHORTFALL, s_pet.last_effort_shortfall);
  persist_write_int(KEY_JOURNEY_STAGE, s_pet.journey_stage);
  persist_write_bool(KEY_JOURNEY_COMPLETE, s_pet.journey_complete);
  queue_snapshot(); // phone keeps a copy for the settings page's Export
}

static void load_pet(void) {
  if (persist_exists(KEY_HUNGER)) {
    s_pet.hunger = persist_read_int(KEY_HUNGER);
    s_pet.energy = persist_read_int(KEY_ENERGY);
    s_pet.strength = persist_read_int(KEY_STRENGTH);
    s_pet.age_hours = persist_read_int(KEY_AGE_HOURS);
    s_pet.training_count = persist_read_int(KEY_TRAINING_COUNT);
    s_pet.species = (Species)persist_read_int(KEY_STAGE);
    if ((unsigned)s_pet.species >= (unsigned)SP_COUNT) s_pet.species = SP_BOTA; // corrupt/unknown save: fall back safely
    s_pet.alive = persist_read_bool(KEY_ALIVE);
    s_pet.last_check = (time_t)persist_read_int(KEY_LAST_CHECK);
    s_pet.has_mess = persist_exists(KEY_HAS_MESS) ? persist_read_bool(KEY_HAS_MESS) : false;
    s_pet.hours_since_clean = persist_exists(KEY_HOURS_SINCE_CLEAN) ? persist_read_int(KEY_HOURS_SINCE_CLEAN) : 0;
    s_pet.care_mistakes = persist_exists(KEY_CARE_MISTAKES) ? persist_read_int(KEY_CARE_MISTAKES) : 0;
    s_pet.hours_in_stage = persist_exists(KEY_HOURS_IN_STAGE) ? persist_read_int(KEY_HOURS_IN_STAGE) : 0;
    s_pet.steps_in_stage = persist_exists(KEY_STEPS_IN_STAGE) ? persist_read_int(KEY_STEPS_IN_STAGE) : 0;
    s_pet.steps_baseline_today = persist_exists(KEY_STEPS_BASELINE_TODAY) ? persist_read_int(KEY_STEPS_BASELINE_TODAY) : 0;
    s_pet.last_check_day_marker = persist_exists(KEY_LAST_CHECK_DAY_MARKER) ? persist_read_int(KEY_LAST_CHECK_DAY_MARKER) : 0;
    s_pet.manually_asleep = persist_exists(KEY_MANUALLY_ASLEEP) ? persist_read_bool(KEY_MANUALLY_ASLEEP) : false;
    s_pet.vital_points = persist_exists(KEY_VITAL_POINTS) ? persist_read_int(KEY_VITAL_POINTS) : 0;
    s_pet.win_count = persist_exists(KEY_WIN_COUNT) ? persist_read_int(KEY_WIN_COUNT) : 0;
    s_pet.loss_count = persist_exists(KEY_LOSS_COUNT) ? persist_read_int(KEY_LOSS_COUNT) : 0;
    s_pet.last_steps_shortfall = persist_exists(KEY_LAST_STEPS_SHORTFALL) ? persist_read_int(KEY_LAST_STEPS_SHORTFALL) : 0;
    s_pet.last_effort_shortfall = persist_exists(KEY_LAST_EFFORT_SHORTFALL) ? persist_read_int(KEY_LAST_EFFORT_SHORTFALL) : 0;
    s_pet.journey_stage = persist_exists(KEY_JOURNEY_STAGE) ? persist_read_int(KEY_JOURNEY_STAGE) : 0;
    if (s_pet.journey_stage < 0 || s_pet.journey_stage >= 5) s_pet.journey_stage = 0;
    s_pet.journey_complete = persist_exists(KEY_JOURNEY_COMPLETE) ? persist_read_bool(KEY_JOURNEY_COMPLETE) : false;
  } else {
    s_pet.hunger = 20;
    s_pet.energy = 90;
    s_pet.strength = 0;
    s_pet.age_hours = 0;
    s_pet.hours_in_stage = 0;
    s_pet.steps_in_stage = 0;
    s_pet.training_count = 0;
    s_pet.species = SP_BOTA;
    s_pet.alive = true;
    s_pet.last_check = time(NULL);
    s_pet.has_mess = false;
    s_pet.hours_since_clean = 0;
    s_pet.care_mistakes = 0;
    s_pet.steps_baseline_today = 0;
    s_pet.last_check_day_marker = 0;
    s_pet.manually_asleep = false;
    s_pet.vital_points = 0;
    s_pet.win_count = 0;
    s_pet.loss_count = 0;
    s_pet.last_steps_shortfall = 0;
    s_pet.last_effort_shortfall = 0;
    s_pet.journey_stage = 0;
    s_pet.journey_complete = false;
  }
}

// Returns 0 when a species has no sprite resource in the project (not
// added yet, or named differently). The #if guards are what make that
// safe: a missing resource would otherwise be a build error. Callers treat
// 0 / a NULL bitmap as "show N/A instead" - see draw_missing_sprite().
static uint32_t resource_for_species(Species sp) {
  switch (sp) {
#if defined(RESOURCE_ID_BOTA_D)
    case SP_BOTA: return RESOURCE_ID_BOTA_D;
#elif defined(RESOURCE_ID_bota_d)
    case SP_BOTA: return RESOURCE_ID_bota_d;
#endif
#if defined(RESOURCE_ID_DOKI_D)
    case SP_DOKI: return RESOURCE_ID_DOKI_D;
#elif defined(RESOURCE_ID_doki_d)
    case SP_DOKI: return RESOURCE_ID_doki_d;
#endif
#if defined(RESOURCE_ID_KORO_D)
    case SP_KORO: return RESOURCE_ID_KORO_D;
#elif defined(RESOURCE_ID_koro_d)
    case SP_KORO: return RESOURCE_ID_koro_d;
#endif
#if defined(RESOURCE_ID_BIBI_D)
    case SP_BIBI: return RESOURCE_ID_BIBI_D;
#elif defined(RESOURCE_ID_bibi_d)
    case SP_BIBI: return RESOURCE_ID_bibi_d;
#endif
#if defined(RESOURCE_ID_AGU_D)
    case SP_AGU: return RESOURCE_ID_AGU_D;
#elif defined(RESOURCE_ID_agu_d)
    case SP_AGU: return RESOURCE_ID_agu_d;
#endif
#if defined(RESOURCE_ID_BETA_D)
    case SP_BETA: return RESOURCE_ID_BETA_D;
#elif defined(RESOURCE_ID_beta_d)
    case SP_BETA: return RESOURCE_ID_beta_d;
#endif
#if defined(RESOURCE_ID_PULSE_D)
    case SP_PULSE: return RESOURCE_ID_PULSE_D;
#elif defined(RESOURCE_ID_pulse_d)
    case SP_PULSE: return RESOURCE_ID_pulse_d;
#endif
#if defined(RESOURCE_ID_GREY_D)
    case SP_GREY: return RESOURCE_ID_GREY_D;
#elif defined(RESOURCE_ID_grey_d)
    case SP_GREY: return RESOURCE_ID_grey_d;
#endif
#if defined(RESOURCE_ID_TYRANO_D)
    case SP_TYRANO: return RESOURCE_ID_TYRANO_D;
#elif defined(RESOURCE_ID_tyrano_d)
    case SP_TYRANO: return RESOURCE_ID_tyrano_d;
#endif
#if defined(RESOURCE_ID_AIRDRA_D)
    case SP_AIRDRA: return RESOURCE_ID_AIRDRA_D;
#elif defined(RESOURCE_ID_airdra_d)
    case SP_AIRDRA: return RESOURCE_ID_airdra_d;
#endif
#if defined(RESOURCE_ID_SEADRA_D)
    case SP_SEADRA: return RESOURCE_ID_SEADRA_D;
#elif defined(RESOURCE_ID_seadra_d)
    case SP_SEADRA: return RESOURCE_ID_seadra_d;
#endif
#if defined(RESOURCE_ID_MERA_D)
    case SP_MERA: return RESOURCE_ID_MERA_D;
#elif defined(RESOURCE_ID_mera_d)
    case SP_MERA: return RESOURCE_ID_mera_d;
#endif
#if defined(RESOURCE_ID_DEVI_D)
    case SP_DEVI: return RESOURCE_ID_DEVI_D;
#elif defined(RESOURCE_ID_devi_d)
    case SP_DEVI: return RESOURCE_ID_devi_d;
#endif
#if defined(RESOURCE_ID_NUME_D)
    case SP_NUME: return RESOURCE_ID_NUME_D;
#elif defined(RESOURCE_ID_nume_d)
    case SP_NUME: return RESOURCE_ID_nume_d;
#endif
#if defined(RESOURCE_ID_KAZUCHI_D)
    case SP_KAZUCHI: return RESOURCE_ID_KAZUCHI_D;
#elif defined(RESOURCE_ID_kazuchi_d)
    case SP_KAZUCHI: return RESOURCE_ID_kazuchi_d;
#endif
#if defined(RESOURCE_ID_BLITZGREY_D)
    case SP_BLITZGREY: return RESOURCE_ID_BLITZGREY_D;
#elif defined(RESOURCE_ID_blitzgrey_d)
    case SP_BLITZGREY: return RESOURCE_ID_blitzgrey_d;
#endif
#if defined(RESOURCE_ID_MAME_D)
    case SP_MAME: return RESOURCE_ID_MAME_D;
#elif defined(RESOURCE_ID_mame_d)
    case SP_MAME: return RESOURCE_ID_mame_d;
#endif
#if defined(RESOURCE_ID_MONZAE_D)
    case SP_MONZAE: return RESOURCE_ID_MONZAE_D;
#elif defined(RESOURCE_ID_monzae_d)
    case SP_MONZAE: return RESOURCE_ID_monzae_d;
#endif
#if defined(RESOURCE_ID_BOUT_D)
    case SP_BOUT: return RESOURCE_ID_BOUT_D;
#elif defined(RESOURCE_ID_bout_d)
    case SP_BOUT: return RESOURCE_ID_bout_d;
#endif
#if defined(RESOURCE_ID_METALGREY_VI_D)
    case SP_METALGREY_VI: return RESOURCE_ID_METALGREY_VI_D;
#elif defined(RESOURCE_ID_metalgrey_vi_d)
    case SP_METALGREY_VI: return RESOURCE_ID_metalgrey_vi_d;
#endif
#if defined(RESOURCE_ID_BANCHOMAME_D)
    case SP_BANCHOMAME: return RESOURCE_ID_BANCHOMAME_D;
#elif defined(RESOURCE_ID_banchomame_d)
    case SP_BANCHOMAME: return RESOURCE_ID_banchomame_d;
#endif
#if defined(RESOURCE_ID_SHINMONZAE_D)
    case SP_SHINMONZAE: return RESOURCE_ID_SHINMONZAE_D;
#elif defined(RESOURCE_ID_shinmonzae_d)
    case SP_SHINMONZAE: return RESOURCE_ID_shinmonzae_d;
#endif
#if defined(RESOURCE_ID_BULK_D)
    case SP_BULK: return RESOURCE_ID_BULK_D;
#elif defined(RESOURCE_ID_bulk_d)
    case SP_BULK: return RESOURCE_ID_bulk_d;
#endif
    default: return 0;
  }
}

static void load_pet_bitmap(void) {
  if (s_pet_bitmap) {
    gbitmap_destroy(s_pet_bitmap);
    s_pet_bitmap = NULL;
  }
  uint32_t res = resource_for_species(s_pet.species);
  if (res != 0) {
    s_pet_bitmap = gbitmap_create_with_resource(res); // can still be NULL (e.g. out of memory)
  }
}

static Tier current_tier(void) {
  return SPECIES[s_pet.species].tier;
}

static int hours_required_for_current_stage(void) {
  return TIER_HOURS[current_tier()];
}

static int steps_required_for_current_stage(void) {
  return TIER_STEPS[current_tier()];
}

// ---- Requirement checks ----

static int current_pp(void) {
  return s_pet.training_count + PP_PER_WIN * s_pet.win_count;
}

static int current_battles(void) {
  return s_pet.win_count + s_pet.loss_count;
}

static int current_win_pct(void) {
  int b = current_battles();
  return b > 0 ? (s_pet.win_count * 100) / b : 0;
}

static bool edge_met(const EvoEdge *e) {
  return current_pp() >= e->pp &&
         s_pet.steps_in_stage >= e->steps &&
         current_battles() >= e->battles &&
         current_win_pct() >= e->win_pct;
}

static const EvoEdge *find_edge_to(Species from, Species to) {
  for (int i = 0; i < EVO_EDGE_COUNT; i++) {
    if (EVO_EDGES[i].from == from && EVO_EDGES[i].to == to) return &EVO_EDGES[i];
  }
  return NULL;
}

// Too many care mistakes at the Child stage forces Numemon, the real
// Digimon rule - but only if this Child actually has a Numemon branch.
static const EvoEdge *forced_numemon_edge(void) {
  if (current_tier() != TIER_CHILD) return NULL;
  if (s_pet.care_mistakes <= CARE_MISTAKES_CAP) return NULL;
  return find_edge_to(s_pet.species, SP_NUME);
}

// The edge this pet would take RIGHT NOW, or NULL if it qualifies for none
// (only possible where there's no fallback edge: Perfect and Mega forms).
static const EvoEdge *choose_evolution(void) {
  const EvoEdge *forced = forced_numemon_edge();
  if (forced) return forced;
  for (int i = 0; i < EVO_EDGE_COUNT; i++) {
    if (EVO_EDGES[i].from == s_pet.species && edge_met(&EVO_EDGES[i])) return &EVO_EDGES[i];
  }
  return NULL;
}

// The "goal" shown on the Stats screen: the strictest form this pet could
// still evolve into (first edge listed), or the forced Numemon edge.
static const EvoEdge *goal_edge(void) {
  const EvoEdge *forced = forced_numemon_edge();
  if (forced) return forced;
  for (int i = 0; i < EVO_EDGE_COUNT; i++) {
    if (EVO_EDGES[i].from == s_pet.species) return &EVO_EDGES[i];
  }
  return NULL;
}

static void refresh(void); // forward declaration - used by evolve_timer_callback below

static void evolve_timer_callback(void *data) {
  s_evolving = false;
  s_evolve_timer = NULL;
  refresh();
}

// True once the tier's timer is up (awake hours), or the pet is so active
// that steps alone bring the evolution forward. Being "due" only means an
// evolution is ATTEMPTED - the requirement table still picks the form.
static bool evolution_due(void) {
  if (!s_pet.alive || current_tier() == TIER_MEGA) return false;
  int hours_needed = hours_required_for_current_stage();
  int steps_needed = steps_required_for_current_stage();
  bool hours_met = (hours_needed >= 0) && (s_pet.hours_in_stage >= hours_needed);
  bool super_active = (steps_needed >= 0) &&
      (s_pet.steps_in_stage >= steps_needed * SUPER_ACTIVE_STEPS_MULTIPLIER);
  return hours_met || super_active;
}

static void check_evolution(void) {
  if (!evolution_due()) return;

  const EvoEdge *edge = choose_evolution();
  if (!edge) return; // due, but nothing qualifies - see evolution_blocked()

  // Snapshot how this stage went versus the strictest branch, BEFORE the
  // reset below wipes the numbers - groundwork for future use (alongside
  // win_count / loss_count, which serve the same purpose for battles).
  const EvoEdge *best = goal_edge();
  if (best) {
    s_pet.last_steps_shortfall = (s_pet.steps_in_stage < best->steps) ? (best->steps - s_pet.steps_in_stage) : 0;
    s_pet.last_effort_shortfall = (current_pp() < best->pp) ? (best->pp - current_pp()) : 0;
  }

  s_pet.species = edge->to;
  s_pet.hours_in_stage = 0;
  s_pet.steps_in_stage = 0;
  s_pet.training_count = 0;
  s_pet.care_mistakes = 0; // resets each evolution, matching the real device
  s_pet.win_count = 0;
  s_pet.loss_count = 0;
  s_pet.vital_points = 0; // fresh form, fresh start - rebuild via steps again
  load_pet_bitmap();

  s_transition_text = "Evolving...";
  s_evolving = true;
  vibes_double_pulse(); // more distinct than the everyday action buzz - this moment should feel bigger
  if (s_evolve_timer) app_timer_cancel(s_evolve_timer);
  s_evolve_timer = app_timer_register(EVOLVE_DURATION_MS, evolve_timer_callback, NULL);
}

// True when the evolution timer is up but NO branch's requirements are met.
// Perfect and Mega forms have no fallback branch (unlike Child -> Numemon),
// so this is where "EVO FAIL" comes from; the Stats screen's goal page
// shows exactly which requirements are still short.
static bool evolution_blocked(void) {
  return evolution_due() && choose_evolution() == NULL;
}

static int steps_since_last_check(time_t last_check, time_t now) {
#if defined(PBL_HEALTH)
  int today_steps = (int)health_service_sum_today(HealthMetricStepCount);
  s_daily_steps_display = today_steps; // captured here for the MetalGreymon display - no extra Health query needed

  struct tm *now_tm = localtime(&now);
  int now_day_marker = (now_tm->tm_year * 1000) + now_tm->tm_yday;

  int delta;
  if (now_day_marker == s_pet.last_check_day_marker) {
    delta = today_steps - s_pet.steps_baseline_today;
  } else {
    delta = today_steps;
  }
  if (delta < 0) delta = 0;

  s_pet.steps_baseline_today = today_steps;
  s_pet.last_check_day_marker = now_day_marker;

  return delta;
#else
  return 0;
#endif
}

// Applies real-world elapsed time (and steps) since the last check.
// BE-style manual sleep freezes EVERYTHING: if manually_asleep is true,
// this entire span is treated as paused - no hunger/energy change, no
// mess, no evolution progress from either time or steps, no Vital Point
// gain. steps_since_last_check() still runs regardless, purely to keep
// the day-rollover baseline bookkeeping accurate for whenever it wakes up.
static void apply_time_decay(void) {
  time_t now = time(NULL);
  int hours_elapsed = (int)((now - s_pet.last_check) / 3600);

  int steps_delta = steps_since_last_check(s_pet.last_check, now);
  bool frozen = s_pet.manually_asleep;

  if (!frozen) {
    s_pet.steps_in_stage += steps_delta;
    s_pet.vital_points += steps_delta / STEPS_PER_VITAL_POINT;
    if (s_pet.vital_points > MAX_VITAL_POINTS) s_pet.vital_points = MAX_VITAL_POINTS;
  }

  if (hours_elapsed <= 0) {
    if (!frozen) check_evolution();
    s_pet.last_check = now;
    save_pet();
    return;
  }
  if (!s_pet.alive) return;

  if (!frozen) {
    time_t cursor = s_pet.last_check;
    int awake_hours = 0; // evolution timer only counts AWAKE hours - it pauses during any sleep, matching the real device
    // was_hunger_critical/was_energy_critical (and the whole edge-detected
    // increment block below) are commented out entirely, alongside the
    // Care Mistakes feature itself - leaving them declared but with their
    // reads removed was what actually broke the build (GCC flags a
    // variable that's assigned but never read as its own fatal error,
    // separate from a plain "unused variable").

    for (int i = 0; i < hours_elapsed; i++) {
      cursor += 3600;
      struct tm *t = localtime(&cursor);

      if (is_night_hour(t->tm_hour)) {
        s_pet.energy += NIGHT_ENERGY_REGEN_PER_HOUR;
        s_pet.hunger += NIGHT_HUNGER_PER_HOUR;
      } else {
        s_pet.hunger += DECAY_PER_HOUR;
        s_pet.energy -= DECAY_PER_HOUR;
        awake_hours++;
      }
      if (s_pet.hunger > MAX_STAT) s_pet.hunger = MAX_STAT;
      if (s_pet.energy < 0) s_pet.energy = 0;
      if (s_pet.energy > MAX_STAT) s_pet.energy = MAX_STAT;

      // Care mistake tracking is DISABLED for now, not removed - it
      // increments silently in the background, with no way for the player
      // to know until they happen to open the app, which felt unfair.
      // Re-enable once the background worker (real-time vibration alerts)
      // exists to actually warn about this as it happens, matching how
      // the real device works.
      // bool hunger_critical_now = (s_pet.hunger >= ATTENTION_HUNGER_THRESHOLD);
      // bool energy_critical_now = (s_pet.energy <= ATTENTION_ENERGY_THRESHOLD);
      // if (hunger_critical_now && !was_hunger_critical) s_pet.care_mistakes++;
      // if (energy_critical_now && !was_energy_critical) s_pet.care_mistakes++;
      // was_hunger_critical = hunger_critical_now;
      // was_energy_critical = energy_critical_now;
    }

    s_pet.age_hours += hours_elapsed;   // lifetime stat - counts every hour, asleep or not
    s_pet.hours_in_stage += awake_hours; // evolution timer - only counts awake hours

    s_pet.hours_since_clean += hours_elapsed;
    if (!s_pet.has_mess && s_pet.hours_since_clean >= MESS_HOURS_THRESHOLD) {
      s_pet.has_mess = true;
      // s_pet.care_mistakes++; // disabled alongside the above - same silent-penalty problem
    }

    if (s_pet.hunger >= MAX_STAT) {
      s_pet.alive = false;
    } else if (s_pet.energy <= 0 && s_pet.hunger > 50) {
      s_pet.alive = false;
    }

    check_evolution();
  }
  // if frozen: hunger/energy/mess/evolution/vital points all stay exactly
  // as they were - nothing above this comment ran

  s_pet.last_check = now;
  save_pet();
}

// The "content area" pets/text should live within.
static GRect content_rect(GRect bounds) {
#if defined(PBL_PLATFORM_EMERY)
  return GRect(SQUARE_LEFT, SQUARE_TOP, SQUARE_RIGHT - SQUARE_LEFT, SQUARE_BOTTOM - SQUARE_TOP);
#elif defined(PBL_PLATFORM_GABBRO)
  return GRect(GABBRO_SQUARE_LEFT, GABBRO_SQUARE_TOP,
      GABBRO_SQUARE_RIGHT - GABBRO_SQUARE_LEFT, GABBRO_SQUARE_BOTTOM - GABBRO_SQUARE_TOP);
#else
  return GRect(BEZEL_SIDE, BEZEL_TOP, bounds.size.w - 2 * BEZEL_SIDE, bounds.size.h - BEZEL_TOP - BEZEL_BOTTOM);
#endif
}

// SINGLE shared source of truth for where the pet sprite's center sits.
static GPoint pet_center_point(GRect bounds) {
  GRect cr = content_rect(bounds);
  int bob = s_idle_bob_up ? -IDLE_BOB_OFFSET : 0;

  int zone_top = cr.origin.y + TOP_UI_HEIGHT;
  int zone_bottom = cr.origin.y + cr.size.h - BOTTOM_UI_HEIGHT;
  int center_y = (zone_top + zone_bottom) / 2 + bob;
  int center_x = cr.origin.x + cr.size.w / 2;

  return GPoint(center_x, center_y);
}

static bool needs_attention(void); // forward declaration - used by draw_sleep_indicator below

// Asleep either automatically (night hours) OR manually (BE-style forced
// sleep) - either way shows the same Zzz indicator.
static bool is_pet_asleep(void) {
  if (!s_pet.alive) return false;
  if (s_pet.manually_asleep) return true;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  return is_night_hour(t->tm_hour);
}

static void draw_sleep_indicator(GContext *ctx, GRect bounds) {
  if (!is_pet_asleep()) return;

  GPoint pet_center = pet_center_point(bounds);
  GRect cr = content_rect(bounds);
  int sprite_half_h = 15;
  if (s_pet_bitmap) {
    sprite_half_h = gbitmap_get_bounds(s_pet_bitmap).size.h / 2;
  }
  int extra_offset = needs_attention() ? 16 : 0;
  GPoint pos = GPoint(pet_center.x + 14, pet_center.y - sprite_half_h - 10 - extra_offset);

  // Clamp so a tall sprite (or one with extra transparent padding baked
  // into its PNG - the file's height isn't necessarily where the visible
  // character actually ends) can't push this up into the stage name text.
  // Same safety net already used for the attention icon.
  int min_y = cr.origin.y + TOP_UI_HEIGHT + 6;
  if (pos.y < min_y) pos.y = min_y;

  const char *zzz_text = s_idle_bob_up ? "Zzz" : "Z";
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, zzz_text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(pos.x - 20, pos.y - 10, 40, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// Shown above the pet's head, on the LEFT side (mirroring the sleep
// indicator's right-side offset, so they can never collide), whenever
// evolution_blocked() is true - the timer is up but no branch's
// requirements are met. Stats > goal page shows what's still short.
static void draw_evo_failed_indicator(GContext *ctx, GRect bounds) {
  if (!evolution_blocked()) return;

  GPoint pet_center = pet_center_point(bounds);
  GRect cr = content_rect(bounds);
  int sprite_half_h = 15;
  if (s_pet_bitmap) {
    sprite_half_h = gbitmap_get_bounds(s_pet_bitmap).size.h / 2;
  }
  GPoint pos = GPoint(pet_center.x - 20, pet_center.y - sprite_half_h - 10);

  int min_y = cr.origin.y + TOP_UI_HEIGHT + 6;
  if (pos.y < min_y) pos.y = min_y;

  graphics_context_set_text_color(ctx, GColorRed);
  graphics_draw_text(ctx, "EVO FAIL", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(pos.x - 30, pos.y - 8, 60, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static bool needs_attention(void) {
  return s_pet.alive && (s_pet.hunger >= ATTENTION_HUNGER_THRESHOLD ||
                          s_pet.energy <= ATTENTION_ENERGY_THRESHOLD ||
                          s_pet.has_mess);
}

// Fires once the grace period has fully elapsed with the app still open.
// If the issue is STILL unresolved at that point, that's the actual
// mistake - genuinely ignored, not just "didn't open the app fast enough."
static void mistake_grace_timeout(void *data) {
  s_mistake_grace_timer = NULL;
  if (needs_attention()) {
    s_pet.care_mistakes++;
    save_pet();
  }
  s_mistake_grace_active = false; // this episode is resolved either way - a fresh one can start later if it happens again
}

// Call this any time the underlying condition could have changed: once
// right when the main screen loads (catching "already had a problem when
// I opened it" and starting their grace period fairly, from scratch), and
// again after any action that might have fixed it. Nothing else can
// change hunger/energy/mess while the app just sits open, so those two
// call sites are the only ones needed.
static void check_mistake_grace(void) {
  if (needs_attention()) {
    if (!s_mistake_grace_active) {
      s_mistake_grace_active = true;
      if (s_mistake_grace_timer) app_timer_cancel(s_mistake_grace_timer);
      s_mistake_grace_timer = app_timer_register(CARE_MISTAKE_GRACE_MS, mistake_grace_timeout, NULL);
    }
  } else if (s_mistake_grace_timer) {
    app_timer_cancel(s_mistake_grace_timer);
    s_mistake_grace_timer = NULL;
    s_mistake_grace_active = false;
  }
}

static void draw_procedural_bezel(GContext *ctx, GRect bounds) {
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 8, GCornersAll);

  GRect screen_area = content_rect(bounds);
  graphics_context_set_fill_color(ctx, GColorPastelYellow);
  graphics_fill_rect(ctx, screen_area, 3, GCornersAll);

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, screen_area, 3);

  int dot_y = BEZEL_TOP / 2;
  int dot_spacing = 8;
  int dot_start_x = bounds.size.w / 2 - dot_spacing * 2;
  graphics_context_set_fill_color(ctx, GColorBlack);
  for (int i = 0; i < 5; i++) {
    graphics_fill_circle(ctx, GPoint(dot_start_x + i * dot_spacing, dot_y), 1);
  }
}

static void draw_background(GContext *ctx, GRect bounds) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  if (s_border_bitmap) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_border_bitmap, bounds);
  }
#else
  draw_procedural_bezel(ctx, bounds);
#endif
}

static void draw_attention_icon(GContext *ctx, GRect bounds) {
  if (!needs_attention()) return;

  GPoint pet_center = pet_center_point(bounds);
  GRect cr = content_rect(bounds);
  int sprite_half_h = 15;
  if (s_pet_bitmap) {
    sprite_half_h = gbitmap_get_bounds(s_pet_bitmap).size.h / 2;
  }
  GPoint pos = GPoint(pet_center.x, pet_center.y - sprite_half_h - 12);

  int min_y = cr.origin.y + TOP_UI_HEIGHT + 6;
  if (pos.y < min_y) pos.y = min_y;

  graphics_context_set_fill_color(ctx, GColorRed);
  graphics_fill_circle(ctx, pos, 8);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "!", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(pos.x - 8, pos.y - 10, 16, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// Missing-sprite placeholder: a small outlined box with "N/A", so a sprite
// that isn't in the project (or failed to load) is obvious but harmless.
static void draw_missing_sprite(GContext *ctx, GPoint center) {
  GRect box = GRect(center.x - 20, center.y - 14, 40, 28);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_rect(ctx, box);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "N/A", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(box.origin.x, box.origin.y + 4, box.size.w, 20),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_pet(GContext *ctx, GRect bounds) {
  GPoint center = pet_center_point(bounds);

  if (!s_pet.alive) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, GPoint(center.x - 15, center.y - 15), GPoint(center.x + 15, center.y + 15));
    graphics_draw_line(ctx, GPoint(center.x + 15, center.y - 15), GPoint(center.x - 15, center.y + 15));
    return;
  }

  if (!s_pet_bitmap) {
    draw_missing_sprite(ctx, center);
    return;
  }

  GRect bmp_bounds = gbitmap_get_bounds(s_pet_bitmap);
  GRect draw_rect = GRect(
      center.x - bmp_bounds.size.w / 2,
      center.y - bmp_bounds.size.h / 2,
      bmp_bounds.size.w,
      bmp_bounds.size.h);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, s_pet_bitmap, draw_rect);
}

// Journey Together completion star, drawn beside the Digimon on the home
// screen. A 10-point GPath, built lazily and freed in deinit().
static GPath *s_star_path = NULL;
static const GPoint STAR_POINTS[10] = {
  { 0, -8 }, { 2, -3 }, { 8, -3 }, { 3, 1 }, { 5, 7 },
  { 0, 3 }, { -5, 7 }, { -3, 1 }, { -8, -3 }, { -2, -3 }
};
static GPathInfo s_star_info = { 10, (GPoint *)STAR_POINTS };

static void draw_journey_star(GContext *ctx, GRect bounds) {
  if (!s_pet.journey_complete || !s_pet.alive) return;
  if (!s_star_path) s_star_path = gpath_create(&s_star_info);
  if (!s_star_path) return;

  GRect cr = content_rect(bounds);
  GPoint c = pet_center_point(bounds);
  int half_w = s_pet_bitmap ? gbitmap_get_bounds(s_pet_bitmap).size.w / 2 : 20;
  int half_h = s_pet_bitmap ? gbitmap_get_bounds(s_pet_bitmap).size.h / 2 : 15;
  GPoint pos = GPoint(c.x + half_w + 2, c.y - half_h + 6);
  // keep the whole star inside the cutout
  if (pos.x > cr.origin.x + cr.size.w - 10) pos.x = cr.origin.x + cr.size.w - 10;
  if (pos.y < cr.origin.y + TOP_UI_HEIGHT + 10) pos.y = cr.origin.y + TOP_UI_HEIGHT + 10;

  gpath_move_to(s_star_path, pos);
  graphics_context_set_fill_color(ctx, GColorYellow);
  gpath_draw_filled(ctx, s_star_path);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  gpath_draw_outline(ctx, s_star_path);
}

static void draw_mess(GContext *ctx, GRect bounds) {
  if (!s_pet.has_mess || !s_pet.alive) return;

  GRect cr = content_rect(bounds);
  GPoint spot = GPoint(cr.origin.x + 14, cr.origin.y + cr.size.h - BOTTOM_UI_HEIGHT + 4);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, spot, 5);
  graphics_fill_circle(ctx, GPoint(spot.x - 3, spot.y + 5), 4);
  graphics_fill_circle(ctx, GPoint(spot.x + 4, spot.y + 6), 4);
}

static const char *species_name(Species sp) {
  if ((unsigned)sp >= (unsigned)SP_COUNT) return "?";
  return SPECIES[sp].name;
}

typedef enum {
  ACTION_FEED,
  ACTION_TRAIN,
  ACTION_SLEEP,
  ACTION_CLEAN,
  ACTION_COUNT
} MenuAction;

static MenuAction s_selected_action = ACTION_FEED;

static const char *action_name(MenuAction action) {
  switch (action) {
    case ACTION_FEED: return "Feed";
    case ACTION_TRAIN: return "Train";
    case ACTION_SLEEP: return "Sleep";
    case ACTION_CLEAN: return "Clean";
    default: return "?";
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_choosing_egg) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "Choose your egg", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GRect(0, bounds.size.h / 2 - 50, bounds.size.w, 30),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "UP: Botamon", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(0, bounds.size.h / 2 - 8, bounds.size.w, 24),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "DOWN: Dokimon", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(0, bounds.size.h / 2 + 20, bounds.size.w, 24),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_evolving) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_transition_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GRect(0, bounds.size.h / 2 - 14, bounds.size.w, 28),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  GRect cr = content_rect(bounds);
  int cr_bottom = cr.origin.y + cr.size.h;

  draw_background(ctx, bounds);
  draw_pet(ctx, bounds);
  draw_mess(ctx, bounds);
  draw_journey_star(ctx, bounds);
  draw_attention_icon(ctx, bounds);
  draw_sleep_indicator(ctx, bounds);
  draw_evo_failed_indicator(ctx, bounds);

  graphics_context_set_text_color(ctx, GColorBlack);

  static char stage_buf[32];
  snprintf(stage_buf, sizeof(stage_buf), "%s", s_pet.alive ? species_name(s_pet.species) : "R.I.P.");
  // Long names (BanchoMamemon, ShinMonzaemon...) drop to the smaller font
  // so they stay inside the narrower Gabbro cutout.
  graphics_draw_text(ctx, stage_buf,
      fonts_get_system_font(strlen(stage_buf) > 11 ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_18_BOLD),
      GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char progress_label[32];
  int hours_needed = hours_required_for_current_stage();
  int steps_needed = steps_required_for_current_stage();
  int pct = 0;

  if (current_tier() != TIER_MEGA) {
    int hours_pct = (hours_needed > 0) ? (s_pet.hours_in_stage * 100) / hours_needed : 0;
    int steps_pct = (steps_needed > 0) ? (s_pet.steps_in_stage * 100) / steps_needed : 0;
    bool steps_leading = steps_pct >= hours_pct;
    pct = steps_leading ? steps_pct : hours_pct;
    if (pct > 100) pct = 100;

    if (steps_leading) {
      snprintf(progress_label, sizeof(progress_label), "Steps: %d/%d", s_pet.steps_in_stage, steps_needed);
    } else {
      snprintf(progress_label, sizeof(progress_label), "Time: %dh/%dh", s_pet.hours_in_stage, hours_needed);
    }

    graphics_draw_text(ctx, progress_label, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr_bottom - 52, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    GRect bar_outline = GRect(cr.origin.x + 10, cr_bottom - 38, cr.size.w - 20, 8);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_rect(ctx, bar_outline);
    GRect bar_fill = GRect(bar_outline.origin.x + 1, bar_outline.origin.y + 1,
        ((bar_outline.size.w - 2) * pct) / 100, bar_outline.size.h - 2);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bar_fill, 0, GCornerNone);
  } else {
    // Final stage (for now) - nothing left to evolve toward, so the same
    // iconic bar+label slot becomes a plain daily step count instead of
    // sitting empty. Different text format ("Today: N steps", no "/goal"
    // ratio) deliberately distinguishes it from the evolution-progress
    // version. The bar's fill is purely a visual reference against a
    // common 10,000-step daily goal - it doesn't gate or unlock anything.
    snprintf(progress_label, sizeof(progress_label), "Today: %d steps", s_daily_steps_display);
    graphics_draw_text(ctx, progress_label, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr_bottom - 52, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    int daily_pct = (s_daily_steps_display * 100) / 10000;
    if (daily_pct > 100) daily_pct = 100;
    if (daily_pct < 0) daily_pct = 0;

    GRect bar_outline = GRect(cr.origin.x + 10, cr_bottom - 38, cr.size.w - 20, 8);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_rect(ctx, bar_outline);
    GRect bar_fill = GRect(bar_outline.origin.x + 1, bar_outline.origin.y + 1,
        ((bar_outline.size.w - 2) * daily_pct) / 100, bar_outline.size.h - 2);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bar_fill, 0, GCornerNone);
  }

  static char stats_buf[64];
  snprintf(stats_buf, sizeof(stats_buf), "H:%d E:%d S:%d",
      s_pet.hunger, s_pet.energy, s_pet.strength);
  graphics_draw_text(ctx, stats_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr_bottom - 26, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  if (s_pet.alive) {
    static char action_buf[32];
    snprintf(action_buf, sizeof(action_buf), "> %s <", action_name(s_selected_action));
    graphics_draw_text(ctx, action_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(cr.origin.x, cr_bottom - 12, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  } else {
    graphics_draw_text(ctx, "Hold DOWN: new egg", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(cr.origin.x, cr_bottom - 12, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void refresh(void) {
  layer_mark_dirty(s_canvas_layer);
}

// Base max HP per tier - same for every Digimon in that tier (real per-
// species stats come later). +100 per tier, matching the pattern you set.
static int base_hp_for_species(Species sp) {
  switch (SPECIES[sp].tier) {
    case TIER_BABY1: return 100;
    case TIER_BABY2: return 100;
    case TIER_CHILD: return 200;
    case TIER_ADULT: return 300;
    case TIER_PERFECT: return 400;
    case TIER_MEGA: return 500;
    default: return 100;
  }
}

// Resolves a battle round-by-round, live, driven by an AppTimer - each
// round plays out as a projectile-travel phase followed by a defender-
// flicker phase (see start_next_round/battle_anim_tick below), so the
// sprites, projectile, and both HP bars all update in step with the
// actual fight, rather than jumping straight to a result.
static void finish_battle(bool won) {
  if (won) {
    // A win counts toward PP (PP_PER_WIN each - see current_pp()), toward
    // the battles / win-ratio requirements, and grants Vital Points, the
    // reward side of the same stake a loss costs you.
    s_pet.win_count++;
    s_pet.vital_points += BATTLE_VP_STAKE;
    if (s_pet.vital_points > MAX_VITAL_POINTS) s_pet.vital_points = MAX_VITAL_POINTS;
  } else {
    // The stake of the gamble: losing costs the exact same amount a win
    // gains - a genuinely even trade now that the HP bonus itself is
    // capped and fair, not compensating for an overpowered advantage.
    s_pet.vital_points -= BATTLE_VP_STAKE;
    if (s_pet.vital_points < 0) s_pet.vital_points = 0;
    s_pet.loss_count++;
  }

  check_evolution(); // a win might push it over the threshold immediately
  save_pet();

  s_last_battle_won = won;
  s_last_player_hp_remaining = s_battle_player_hp > 0 ? s_battle_player_hp : 0;
  s_battle_state = BATTLE_RESULT;
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
}

static void battle_anim_tick(void *data); // forward declaration - mutually referenced with start_next_round below

// Kicks off one round: rolls this round's attacker and starts the
// projectile traveling toward the defender.
static void start_next_round(void *data) {
  s_battle_round_timer = NULL;

  s_projectile_toward_opponent = (rand() % 2) == 0;
  s_battle_anim_phase = BATTLE_ANIM_PROJECTILE;
  s_battle_anim_step = 0;
  s_flicker_visible = true;

  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
  s_battle_round_timer = app_timer_register(PROJECTILE_STEP_MS, battle_anim_tick, NULL);
}

// Drives both animation phases. During PROJECTILE, just advances the
// travel step and redraws. On reaching the defender, damage is actually
// applied (HP bars will visibly drop from here), then the FLICKER phase
// begins. Once the flicker finishes, either the battle is over or the
// next round starts after a brief pause.
static void battle_anim_tick(void *data) {
  s_battle_round_timer = NULL;

  if (s_battle_anim_phase == BATTLE_ANIM_PROJECTILE) {
    s_battle_anim_step++;
    if (s_battle_anim_step < PROJECTILE_STEPS) {
      if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
      s_battle_round_timer = app_timer_register(PROJECTILE_STEP_MS, battle_anim_tick, NULL);
      return;
    }

    // Impact - damage is applied right here, not at round start
    if (s_projectile_toward_opponent) {
      s_battle_opponent_hp -= BATTLE_DAMAGE_PER_HIT;
      s_last_hit_was_player = false;
    } else {
      s_battle_player_hp -= BATTLE_DAMAGE_PER_HIT;
      s_last_hit_was_player = true;
    }
    s_last_battle_rounds++;

    s_battle_anim_phase = BATTLE_ANIM_FLICKER;
    s_battle_anim_step = 0;
    s_flicker_visible = false; // flicker starts by hiding the defender
    if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
    s_battle_round_timer = app_timer_register(FLICKER_STEP_MS, battle_anim_tick, NULL);
    return;
  }

  // BATTLE_ANIM_FLICKER
  s_battle_anim_step++;
  s_flicker_visible = !s_flicker_visible;
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);

  if (s_battle_anim_step < FLICKER_STEPS) {
    s_battle_round_timer = app_timer_register(FLICKER_STEP_MS, battle_anim_tick, NULL);
    return;
  }

  // Flicker done - battle over, or queue the next round after a brief pause
  if (s_battle_player_hp <= 0 || s_battle_opponent_hp <= 0) {
    finish_battle(s_battle_opponent_hp <= 0);
    return;
  }
  s_battle_round_timer = app_timer_register(POST_ROUND_PAUSE_MS, start_next_round, NULL);
}

// Only the player gets a Vital Points bonus (opponent is a same-tier
// "wild" stand-in with just the tier's base HP) - that asymmetry is the
// whole point, since it's what makes your own steps matter to the outcome.
// Bonus is capped at a flat +50 HP (2 extra hit-absorptions at
// BATTLE_DAMAGE_PER_HIT=25), not scaled per-tier - at 0 VP it's a fair
// fight, at max 100 VP you can afford roughly 2 extra losses in the coin
// flip and still win, not dozens.
static void start_battle(void) {
  s_battle_player_max_hp = base_hp_for_species(s_pet.species) + s_pet.vital_points / 2;
  s_battle_opponent_max_hp = base_hp_for_species(s_pet.species);
  s_battle_player_hp = s_battle_player_max_hp;
  s_battle_opponent_hp = s_battle_opponent_max_hp;
  s_last_battle_rounds = 0;
  s_last_hit_was_player = false;

  s_battle_state = BATTLE_FIGHTING;
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);

  if (s_battle_round_timer) app_timer_cancel(s_battle_round_timer);
  start_next_round(NULL);
}


// Parabolic jump: 0 on the ground, JOURNEY_JUMP_PEAK at the midpoint.
static int journey_jump_height(int t) {
  if (t < 0 || t > JOURNEY_JUMP_FRAMES) return 0;
  return (JOURNEY_JUMP_PEAK * 4 * t * (JOURNEY_JUMP_FRAMES - t)) /
         (JOURNEY_JUMP_FRAMES * JOURNEY_JUMP_FRAMES);
}

static void journey_finish(bool crashed) {
  s_journey_pp_awarded = false;
  if (crashed) {
    s_journey_result = JR_CRASH;
    vibes_short_pulse();
  } else if (s_journey_played >= JOURNEY_STAGE_COUNT - 1) {
    // Final stage cleared: back to stage 1 for replays, reward only once.
    s_journey_result = JR_ALL_DONE;
    s_pet.journey_stage = 0;
    if (!s_pet.journey_complete) {
      s_pet.journey_complete = true;
      s_pet.training_count += JOURNEY_REWARD_PP; // PP = trains + 2*wins, so this is +1 PP
      s_journey_pp_awarded = true;
    }
    save_pet();
    vibes_double_pulse();
    if (s_canvas_layer) layer_mark_dirty(s_canvas_layer); // star appears on the home screen
  } else {
    s_journey_result = JR_STAGE_CLEAR;
    s_pet.journey_stage = s_journey_played + 1;
    save_pet();
    vibes_double_pulse();
  }
  s_battle_state = JOURNEY_RESULT;
  if (s_journey_timer) { app_timer_cancel(s_journey_timer); s_journey_timer = NULL; }
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
}

static void journey_tick(void *data) {
  s_journey_timer = NULL;
  if (s_battle_state != JOURNEY_PLAYING || !s_battle_canvas_layer) return;

  GRect cr = content_rect(layer_get_bounds(s_battle_canvas_layer));
  int pet_x = cr.origin.x + cr.size.w / 3;
  int half_w = JOURNEY_HITBOX_HALF; // fixed "feet" box, independent of sprite size
  int pet_l = pet_x - half_w;
  int pet_r = pet_x + half_w;

  if (s_journey_jump_t >= 0) {
    s_journey_jump_t++;
    if (s_journey_jump_t > JOURNEY_JUMP_FRAMES) s_journey_jump_t = -1;
  }
  int jump_h = journey_jump_height(s_journey_jump_t);

  for (int i = 0; i < s_journey_blocks; i++) {
    s_journey_block_x[i] -= s_journey_speed;
    int bl = s_journey_block_x[i];
    int br = bl + JOURNEY_BLOCK_W;
    if (!s_journey_block_done[i]) {
      if (br > pet_l && bl < pet_r && jump_h < JOURNEY_BLOCK_H) {
        journey_finish(true);
        return;
      }
      if (br <= pet_l) {
        s_journey_block_done[i] = true;
        s_journey_cleared++;
      }
    }
  }

  if (s_journey_cleared >= s_journey_blocks) {
    journey_finish(false);
    return;
  }
  layer_mark_dirty(s_battle_canvas_layer);
  s_journey_timer = app_timer_register(JOURNEY_FRAME_MS, journey_tick, NULL);
}

// After the "Stage N" banner: lay out this stage's blocks and start moving.
static void journey_begin_run(void *data) {
  s_journey_timer = NULL;
  if (s_battle_state != JOURNEY_INTRO || !s_battle_canvas_layer) return;

  const JourneyStageCfg *cfg = &JOURNEY_STAGES[s_journey_played];
  s_journey_blocks = cfg->blocks;
  s_journey_speed = cfg->speed;
  int gap_min = JOURNEY_JUMP_FRAMES * cfg->speed + JOURNEY_BLOCK_W + 2 * JOURNEY_HITBOX_HALF + cfg->margin;

  GRect cr = content_rect(layer_get_bounds(s_battle_canvas_layer));
  int x = cr.origin.x + cr.size.w + 10;
  for (int i = 0; i < s_journey_blocks; i++) {
    s_journey_block_x[i] = x;
    s_journey_block_done[i] = false;
    x += gap_min + (int)(rand() % (cfg->range + 1));
  }
  s_journey_cleared = 0;
  s_journey_jump_t = -1;
  s_battle_state = JOURNEY_PLAYING;
  layer_mark_dirty(s_battle_canvas_layer);
  s_journey_timer = app_timer_register(JOURNEY_FRAME_MS, journey_tick, NULL);
}

static void start_journey(void) {
  if (!s_battle_canvas_layer) return;
  s_journey_played = s_pet.journey_stage;
  if (s_journey_played < 0 || s_journey_played >= JOURNEY_STAGE_COUNT) s_journey_played = 0;
  s_journey_blocks = JOURNEY_STAGES[s_journey_played].blocks;
  s_journey_cleared = 0;
  s_battle_state = JOURNEY_INTRO;
  layer_mark_dirty(s_battle_canvas_layer);
  if (s_journey_timer) app_timer_cancel(s_journey_timer);
  s_journey_timer = app_timer_register(JOURNEY_INTRO_MS, journey_begin_run, NULL);
}

// BACK on secondary screens: a short press goes back one screen (what the
// default did), a long press jumps straight to the home screen however deep
// we are. The loop is bounded so it can never spin.
#define BACK_HOME_HOLD_MS 500

static void back_short_click_handler(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(true);
}

static void back_home_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  for (int i = 0; i < 8; i++) {
    Window *top = window_stack_get_top_window();
    if (!top || top == s_window) break;
    window_stack_pop(i == 0); // animate once, then clear the rest quietly
  }
}

static void subscribe_back_buttons(void) {
  window_single_click_subscribe(BUTTON_ID_BACK, back_short_click_handler);
  window_long_click_subscribe(BUTTON_ID_BACK, BACK_HOME_HOLD_MS, back_home_long_click_handler, NULL);
}

// ---- Battle setup screen - separate window, reached via the Battle menu
// option. Reuses draw_background() (and, on Emery, the same already-loaded
// s_border_bitmap from the main window, since pushing a new window doesn't
// unload the one beneath it) for visual consistency.
static void battle_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GRect cr = content_rect(bounds);

  draw_background(ctx, bounds);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (s_battle_state == JOURNEY_PLAYING) {
    int ground = cr.origin.y + cr.size.h - 12;
    int pet_x = cr.origin.x + cr.size.w / 3;
    int cr_right = cr.origin.x + cr.size.w;

    static char journey_buf[32];
    snprintf(journey_buf, sizeof(journey_buf), "Stage %d: %d/%d", s_journey_played + 1, s_journey_cleared, s_journey_blocks);
    graphics_draw_text(ctx, journey_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, GPoint(cr.origin.x, ground), GPoint(cr_right, ground));

    // Blocks - clipped to the cutout so they never draw over the border art.
    graphics_context_set_fill_color(ctx, GColorBlack);
    for (int i = 0; i < s_journey_blocks; i++) {
      int x0 = s_journey_block_x[i];
      int x1 = x0 + JOURNEY_BLOCK_W;
      if (x0 < cr.origin.x) x0 = cr.origin.x;
      if (x1 > cr_right) x1 = cr_right;
      if (x1 > x0) {
        graphics_fill_rect(ctx, GRect(x0, ground - JOURNEY_BLOCK_H, x1 - x0, JOURNEY_BLOCK_H), 0, GCornerNone);
      }
    }

    int jump_h = journey_jump_height(s_journey_jump_t);
    if (s_pet_bitmap) {
      GRect bmp_bounds = gbitmap_get_bounds(s_pet_bitmap);
      GRect pet_rect = GRect(pet_x - bmp_bounds.size.w / 2, ground - jump_h - bmp_bounds.size.h,
          bmp_bounds.size.w, bmp_bounds.size.h);
      graphics_context_set_compositing_mode(ctx, GCompOpSet);
      graphics_draw_bitmap_in_rect(ctx, s_pet_bitmap, pet_rect);
    } else {
      draw_missing_sprite(ctx, GPoint(pet_x, ground - jump_h - 14));
    }
    return;
  }

  if (s_battle_state == JOURNEY_INTRO) {
    graphics_draw_text(ctx, JOURNEY_STAGE_NAMES[s_journey_played],
        fonts_get_system_font(cr.size.w >= 140 ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD), // narrower Gabbro cutout: smaller font so "Stage Three" fits
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 - 28, cr.size.w, 34),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    static char intro_buf[24];
    snprintf(intro_buf, sizeof(intro_buf), "%d blocks - get ready!", s_journey_blocks);
    graphics_draw_text(ctx, intro_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 10, cr.size.w, 18),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_battle_state == JOURNEY_RESULT) {
    GFont f18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    GFont f14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    GFont f18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    int mid = cr.origin.y + cr.size.h / 2;
    static char res_buf[40];

    if (s_journey_result == JR_CRASH) {
      graphics_draw_text(ctx, "Journey Over", f18b,
          GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      snprintf(res_buf, sizeof(res_buf), "%s: %d/%d", JOURNEY_STAGE_NAMES[s_journey_played], s_journey_cleared, s_journey_blocks);
      graphics_draw_text(ctx, res_buf, f18, GRect(cr.origin.x, mid - 24, cr.size.w, 22),
          GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      graphics_draw_text(ctx, "Hit a block! Try again.", f14, GRect(cr.origin.x, mid + 2, cr.size.w, 18),
          GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    } else if (s_journey_result == JR_STAGE_CLEAR) {
      snprintf(res_buf, sizeof(res_buf), "%s Clear!", JOURNEY_STAGE_NAMES[s_journey_played]);
      graphics_draw_text(ctx, res_buf, f18b,
          GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      graphics_draw_text(ctx, "Congratulations!", f18, GRect(cr.origin.x, mid - 24, cr.size.w, 22),
          GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      snprintf(res_buf, sizeof(res_buf), "Next: %s", JOURNEY_STAGE_NAMES[s_journey_played + 1]);
      graphics_draw_text(ctx, res_buf, f14, GRect(cr.origin.x, mid + 2, cr.size.w, 18),
          GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    } else {
      graphics_draw_text(ctx, "Journey Complete!", f18b,
          GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      graphics_draw_text(ctx, "You finished the Journey Together challenge!", f14,
          GRect(cr.origin.x, mid - 32, cr.size.w, 34), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      if (s_journey_pp_awarded) {
        snprintf(res_buf, sizeof(res_buf), "+%d PP earned!", JOURNEY_REWARD_PP);
        graphics_draw_text(ctx, res_buf, f18, GRect(cr.origin.x, mid + 4, cr.size.w, 22),
            GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
        graphics_draw_text(ctx, "A star now marks your Digimon.", f14, GRect(cr.origin.x, mid + 26, cr.size.w, 34),
            GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      } else {
        graphics_draw_text(ctx, "PP bonus already earned.", f14, GRect(cr.origin.x, mid + 6, cr.size.w, 18),
            GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      }
    }

    graphics_draw_text(ctx, "SELECT: Continue", f14,
        GRect(cr.origin.x, cr.origin.y + cr.size.h - 18, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_battle_state == BATTLE_FIGHTING) {
    graphics_draw_text(ctx, "Fighting!", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    int mid_y = cr.origin.y + cr.size.h / 2 - 6;
    // Bigger sprites (48px Megas) would run into the HP-bar labels, so pull
    // the fighters up just enough to clear them, but never up into the
    // "Fighting!" title.
    if (s_pet_bitmap) {
      int sprite_h = gbitmap_get_bounds(s_pet_bitmap).size.h;
      int bar_label_top = cr.origin.y + cr.size.h - 30 - 16; // top of the "You"/"Wild" labels
      int max_mid = bar_label_top - 2 - sprite_h / 2;
      int min_mid = cr.origin.y + 22 + sprite_h / 2;
      if (mid_y > max_mid) mid_y = max_mid;
      if (mid_y < min_mid) mid_y = min_mid;
    }
    int player_x = cr.origin.x + cr.size.w / 4;
    int opp_x = cr.origin.x + (cr.size.w * 3) / 4;

    // Attacker leans toward the defender while its projectile is in
    // flight - tied to this round's actual roll, not a generic idle bounce.
    bool attacking_now = (s_battle_anim_phase == BATTLE_ANIM_PROJECTILE);
    int player_bounce = (attacking_now && s_projectile_toward_opponent) ? 6 : 0;
    int opp_bounce = (attacking_now && !s_projectile_toward_opponent) ? -6 : 0;

    // During the flicker phase, skip drawing the defender every other
    // sub-frame - the actual blink effect.
    bool skip_player = (s_battle_anim_phase == BATTLE_ANIM_FLICKER) && s_last_hit_was_player && !s_flicker_visible;
    bool skip_opponent = (s_battle_anim_phase == BATTLE_ANIM_FLICKER) && !s_last_hit_was_player && !s_flicker_visible;

    if (!s_pet_bitmap) {
      if (!skip_player) draw_missing_sprite(ctx, GPoint(player_x + player_bounce, mid_y));
      if (!skip_opponent) draw_missing_sprite(ctx, GPoint(opp_x + opp_bounce, mid_y));
    } else {
      GRect bmp_bounds = gbitmap_get_bounds(s_pet_bitmap);
      graphics_context_set_compositing_mode(ctx, GCompOpSet);

      if (!skip_player) {
        GRect player_rect = GRect(player_x + player_bounce - bmp_bounds.size.w / 2,
            mid_y - bmp_bounds.size.h / 2, bmp_bounds.size.w, bmp_bounds.size.h);
        graphics_draw_bitmap_in_rect(ctx, s_pet_bitmap, player_rect);
      }

      // Same sprite stands in for the opponent (same tier fights same
      // tier) - Pebble's basic graphics API has no simple bitmap flip, so
      // this is a mirrored position, not a mirrored sprite.
      if (!skip_opponent) {
        GRect opp_rect = GRect(opp_x + opp_bounce - bmp_bounds.size.w / 2,
            mid_y - bmp_bounds.size.h / 2, bmp_bounds.size.w, bmp_bounds.size.h);
        graphics_draw_bitmap_in_rect(ctx, s_pet_bitmap, opp_rect);
      }
    }

    // Projectile - a small ball traveling from attacker to defender,
    // interpolated across the travel sub-frames. Only visible mid-flight.
    if (s_battle_anim_phase == BATTLE_ANIM_PROJECTILE) {
      GPoint start_pt = s_projectile_toward_opponent ? GPoint(player_x, mid_y) : GPoint(opp_x, mid_y);
      GPoint end_pt = s_projectile_toward_opponent ? GPoint(opp_x, mid_y) : GPoint(player_x, mid_y);
      int proj_x = start_pt.x + ((end_pt.x - start_pt.x) * s_battle_anim_step) / PROJECTILE_STEPS;
      int proj_y = start_pt.y + ((end_pt.y - start_pt.y) * s_battle_anim_step) / PROJECTILE_STEPS;
      graphics_context_set_fill_color(ctx, GColorBlack);
      graphics_fill_circle(ctx, GPoint(proj_x, proj_y), 3);
    }

    int bar_y = cr.origin.y + cr.size.h - 30;
    int bar_w = cr.size.w / 2 - 16;

    GRect player_bar_outline = GRect(player_x - bar_w / 2, bar_y, bar_w, 8);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_rect(ctx, player_bar_outline);
    int player_hp_pct = s_battle_player_max_hp > 0 ?
        (s_battle_player_hp > 0 ? s_battle_player_hp : 0) * 100 / s_battle_player_max_hp : 0;
    GRect player_bar_fill = GRect(player_bar_outline.origin.x + 1, player_bar_outline.origin.y + 1,
        ((player_bar_outline.size.w - 2) * player_hp_pct) / 100, player_bar_outline.size.h - 2);
    graphics_context_set_fill_color(ctx, GColorGreen);
    graphics_fill_rect(ctx, player_bar_fill, 0, GCornerNone);

    GRect opp_bar_outline = GRect(opp_x - bar_w / 2, bar_y, bar_w, 8);
    graphics_draw_rect(ctx, opp_bar_outline);
    int opp_hp_pct = s_battle_opponent_max_hp > 0 ?
        (s_battle_opponent_hp > 0 ? s_battle_opponent_hp : 0) * 100 / s_battle_opponent_max_hp : 0;
    GRect opp_bar_fill = GRect(opp_bar_outline.origin.x + 1, opp_bar_outline.origin.y + 1,
        ((opp_bar_outline.size.w - 2) * opp_hp_pct) / 100, opp_bar_outline.size.h - 2);
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, opp_bar_fill, 0, GCornerNone);

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, "You", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(player_x - bar_w / 2, bar_y - 16, bar_w, 14), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "Wild", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(opp_x - bar_w / 2, bar_y - 16, bar_w, 14), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  if (s_battle_state == BATTLE_RESULT) {
    graphics_draw_text(ctx, s_last_battle_won ? "You Won!" : "You Lost...",
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    static char rounds_buf[32];
    snprintf(rounds_buf, sizeof(rounds_buf), "%d rounds", s_last_battle_rounds);
    graphics_draw_text(ctx, rounds_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 - 40, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    static char outcome_buf[48];
    if (s_last_battle_won) {
      snprintf(outcome_buf, sizeof(outcome_buf), "+%d Vital Points", BATTLE_VP_STAKE);
    } else {
      snprintf(outcome_buf, sizeof(outcome_buf), "-%d Vital Points", BATTLE_VP_STAKE);
    }
    graphics_draw_text(ctx, outcome_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 - 20, cr.size.w, 20),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    static char record_buf[32];
    snprintf(record_buf, sizeof(record_buf), "Record: %d-%d", s_pet.win_count, s_pet.loss_count);
    graphics_draw_text(ctx, record_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 10, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    graphics_draw_text(ctx, "SELECT: Continue", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h - 18, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  // BATTLE_IDLE - setup screen; UP/DOWN toggles Battle / Journey
  graphics_draw_text(ctx, s_journey_mode ? "< Journey >" : "< Battle >",
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  if (s_journey_mode) {
    graphics_draw_text(ctx, "Journey Together", fonts_get_system_font(FONT_KEY_GOTHIC_18),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 - 44, cr.size.w, 22),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    static char stage_line[24];
    int next_stage = s_pet.journey_stage < JOURNEY_STAGE_COUNT ? s_pet.journey_stage : 0;
    snprintf(stage_line, sizeof(stage_line), "Stage %d of %d", next_stage + 1, JOURNEY_STAGE_COUNT);
    graphics_draw_text(ctx, stage_line, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 - 18, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, s_pet.journey_complete ? "Challenge complete!" : "SELECT = jump", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 0, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, "SELECT: Start", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 24, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  } else {
    static char vp_label[32];
    snprintf(vp_label, sizeof(vp_label), "Vital Points: %d/%d", s_pet.vital_points, MAX_VITAL_POINTS);
    graphics_draw_text(ctx, vp_label, fonts_get_system_font(FONT_KEY_GOTHIC_18),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 - 44, cr.size.w, 20),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    GRect vp_outline = GRect(cr.origin.x + 15, cr.origin.y + cr.size.h / 2 - 20, cr.size.w - 30, 14);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_rect(ctx, vp_outline);
    GRect vp_fill = GRect(vp_outline.origin.x + 1, vp_outline.origin.y + 1,
        ((vp_outline.size.w - 2) * s_pet.vital_points) / MAX_VITAL_POINTS, vp_outline.size.h - 2);
    graphics_context_set_fill_color(ctx, GColorBlue);
    graphics_fill_rect(ctx, vp_fill, 0, GCornerNone);

    static char record_buf2[32];
    snprintf(record_buf2, sizeof(record_buf2), "Record: %d-%d", s_pet.win_count, s_pet.loss_count);
    graphics_draw_text(ctx, record_buf2, fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 6, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    graphics_draw_text(ctx, s_pet.vital_points > 0 ? "SELECT: Battle" : "Need VP - walk more!",
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 24, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  graphics_draw_text(ctx, "UP/DOWN: switch mode", fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + cr.size.h - 18, cr.size.w, 16),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// SELECT starts a battle from idle (only if there are Vital Points to
// stake), or dismisses the result back to idle. No-ops while
// BATTLE_FIGHTING is playing out - the timer drives that on its own.
static void battle_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_battle_state == BATTLE_IDLE) {
    if (s_journey_mode) {
      start_journey();
    } else if (s_pet.vital_points > 0) {
      start_battle();
    }
    // else: do nothing - the idle screen already explains why
  } else if (s_battle_state == BATTLE_RESULT || s_battle_state == JOURNEY_RESULT) {
    s_battle_state = BATTLE_IDLE;
    layer_mark_dirty(s_battle_canvas_layer);
  } else if (s_battle_state == JOURNEY_PLAYING) {
    if (s_journey_jump_t < 0) s_journey_jump_t = 0; // no double-jumping mid-air
  }
}

static void battle_mode_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_battle_state != BATTLE_IDLE) return;
  s_journey_mode = !s_journey_mode;
  layer_mark_dirty(s_battle_canvas_layer);
}

static void battle_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, battle_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, battle_mode_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, battle_mode_click_handler);
  subscribe_back_buttons(); // short BACK = back, long BACK = home
}

static void battle_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_battle_state = BATTLE_IDLE; // always start fresh when entering the screen

  s_battle_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_battle_canvas_layer, battle_canvas_update_proc);
  layer_add_child(window_layer, s_battle_canvas_layer);

  window_set_click_config_provider(window, battle_click_config_provider);
}

static void battle_window_unload(Window *window) {
  if (s_battle_round_timer) {
    app_timer_cancel(s_battle_round_timer);
    s_battle_round_timer = NULL;
  }
  if (s_journey_timer) {
    app_timer_cancel(s_journey_timer);
    s_journey_timer = NULL;
  }
  layer_destroy(s_battle_canvas_layer);
  s_battle_canvas_layer = NULL;
}

// ---- Stats screen - long-press DOWN opens this. Surfaces the numbers
// that don't have a home anywhere else yet, most importantly Care
// Mistakes and total Effort, both of which now genuinely gate evolution
// but were previously invisible in the UI entirely.
static int s_stats_page = 0; // 0 = stats, 1 = evolution goal

static void draw_goal_line(GContext *ctx, GRect cr, int y, const char *label, int have, int need, const char *suffix) {
  static char buf[32];
  if (need <= 0) {
    snprintf(buf, sizeof(buf), "%s: N/A", label);
  } else if (have >= need) {
    snprintf(buf, sizeof(buf), "%s: %d/%d%s OK", label, have, need, suffix);
  } else {
    snprintf(buf, sizeof(buf), "%s: %d/%d%s", label, have, need, suffix);
  }
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + y, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void stats_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GRect cr = content_rect(bounds);

  draw_background(ctx, bounds);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (s_stats_page == 1) {
    // ---- Evolution goal page: the strictest form this pet could still
    // become, and how far along each of its requirements is. "OK" = met.
    graphics_draw_text(ctx, "Evo Goal", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    const EvoEdge *goal = (current_tier() == TIER_MEGA) ? NULL : goal_edge();
    if (!goal) {
      graphics_draw_text(ctx, "Fully evolved!", fonts_get_system_font(FONT_KEY_GOTHIC_14),
          GRect(cr.origin.x, cr.origin.y + 40, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    } else {
      static char goal_buf[32];
      snprintf(goal_buf, sizeof(goal_buf), "-> %s", species_name(goal->to));
      graphics_draw_text(ctx, goal_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
          GRect(cr.origin.x, cr.origin.y + 24, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      draw_goal_line(ctx, cr, 40, "PP", current_pp(), goal->pp, "");
      draw_goal_line(ctx, cr, 56, "Steps", s_pet.steps_in_stage, goal->steps, "");
      draw_goal_line(ctx, cr, 72, "Battles", current_battles(), goal->battles, "");
      draw_goal_line(ctx, cr, 88, "Win%", current_win_pct(), goal->win_pct, "%");
    }

    graphics_draw_text(ctx, "SEL: Stats", fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(cr.origin.x, cr.origin.y + cr.size.h - 16, cr.size.w, 16),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  graphics_draw_text(ctx, "Stats", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char mistakes_buf[32];
  snprintf(mistakes_buf, sizeof(mistakes_buf), "Mistakes: %d/%d", s_pet.care_mistakes, CARE_MISTAKES_CAP);
  graphics_draw_text(ctx, mistakes_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 24, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char time_buf[32];
  int hours_needed_stat = hours_required_for_current_stage();
  if (hours_needed_stat >= 0) {
    snprintf(time_buf, sizeof(time_buf), "Time: %dh/%dh", s_pet.hours_in_stage, hours_needed_stat);
  } else {
    snprintf(time_buf, sizeof(time_buf), "Time: MAX");
  }
  graphics_draw_text(ctx, time_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 40, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char pp_buf[32];
  snprintf(pp_buf, sizeof(pp_buf), "PP: %d", current_pp());
  graphics_draw_text(ctx, pp_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 56, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char record_buf[32];
  snprintf(record_buf, sizeof(record_buf), "Record: %d-%d", s_pet.win_count, s_pet.loss_count);
  graphics_draw_text(ctx, record_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 72, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char vp_buf[32];
  snprintf(vp_buf, sizeof(vp_buf), "VP: %d/%d", s_pet.vital_points, MAX_VITAL_POINTS);
  graphics_draw_text(ctx, vp_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 88, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "SEL: Goal", fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + cr.size.h - 16, cr.size.w, 16),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void stats_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_stats_page = (s_stats_page + 1) % 2;
  if (s_stats_canvas_layer) layer_mark_dirty(s_stats_canvas_layer);
}

static void stats_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, stats_select_click_handler);
  subscribe_back_buttons(); // short BACK = back, long BACK = home
}

static void stats_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_stats_page = 0; // always open on the main stats page

  s_stats_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_stats_canvas_layer, stats_canvas_update_proc);
  layer_add_child(window_layer, s_stats_canvas_layer);

  window_set_click_config_provider(window, stats_click_config_provider);
}

static void stats_window_unload(Window *window) {
  layer_destroy(s_stats_canvas_layer);
  s_stats_canvas_layer = NULL;
}

static void idle_bob_step(void *data) {
  s_idle_bob_up = !s_idle_bob_up;
  refresh();
  s_idle_timer = app_timer_register(IDLE_BOB_MS, idle_bob_step, NULL);
}

// Starts or stops the idle bob timer to match the current battery saver
// setting. Safe to call any time - at window load, or live while the app
// is already open if the setting just changed via the phone config page.
static void update_idle_timer(void) {
  if (s_battery_saver) {
    if (s_idle_timer) {
      app_timer_cancel(s_idle_timer);
      s_idle_timer = NULL;
    }
    s_idle_bob_up = false; // rest at the non-bobbed position
  } else if (!s_idle_timer) {
    s_idle_timer = app_timer_register(IDLE_BOB_MS, idle_bob_step, NULL);
  }
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

static void do_feed(void) {
  s_pet.hunger -= 35;
  if (s_pet.hunger < 0) s_pet.hunger = 0;
  vibes_short_pulse();
  save_pet();
  check_mistake_grace(); // may have just resolved a visible issue - cancel the grace timer if so
}

static void do_train(void) {
  if (s_pet.energy < 25) return; // no vibration on a blocked attempt - nothing actually happened
  s_pet.strength += 20;
  if (s_pet.strength > MAX_STAT) s_pet.strength = MAX_STAT;
  s_pet.energy -= 25;
  s_pet.training_count++;
  vibes_short_pulse();
  save_pet();
  check_mistake_grace();
}

static void do_sleep(void) {
  s_pet.energy += 60;
  if (s_pet.energy > MAX_STAT) s_pet.energy = MAX_STAT;
  s_pet.hunger += 15;
  if (s_pet.hunger > MAX_STAT) s_pet.hunger = MAX_STAT;
  vibes_short_pulse();
  save_pet();
  check_mistake_grace();
}

static void do_clean(void) {
  s_pet.has_mess = false;
  s_pet.hours_since_clean = 0;
  vibes_short_pulse();
  save_pet();
  check_mistake_grace();
}

// UP always opens the Battle screen directly - no longer part of the
// Feed/Train/Sleep/Clean cycle at all, sidestepping any ambiguity with
// SELECT's long-press (manual sleep) entirely.
static void finish_hatch(Species egg); // defined after reset_pet()

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_choosing_egg) { finish_hatch(SP_BOTA); return; }
  if (s_evolving || !s_pet.alive) return;
  window_stack_push(s_battle_window, true);
}

// DOWN alone now cycles through the remaining 4 menu items on a short
// press. A long press opens the Stats screen instead.
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_choosing_egg) { finish_hatch(SP_DOKI); return; }
  if (s_evolving || !s_pet.alive) return;
  s_selected_action = (s_selected_action + 1) % ACTION_COUNT;
  refresh();
}

static void start_hatch(void); // defined after reset_pet()

// Long DOWN: Stats screen while alive; on the R.I.P. screen it hatches a
// fresh egg instead (the long hold doubles as the "are you sure").
static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_evolving || s_choosing_egg) return;
  if (!s_pet.alive) {
    start_hatch();
    return;
  }
  window_stack_push(s_stats_window, true);
}

#if defined(PBL_TOUCH)
// Tapping the pet sprite itself opens Stats - a second, alternate way in
// alongside the DOWN long-press (which stays as-is for now; may get
// repurposed later once touch is confirmed working well). Only acts on
// TouchEvent_Liftoff (the completed tap), checked against exactly the
// same bounding box draw_pet() actually renders into - including the
// idle bob offset, so "touching the Digimon" means touching where it
// visually is, not some fixed screen region.
static void touch_handler(const TouchEvent *event, void *context) {
  if (event->type != TouchEvent_Liftoff) return;
  if (s_choosing_egg || s_evolving || !s_pet.alive) return;

  GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
  GPoint center = pet_center_point(bounds);
  // A missing sprite is drawn as a 40x28 "N/A" box, so it stays tappable.
  GRect bmp_bounds = s_pet_bitmap ? gbitmap_get_bounds(s_pet_bitmap) : GRect(0, 0, 40, 28);
  // Hit area is TWICE the sprite's width and height (centered on it), so
  // the Digimon is easy to tap - clamped to the screen cutout so it never
  // reaches the border art.
  GRect pet_rect = GRect(
      center.x - bmp_bounds.size.w,
      center.y - bmp_bounds.size.h,
      bmp_bounds.size.w * 2,
      bmp_bounds.size.h * 2);
  GRect cr_touch = content_rect(bounds);
  if (pet_rect.origin.x < cr_touch.origin.x) {
    pet_rect.size.w -= cr_touch.origin.x - pet_rect.origin.x;
    pet_rect.origin.x = cr_touch.origin.x;
  }
  if (pet_rect.origin.y < cr_touch.origin.y) {
    pet_rect.size.h -= cr_touch.origin.y - pet_rect.origin.y;
    pet_rect.origin.y = cr_touch.origin.y;
  }
  if (pet_rect.origin.x + pet_rect.size.w > cr_touch.origin.x + cr_touch.size.w) {
    pet_rect.size.w = cr_touch.origin.x + cr_touch.size.w - pet_rect.origin.x;
  }
  if (pet_rect.origin.y + pet_rect.size.h > cr_touch.origin.y + cr_touch.size.h) {
    pet_rect.size.h = cr_touch.origin.y + cr_touch.size.h - pet_rect.origin.y;
  }

  bool inside = (event->x >= pet_rect.origin.x && event->x < pet_rect.origin.x + pet_rect.size.w &&
                 event->y >= pet_rect.origin.y && event->y < pet_rect.origin.y + pet_rect.size.h);
  if (inside) {
    window_stack_push(s_stats_window, true);
  }
}
#endif

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_choosing_egg || s_evolving || !s_pet.alive) return;
  switch (s_selected_action) {
    case ACTION_FEED: do_feed(); break;
    case ACTION_TRAIN: do_train(); break;
    case ACTION_SLEEP: do_sleep(); break;
    case ACTION_CLEAN: do_clean(); break;
    default: break;
  }
  refresh();
}

// Long-press on SELECT toggles BE-style manual sleep on/off - our
// adaptation of "hold two buttons together" for a 3-button watch.
static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_choosing_egg || s_evolving || !s_pet.alive) return;
  s_pet.manually_asleep = !s_pet.manually_asleep;
  save_pet();
  refresh();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, STATS_HOLD_MS, down_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, MANUAL_SLEEP_HOLD_MS, select_long_click_handler, NULL);
}

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  load_pet_bitmap();

#if defined(PBL_PLATFORM_EMERY)
  s_border_bitmap = gbitmap_create_with_resource(RESOURCE_ID_DIGIVICE_BORDER);
#elif defined(PBL_PLATFORM_GABBRO)
  s_border_bitmap = gbitmap_create_with_resource(RESOURCE_ID_DIGIVICE_ROUND_BORDER);
#endif

  update_idle_timer(); // starts it, unless battery saver is already on
  check_mistake_grace(); // starts the grace period fresh if there's already a visible issue - a fair chance, not an instant penalty

#if defined(PBL_TOUCH)
  touch_service_subscribe(touch_handler, NULL);
#endif
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  if (s_pet_bitmap) {
    gbitmap_destroy(s_pet_bitmap);
    s_pet_bitmap = NULL;
  }
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  if (s_border_bitmap) {
    gbitmap_destroy(s_border_bitmap);
    s_border_bitmap = NULL;
  }
#endif
  if (s_idle_timer) {
    app_timer_cancel(s_idle_timer);
    s_idle_timer = NULL;
  }
  if (s_evolve_timer) {
    app_timer_cancel(s_evolve_timer);
    s_evolve_timer = NULL;
  }
  if (s_mistake_grace_timer) {
    // Closing the app never counts as a mistake by itself - only
    // genuinely ignoring a visible issue while the app stays open does.
    app_timer_cancel(s_mistake_grace_timer);
    s_mistake_grace_timer = NULL;
  }
  s_mistake_grace_active = false;

#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
}

// Full reset, triggered from the phone-side config page. Puts the pet
// back to exactly the same fresh-egg state as a brand new install, and
// makes sure whichever screen(s) are currently loaded redraw with it.
static void reset_pet(void) {
  if (s_evolve_timer) {
    app_timer_cancel(s_evolve_timer);
    s_evolve_timer = NULL;
  }
  s_evolving = false;

  if (s_mistake_grace_timer) {
    app_timer_cancel(s_mistake_grace_timer);
    s_mistake_grace_timer = NULL;
  }
  s_mistake_grace_active = false;

  s_pet.hunger = 20;
  s_pet.energy = 90;
  s_pet.strength = 0;
  s_pet.age_hours = 0;
  s_pet.hours_in_stage = 0;
  s_pet.steps_in_stage = 0;
  s_pet.training_count = 0;
  s_pet.species = SP_BOTA;
  s_pet.alive = true;
  s_pet.last_check = time(NULL);
  s_pet.has_mess = false;
  s_pet.hours_since_clean = 0;
  s_pet.care_mistakes = 0;
  s_pet.steps_baseline_today = 0;
  s_pet.last_check_day_marker = 0;
  s_pet.manually_asleep = false;
  s_pet.vital_points = 0;
  s_pet.win_count = 0;
  s_pet.loss_count = 0;
  s_pet.last_steps_shortfall = 0;
  s_pet.last_effort_shortfall = 0;
  s_pet.journey_stage = 0;
  s_pet.journey_complete = false;

  save_pet();
  load_pet_bitmap();

  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
  if (s_stats_canvas_layer) layer_mark_dirty(s_stats_canvas_layer);
}

// Reset, then ask which egg to hatch (Botamon or Dokimon line). Used by
// both the on-watch hold on the R.I.P. screen and the phone reset. The
// pet is already a fresh Botamon at this point, so leaving the app here
// simply keeps that default.
static void start_hatch(void) {
  reset_pet(); // also clears any evolve timer
  s_choosing_egg = true;
  vibes_short_pulse();
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

// The chosen egg takes over, with the black "Hatching..." screen reusing
// the evolution screen and its timer.
static void finish_hatch(Species egg) {
  s_choosing_egg = false;
  s_pet.species = egg;
  save_pet();
  load_pet_bitmap();

  s_transition_text = "Hatching...";
  s_evolving = true;
  vibes_double_pulse();
  if (s_evolve_timer) app_timer_cancel(s_evolve_timer);
  s_evolve_timer = app_timer_register(HATCH_DURATION_MS, evolve_timer_callback, NULL);
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

// ---- Save export / import --------------------------------------------
// The watch can't write files, so the pet travels as ONE text value
// (message key "SaveData") in both directions:
//   watch -> phone: a snapshot after saves, which the phone keeps and
//                   shows as JSON on the settings page (Export).
//   phone -> watch: the same format, when you paste JSON into the
//                   settings page (Import).
// Format: comma-separated integers, in exactly this order:
//   version, species, hunger, energy, strength, age_hours, hours_in_stage,
//   steps_in_stage, training_count, alive, has_mess, hours_since_clean,
//   care_mistakes, manually_asleep, vital_points, win_count, loss_count,
//   last_steps_shortfall, last_effort_shortfall
// The phone script (index.js) uses the same order - change both together.
// Deliberately NOT included: last_check and the step baseline. Import
// resets them to "now" so the pet doesn't age by the time spent away and
// today's steps aren't counted twice.
#define SAVE_FORMAT_VERSION 1
#define SAVE_FIELD_COUNT 19
#define SNAPSHOT_DEBOUNCE_MS 2500
#define SNAPSHOT_FIRST_DELAY_MS 4000 // gives the phone's JS time to start after launch
#define IMPORT_RESTORE_MS 2500
#define SAVE_COUNT_MAX 999999

static AppTimer *s_snapshot_timer = NULL;
static bool s_app_message_ready = false;

static void send_snapshot(void *data) {
  s_snapshot_timer = NULL;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return; // busy or not connected - the next save retries

  static char buf[200];
  snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
      SAVE_FORMAT_VERSION, (int)s_pet.species, s_pet.hunger, s_pet.energy, s_pet.strength,
      s_pet.age_hours, s_pet.hours_in_stage, s_pet.steps_in_stage, s_pet.training_count,
      s_pet.alive ? 1 : 0, s_pet.has_mess ? 1 : 0, s_pet.hours_since_clean,
      s_pet.care_mistakes, s_pet.manually_asleep ? 1 : 0, s_pet.vital_points,
      s_pet.win_count, s_pet.loss_count, s_pet.last_steps_shortfall, s_pet.last_effort_shortfall);
  dict_write_cstring(iter, MESSAGE_KEY_SaveData, buf);
  app_message_outbox_send();
}

// Debounced: several saves in a row (e.g. an action then an evolution)
// collapse into one message, which keeps Bluetooth use low.
static void queue_snapshot(void) {
  if (!s_app_message_ready) return;
  if (s_snapshot_timer) app_timer_cancel(s_snapshot_timer);
  s_snapshot_timer = app_timer_register(SNAPSHOT_DEBOUNCE_MS, send_snapshot, NULL);
}

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Parses and applies a save string. Returns false (and changes nothing)
// if it isn't a well-formed version-1 save. Values are clamped to legal
// ranges rather than rejected, so a hand-edited number can't break the pet.
// Reads one (optionally negative) whole number and advances the cursor.
// Hand-rolled on purpose: it behaves identically everywhere, instead of
// relying on the platform's strtol.
static bool parse_int_field(const char **cursor, int *out) {
  const char *p = *cursor;
  bool negative = false;
  if (*p == '-') { negative = true; p++; }
  if (*p < '0' || *p > '9') return false;
  long n = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (*p - '0');
    if (n > 100000000L) n = 100000000L; // cap so a huge number can't overflow; clamped later anyway
    p++;
  }
  *out = (int)(negative ? -n : n);
  *cursor = p;
  return true;
}

static bool import_save(const char *csv) {
  int v[SAVE_FIELD_COUNT];
  const char *p = csv;
  for (int i = 0; i < SAVE_FIELD_COUNT; i++) {
    if (!parse_int_field(&p, &v[i])) {
      APP_LOG(APP_LOG_LEVEL_INFO, "Import rejected: field %d isn't a number", i);
      return false;
    }
    if (i < SAVE_FIELD_COUNT - 1) {
      if (*p != ',') {
        APP_LOG(APP_LOG_LEVEL_INFO, "Import rejected: expected a comma after field %d", i);
        return false;
      }
      p++;
    }
  }
  if (*p != '\0') {
    APP_LOG(APP_LOG_LEVEL_INFO, "Import rejected: extra text after the last field");
    return false;
  }
  if (v[0] != SAVE_FORMAT_VERSION) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Import rejected: version %d (expected %d)", v[0], SAVE_FORMAT_VERSION);
    return false;
  }
  if (v[1] < 0 || v[1] >= SP_COUNT) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Import rejected: species %d out of range (0-%d)", v[1], SP_COUNT - 1);
    return false;
  }

  // Valid - stop anything mid-flight before swapping the pet underneath it.
  if (s_evolve_timer) { app_timer_cancel(s_evolve_timer); s_evolve_timer = NULL; }
  if (s_mistake_grace_timer) { app_timer_cancel(s_mistake_grace_timer); s_mistake_grace_timer = NULL; }
  s_mistake_grace_active = false;
  if (s_battle_round_timer) { app_timer_cancel(s_battle_round_timer); s_battle_round_timer = NULL; }
  if (s_journey_timer) { app_timer_cancel(s_journey_timer); s_journey_timer = NULL; }
  s_battle_state = BATTLE_IDLE;
  s_choosing_egg = false;

  s_pet.species = (Species)v[1];
  s_pet.hunger = clamp_int(v[2], 0, MAX_STAT);
  s_pet.energy = clamp_int(v[3], 0, MAX_STAT);
  s_pet.strength = clamp_int(v[4], 0, MAX_STAT);
  s_pet.age_hours = clamp_int(v[5], 0, SAVE_COUNT_MAX);
  s_pet.hours_in_stage = clamp_int(v[6], 0, SAVE_COUNT_MAX);
  s_pet.steps_in_stage = clamp_int(v[7], 0, SAVE_COUNT_MAX);
  s_pet.training_count = clamp_int(v[8], 0, SAVE_COUNT_MAX);
  s_pet.alive = v[9] != 0;
  s_pet.has_mess = v[10] != 0;
  s_pet.hours_since_clean = clamp_int(v[11], 0, SAVE_COUNT_MAX);
  s_pet.care_mistakes = clamp_int(v[12], 0, SAVE_COUNT_MAX);
  s_pet.manually_asleep = v[13] != 0;
  s_pet.vital_points = clamp_int(v[14], 0, MAX_VITAL_POINTS);
  s_pet.win_count = clamp_int(v[15], 0, SAVE_COUNT_MAX);
  s_pet.loss_count = clamp_int(v[16], 0, SAVE_COUNT_MAX);
  s_pet.last_steps_shortfall = clamp_int(v[17], 0, SAVE_COUNT_MAX);
  s_pet.last_effort_shortfall = clamp_int(v[18], 0, SAVE_COUNT_MAX);

  // Time and steps restart from this moment.
  s_pet.last_check = time(NULL);
  s_pet.last_check_day_marker = 0;
  s_pet.steps_baseline_today = 0;
  (void)steps_since_last_check(s_pet.last_check, s_pet.last_check); // re-baselines to today's current steps

  save_pet();
  load_pet_bitmap();

  s_transition_text = "Restoring...";
  s_evolving = true;
  vibes_double_pulse();
  s_evolve_timer = app_timer_register(IMPORT_RESTORE_MS, evolve_timer_callback, NULL);

  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);
  if (s_stats_canvas_layer) layer_mark_dirty(s_stats_canvas_layer);
  check_mistake_grace();
  return true;
}

static void inbox_received_handler(DictionaryIterator *iterator, void *context) {
  // ResetDigimon: 1 = reset and ask which egg, 2 = hatch Botamon, 3 = hatch Dokimon.
  Tuple *reset_tuple = dict_find(iterator, MESSAGE_KEY_ResetDigimon);
  if (reset_tuple) {
    int mode = (int)reset_tuple->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "Reset received, mode %d", mode);
    if (mode == 2 || mode == 3) {
      reset_pet();
      finish_hatch(mode == 2 ? SP_BOTA : SP_DOKI);
    } else {
      start_hatch();
    }
  }

  Tuple *battery_saver_tuple = dict_find(iterator, MESSAGE_KEY_BatterySaver);
  if (battery_saver_tuple) {
    s_battery_saver = battery_saver_tuple->value->int32 == 1;
    persist_write_bool(KEY_BATTERY_SAVER, s_battery_saver);
    update_idle_timer(); // takes effect immediately, not just on next launch
  }

  Tuple *save_tuple = dict_find(iterator, MESSAGE_KEY_SaveData);
  if (save_tuple && save_tuple->type == TUPLE_CSTRING) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Import received: %s", save_tuple->value->cstring);
    if (import_save(save_tuple->value->cstring)) {
      APP_LOG(APP_LOG_LEVEL_INFO, "Import applied: species %d, hunger %d", (int)s_pet.species, s_pet.hunger);
    } else {
      APP_LOG(APP_LOG_LEVEL_INFO, "Import REJECTED (malformed save string)");
      vibes_long_pulse(); // rejected - nothing was changed
    }
  } else if (save_tuple) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SaveData arrived but wasn't a string (type %d)", (int)save_tuple->type);
  }
}

static void init(void) {
  load_pet();
  apply_time_decay();
  s_battery_saver = persist_exists(KEY_BATTERY_SAVER) ? persist_read_bool(KEY_BATTERY_SAVER) : false;

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  s_battle_window = window_create();
  window_set_window_handlers(s_battle_window, (WindowHandlers) {
    .load = battle_window_load,
    .unload = battle_window_unload,
  });
  // No click_config_provider needed - BACK's default behavior (pop the
  // window) is exactly what we want here, and there's nothing else to do
  // on this screen yet.

  s_stats_window = window_create();
  window_set_window_handlers(s_stats_window, (WindowHandlers) {
    .load = stats_window_load,
    .unload = stats_window_unload,
  });

  // Small explicit buffers, not the platform maximum: a save string is
  // ~100 characters, plus the settings flags that can ride along with it.
  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(256, 256);
  s_app_message_ready = true;
  // First snapshot a few seconds after launch, once the phone's JS is up.
  s_snapshot_timer = app_timer_register(SNAPSHOT_FIRST_DELAY_MS, send_snapshot, NULL);
}

static void deinit(void) {
  if (s_star_path) { gpath_destroy(s_star_path); s_star_path = NULL; }
  save_pet();
  window_destroy(s_stats_window);
  window_destroy(s_battle_window);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}