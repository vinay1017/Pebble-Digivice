#include <pebble.h>
#include <stdlib.h> // rand()/srand() for battle coin flips

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

#define STAGE_HOURS_BOTAMON 1
#define STAGE_HOURS_KOROMON 23
#define STAGE_HOURS_AGUMON 48
#define STAGE_HOURS_GREYMON 96
#define STAGE_STEPS_BOTAMON 500
#define STAGE_STEPS_KOROMON 5000
#define STAGE_STEPS_AGUMON 15000
#define STAGE_STEPS_GREYMON 30000

// Evolution redesign, matching the real Digimon v-pet's actual mechanic
// (verified against official manuals) more closely than a pure time/step
// timer: successful Training builds toward an "Effort" meter (4 successful
// trains = 1 Effort Heart, same ratio the real device uses), and since we
// want steps to matter too, steps ALSO build Effort at their own rate -
// both paths feed the same pool rather than being two separate systems.
// Evolution normally needs hours-in-stage AND enough Effort AND care
// mistakes staying under a cap (mirroring the real "Care Mistakes" system,
// see care-mistake notes below) - OR, for someone extremely active, a much
// higher steps count alone can bypass all of that entirely.
#define TRAINS_PER_EFFORT_HEART 4
#define STEPS_PER_EFFORT_HEART 500
#define EFFORT_REQUIRED_BOTAMON 1
#define EFFORT_REQUIRED_KOROMON 2
#define EFFORT_REQUIRED_AGUMON 3
#define EFFORT_REQUIRED_GREYMON 5
#define CARE_MISTAKES_CAP 3           // stay at or under this many per stage for normal evolution
#define SUPER_ACTIVE_STEPS_MULTIPLIER 3 // steps_required * this = alternate all-steps bypass

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
#define BATTLE_LOSS_VP_PENALTY 20   // the "stake" of the gamble - lost VP on a loss
#define BATTLE_WIN_VP_REWARD 15     // slightly less than the loss penalty, so battling isn't a guaranteed-profit loop
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

typedef enum {
  STAGE_BOTAMON,
  STAGE_KOROMON,
  STAGE_AGUMON,
  STAGE_GREYMON,
  STAGE_METALGREYMON
} Stage;

typedef struct {
  int hunger;
  int energy;
  int strength;
  int age_hours;
  int hours_in_stage;
  int steps_in_stage;
  int training_count;
  Stage stage;
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
} PetState;

static Window *s_window;
static Window *s_battle_window;
static Window *s_stats_window;
static Layer *s_canvas_layer;
static Layer *s_battle_canvas_layer;
static Layer *s_stats_canvas_layer;
static PetState s_pet;
static GBitmap *s_pet_bitmap = NULL;
#if defined(PBL_PLATFORM_EMERY)
static GBitmap *s_border_bitmap = NULL;
#endif
static AppTimer *s_idle_timer = NULL;
static bool s_idle_bob_up = false;
static bool s_evolving = false;
static AppTimer *s_evolve_timer = NULL;

// Battle screen state - transient, not persisted (just "what to show right now")
typedef enum { BATTLE_IDLE, BATTLE_FIGHTING, BATTLE_RESULT } BattleScreenState;
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
  KEY_LOSS_COUNT
};

static bool is_night_hour(int hour) {
  return hour >= NIGHT_START_HOUR || hour < DAY_START_HOUR;
}

static void save_pet(void) {
  persist_write_int(KEY_HUNGER, s_pet.hunger);
  persist_write_int(KEY_ENERGY, s_pet.energy);
  persist_write_int(KEY_STRENGTH, s_pet.strength);
  persist_write_int(KEY_AGE_HOURS, s_pet.age_hours);
  persist_write_int(KEY_TRAINING_COUNT, s_pet.training_count);
  persist_write_int(KEY_STAGE, (int)s_pet.stage);
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
}

static void load_pet(void) {
  if (persist_exists(KEY_HUNGER)) {
    s_pet.hunger = persist_read_int(KEY_HUNGER);
    s_pet.energy = persist_read_int(KEY_ENERGY);
    s_pet.strength = persist_read_int(KEY_STRENGTH);
    s_pet.age_hours = persist_read_int(KEY_AGE_HOURS);
    s_pet.training_count = persist_read_int(KEY_TRAINING_COUNT);
    s_pet.stage = (Stage)persist_read_int(KEY_STAGE);
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
  } else {
    s_pet.hunger = 20;
    s_pet.energy = 90;
    s_pet.strength = 0;
    s_pet.age_hours = 0;
    s_pet.hours_in_stage = 0;
    s_pet.steps_in_stage = 0;
    s_pet.training_count = 0;
    s_pet.stage = STAGE_BOTAMON;
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
  }
}

static uint32_t resource_for_stage(Stage stage) {
  switch (stage) {
    case STAGE_BOTAMON: return RESOURCE_ID_BOTAMON;
    case STAGE_KOROMON: return RESOURCE_ID_KOROMON;
    case STAGE_AGUMON: return RESOURCE_ID_AGUMON;
    case STAGE_GREYMON: return RESOURCE_ID_GREYMON;
    case STAGE_METALGREYMON: return RESOURCE_ID_METALGREYMON;
    default: return RESOURCE_ID_BOTAMON;
  }
}

static void load_pet_bitmap(void) {
  if (s_pet_bitmap) {
    gbitmap_destroy(s_pet_bitmap);
  }
  s_pet_bitmap = gbitmap_create_with_resource(resource_for_stage(s_pet.stage));
}

static int hours_required_for_current_stage(void) {
  switch (s_pet.stage) {
    case STAGE_BOTAMON: return STAGE_HOURS_BOTAMON;
    case STAGE_KOROMON: return STAGE_HOURS_KOROMON;
    case STAGE_AGUMON: return STAGE_HOURS_AGUMON;
    case STAGE_GREYMON: return STAGE_HOURS_GREYMON;
    default: return -1;
  }
}

static int steps_required_for_current_stage(void) {
  switch (s_pet.stage) {
    case STAGE_BOTAMON: return STAGE_STEPS_BOTAMON;
    case STAGE_KOROMON: return STAGE_STEPS_KOROMON;
    case STAGE_AGUMON: return STAGE_STEPS_AGUMON;
    case STAGE_GREYMON: return STAGE_STEPS_GREYMON;
    default: return -1;
  }
}

static int effort_required_for_current_stage(void) {
  switch (s_pet.stage) {
    case STAGE_BOTAMON: return EFFORT_REQUIRED_BOTAMON;
    case STAGE_KOROMON: return EFFORT_REQUIRED_KOROMON;
    case STAGE_AGUMON: return EFFORT_REQUIRED_AGUMON;
    case STAGE_GREYMON: return EFFORT_REQUIRED_GREYMON;
    default: return -1;
  }
}

static Stage next_stage(Stage stage) {
  switch (stage) {
    case STAGE_BOTAMON: return STAGE_KOROMON;
    case STAGE_KOROMON: return STAGE_AGUMON;
    case STAGE_AGUMON: return STAGE_GREYMON;
    case STAGE_GREYMON: return STAGE_METALGREYMON;
    default: return STAGE_METALGREYMON;
  }
}

static void refresh(void); // forward declaration - used by evolve_timer_callback below

static void evolve_timer_callback(void *data) {
  s_evolving = false;
  s_evolve_timer = NULL;
  refresh();
}

static void check_evolution(void) {
  if (!s_pet.alive) return;
  if (s_pet.stage == STAGE_METALGREYMON) return;

  int hours_needed = hours_required_for_current_stage();
  int steps_needed = steps_required_for_current_stage();
  int effort_needed = effort_required_for_current_stage();

  bool hours_met = (hours_needed >= 0) && (s_pet.hours_in_stage >= hours_needed);

  // Both successful Training and steps feed the SAME Effort pool - matches
  // the real device's "4 successful trainings = 1 Effort Heart" while also
  // letting real-world walking count toward the same goal.
  int total_effort = (s_pet.training_count / TRAINS_PER_EFFORT_HEART) +
                      (s_pet.steps_in_stage / STEPS_PER_EFFORT_HEART);
  bool effort_met = (effort_needed >= 0) && (total_effort >= effort_needed);

  bool care_ok = s_pet.care_mistakes <= CARE_MISTAKES_CAP;

  bool normal_path = hours_met && effort_met && care_ok;

  // Extremely high steps alone can bypass everything else - a nod to the
  // "just hit a big number" idea, on top of the authentic care-based path.
  bool super_active_path = (steps_needed >= 0) &&
      (s_pet.steps_in_stage >= steps_needed * SUPER_ACTIVE_STEPS_MULTIPLIER);

  if (normal_path || super_active_path) {
    s_pet.stage = next_stage(s_pet.stage);
    s_pet.hours_in_stage = 0;
    s_pet.steps_in_stage = 0;
    s_pet.training_count = 0;
    s_pet.care_mistakes = 0; // resets each evolution, matching the real device
    s_pet.win_count = 0;
    s_pet.loss_count = 0;
    load_pet_bitmap();

    s_evolving = true;
    vibes_short_pulse();
    if (s_evolve_timer) app_timer_cancel(s_evolve_timer);
    s_evolve_timer = app_timer_register(EVOLVE_DURATION_MS, evolve_timer_callback, NULL);
  }
}

static int steps_since_last_check(time_t last_check, time_t now) {
#if defined(PBL_HEALTH)
  int today_steps = (int)health_service_sum_today(HealthMetricStepCount);

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
    bool was_hunger_critical = (s_pet.hunger >= ATTENTION_HUNGER_THRESHOLD);
    bool was_energy_critical = (s_pet.energy <= ATTENTION_ENERGY_THRESHOLD);

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

      // Care mistake: a real mistake is an unanswered "call" - hunger/energy
      // crossing INTO the critical zone and not being addressed, not simply
      // interacting at the wrong time (waking the pet was never a mistake
      // on the real device - only ignoring an actual need was). Edge-
      // triggered so a mistake only counts once per neglect episode, not
      // once per hour it stays critical.
      bool hunger_critical_now = (s_pet.hunger >= ATTENTION_HUNGER_THRESHOLD);
      bool energy_critical_now = (s_pet.energy <= ATTENTION_ENERGY_THRESHOLD);
      if (hunger_critical_now && !was_hunger_critical) s_pet.care_mistakes++;
      if (energy_critical_now && !was_energy_critical) s_pet.care_mistakes++;
      was_hunger_critical = hunger_critical_now;
      was_energy_critical = energy_critical_now;
    }

    s_pet.age_hours += hours_elapsed;   // lifetime stat - counts every hour, asleep or not
    s_pet.hours_in_stage += awake_hours; // evolution timer - only counts awake hours

    s_pet.hours_since_clean += hours_elapsed;
    if (!s_pet.has_mess && s_pet.hours_since_clean >= MESS_HOURS_THRESHOLD) {
      s_pet.has_mess = true;
      s_pet.care_mistakes++;
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

static bool needs_attention(void) {
  return s_pet.alive && (s_pet.hunger >= ATTENTION_HUNGER_THRESHOLD ||
                          s_pet.energy <= ATTENTION_ENERGY_THRESHOLD ||
                          s_pet.has_mess);
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
#if defined(PBL_PLATFORM_EMERY)
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

static void draw_pet(GContext *ctx, GRect bounds) {
  GPoint center = pet_center_point(bounds);

  if (!s_pet.alive) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_draw_line(ctx, GPoint(center.x - 15, center.y - 15), GPoint(center.x + 15, center.y + 15));
    graphics_draw_line(ctx, GPoint(center.x + 15, center.y - 15), GPoint(center.x - 15, center.y + 15));
    return;
  }

  if (!s_pet_bitmap) return;

  GRect bmp_bounds = gbitmap_get_bounds(s_pet_bitmap);
  GRect draw_rect = GRect(
      center.x - bmp_bounds.size.w / 2,
      center.y - bmp_bounds.size.h / 2,
      bmp_bounds.size.w,
      bmp_bounds.size.h);
  graphics_context_set_compositing_mode(ctx, GCompOpSet);
  graphics_draw_bitmap_in_rect(ctx, s_pet_bitmap, draw_rect);
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

static const char *stage_name(Stage stage) {
  switch (stage) {
    case STAGE_BOTAMON: return "Botamon";
    case STAGE_KOROMON: return "Koromon";
    case STAGE_AGUMON: return "Agumon";
    case STAGE_GREYMON: return "Greymon";
    case STAGE_METALGREYMON: return "MetalGreymon";
    default: return "?";
  }
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

  if (s_evolving) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "Evolving...", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
        GRect(0, bounds.size.h / 2 - 14, bounds.size.w, 28),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  GRect cr = content_rect(bounds);
  int cr_bottom = cr.origin.y + cr.size.h;

  draw_background(ctx, bounds);
  draw_pet(ctx, bounds);
  draw_mess(ctx, bounds);
  draw_attention_icon(ctx, bounds);
  draw_sleep_indicator(ctx, bounds);

  graphics_context_set_text_color(ctx, GColorBlack);

  static char stage_buf[32];
  snprintf(stage_buf, sizeof(stage_buf), "%s", s_pet.alive ? stage_name(s_pet.stage) : "R.I.P.");
  graphics_draw_text(ctx, stage_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char progress_label[32];
  int hours_needed = hours_required_for_current_stage();
  int steps_needed = steps_required_for_current_stage();
  int pct = 0;

  if (s_pet.stage != STAGE_METALGREYMON) {
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
  }
}

static void refresh(void) {
  layer_mark_dirty(s_canvas_layer);
}

// Base max HP per tier - same for every Digimon in that tier (real per-
// species stats come later). +100 per tier, matching the pattern you set.
static int base_hp_for_stage(Stage stage) {
  switch (stage) {
    case STAGE_BOTAMON: return 100;
    case STAGE_KOROMON: return 100;
    case STAGE_AGUMON: return 200;
    case STAGE_GREYMON: return 300;
    case STAGE_METALGREYMON: return 400;
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
    // A win grants a full Effort Heart's worth of credit directly - real,
    // immediate progress toward evolution - plus Vital Points, the reward
    // side of the same stake a loss costs you.
    s_pet.training_count += TRAINS_PER_EFFORT_HEART;
    s_pet.win_count++;
    s_pet.vital_points += BATTLE_WIN_VP_REWARD;
    if (s_pet.vital_points > MAX_VITAL_POINTS) s_pet.vital_points = MAX_VITAL_POINTS;
  } else {
    // The stake of the gamble: losing costs Vital Points, the exact
    // resource battling is built around.
    s_pet.vital_points -= BATTLE_LOSS_VP_PENALTY;
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
static void start_battle(void) {
  s_battle_player_max_hp = base_hp_for_stage(s_pet.stage) + s_pet.vital_points * 10;
  s_battle_opponent_max_hp = base_hp_for_stage(s_pet.stage);
  s_battle_player_hp = s_battle_player_max_hp;
  s_battle_opponent_hp = s_battle_opponent_max_hp;
  s_last_battle_rounds = 0;
  s_last_hit_was_player = false;

  s_battle_state = BATTLE_FIGHTING;
  if (s_battle_canvas_layer) layer_mark_dirty(s_battle_canvas_layer);

  if (s_battle_round_timer) app_timer_cancel(s_battle_round_timer);
  start_next_round(NULL);
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

  if (s_battle_state == BATTLE_FIGHTING) {
    graphics_draw_text(ctx, "Fighting!", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    int mid_y = cr.origin.y + cr.size.h / 2 - 6;
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

    if (s_pet_bitmap) {
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
      snprintf(outcome_buf, sizeof(outcome_buf), "+%d Vital Points", BATTLE_WIN_VP_REWARD);
    } else {
      snprintf(outcome_buf, sizeof(outcome_buf), "-%d Vital Points", BATTLE_LOSS_VP_PENALTY);
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

  // BATTLE_IDLE
  graphics_draw_text(ctx, "Battle Setup", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

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

  graphics_draw_text(ctx, "SELECT: Battle", fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + cr.size.h / 2 + 24, cr.size.w, 16),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "< Back", fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + cr.size.h - 18, cr.size.w, 16),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// SELECT starts a battle from idle, or dismisses the result back to idle.
// No-ops while BATTLE_FIGHTING is playing out - the timer drives that on
// its own.
static void battle_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_battle_state == BATTLE_IDLE) {
    start_battle();
  } else if (s_battle_state == BATTLE_RESULT) {
    s_battle_state = BATTLE_IDLE;
    layer_mark_dirty(s_battle_canvas_layer);
  }
}

static void battle_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, battle_select_click_handler);
  // BACK is left at its default behavior (pop back to the main screen)
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
  layer_destroy(s_battle_canvas_layer);
  s_battle_canvas_layer = NULL;
}

// ---- Stats screen - long-press DOWN opens this. Surfaces the numbers
// that don't have a home anywhere else yet, most importantly Care
// Mistakes and total Effort, both of which now genuinely gate evolution
// but were previously invisible in the UI entirely.
static void stats_canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GRect cr = content_rect(bounds);

  draw_background(ctx, bounds);
  graphics_context_set_text_color(ctx, GColorBlack);

  graphics_draw_text(ctx, "Stats", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(cr.origin.x, cr.origin.y + 2, cr.size.w, 20), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char age_buf[32];
  int age_days = s_pet.age_hours / 24;
  int age_hours_rem = s_pet.age_hours % 24;
  snprintf(age_buf, sizeof(age_buf), "Age: %dd %dh", age_days, age_hours_rem);
  graphics_draw_text(ctx, age_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 24, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char mistakes_buf[32];
  snprintf(mistakes_buf, sizeof(mistakes_buf), "Mistakes: %d/%d", s_pet.care_mistakes, CARE_MISTAKES_CAP);
  graphics_draw_text(ctx, mistakes_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 40, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char effort_buf[32];
  int effort_needed = effort_required_for_current_stage();
  int total_effort = (s_pet.training_count / TRAINS_PER_EFFORT_HEART) +
                      (s_pet.steps_in_stage / STEPS_PER_EFFORT_HEART);
  if (effort_needed >= 0) {
    snprintf(effort_buf, sizeof(effort_buf), "Effort: %d/%d", total_effort, effort_needed);
  } else {
    snprintf(effort_buf, sizeof(effort_buf), "Effort: MAX");
  }
  graphics_draw_text(ctx, effort_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 56, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char record_buf[32];
  snprintf(record_buf, sizeof(record_buf), "Record: %d-%d", s_pet.win_count, s_pet.loss_count);
  graphics_draw_text(ctx, record_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 72, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  static char vp_buf[32];
  snprintf(vp_buf, sizeof(vp_buf), "VP: %d/%d", s_pet.vital_points, MAX_VITAL_POINTS);
  graphics_draw_text(ctx, vp_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + 88, cr.size.w, 16), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "< Back", fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(cr.origin.x, cr.origin.y + cr.size.h - 16, cr.size.w, 16),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void stats_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_stats_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_stats_canvas_layer, stats_canvas_update_proc);
  layer_add_child(window_layer, s_stats_canvas_layer);
  // No click_config_provider needed - default BACK behavior (pop) is all this screen needs.
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

static void do_feed(void) {
  s_pet.hunger -= 35;
  if (s_pet.hunger < 0) s_pet.hunger = 0;
  save_pet();
}

static void do_train(void) {
  if (s_pet.energy < 25) return;
  s_pet.strength += 20;
  if (s_pet.strength > MAX_STAT) s_pet.strength = MAX_STAT;
  s_pet.energy -= 25;
  s_pet.training_count++;
  save_pet();
}

static void do_sleep(void) {
  s_pet.energy += 60;
  if (s_pet.energy > MAX_STAT) s_pet.energy = MAX_STAT;
  s_pet.hunger += 15;
  if (s_pet.hunger > MAX_STAT) s_pet.hunger = MAX_STAT;
  save_pet();
}

static void do_clean(void) {
  s_pet.has_mess = false;
  s_pet.hours_since_clean = 0;
  save_pet();
}

// UP always opens the Battle screen directly - no longer part of the
// Feed/Train/Sleep/Clean cycle at all, sidestepping any ambiguity with
// SELECT's long-press (manual sleep) entirely.
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_pet.alive) return;
  window_stack_push(s_battle_window, true);
}

// DOWN alone now cycles through the remaining 4 menu items on a short
// press. A long press opens the Stats screen instead.
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_pet.alive) return;
  s_selected_action = (s_selected_action + 1) % ACTION_COUNT;
  refresh();
}

static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_pet.alive) return;
  window_stack_push(s_stats_window, true);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_pet.alive) return;
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
  if (!s_pet.alive) return;
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
#endif

  s_idle_timer = app_timer_register(IDLE_BOB_MS, idle_bob_step, NULL);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  if (s_pet_bitmap) {
    gbitmap_destroy(s_pet_bitmap);
    s_pet_bitmap = NULL;
  }
#if defined(PBL_PLATFORM_EMERY)
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
}

static void init(void) {
  load_pet();
  apply_time_decay();

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
}

static void deinit(void) {
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