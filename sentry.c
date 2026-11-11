/* Sentry: a patient turret, in C on WebAssembly ABI 4.
 *
 * It holds its ground near the centre, banks battery and fires heavy shots
 * at slow targets once its own drift has settled. When it takes damage while
 * already hurt, it relocates: a quick dash in the other direction before it
 * settles again. The core tracks the robots it sees; Sentry locks onto the
 * nearest, leads its tracked motion and looks only where a muzzle flashed.
 * It calls the core through the clunkyard.* imports for its facts, angles
 * and lead, and needs no libc. */
#include "clunkyard.h"

typedef cy_vec3 V;
static V add(V a, V b) { return (V){a.x + b.x, a.y + b.y, a.z + b.z}; }
static V sub(V a, V b) { return (V){a.x - b.x, a.y - b.y, a.z - b.z}; }
static V mul(V a, double k) { return (V){a.x * k, a.y * k, a.z * k}; }
static double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double length(V a) { return __builtin_sqrt(dot(a, a)); }
static double planar(double x, double y) { return __builtin_sqrt(x * x + y * y); }
static double absolute(double x) { return x < 0 ? -x : x; }
static double clip(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static double clamp1(double x) { return clip(x, -1, 1); }
static double least(double a, double b) { return a < b ? a : b; }
static double most(double a, double b) { return a > b ? a : b; }

/* Angles in degrees, from the core's angle imports. */
static const V ORIGIN = {0, 0, 0};
static double diff(double target, double current) { double d = 0; cy_angles_angle_diff(target, current, &d); return d; }
static double norm(double degrees) { double d = 0; cy_angles_normalize(degrees, &d); return d; }
static double heading_of(V v) { double h = 0; cy_angles_bearing_to(&ORIGIN, &v, &h); return h; }
static double pitch_of(V v) { double p = 0; cy_angles_elevation_to(&ORIGIN, &v, &p); return p; }
/* The unit vector along a heading: its x is the cosine and its y the sine. */
static V unit_at(double degrees) { V u = {1, 0, 0}; cy_angles_direction(degrees, 0, &u); return u; }
#define DEG 57.29577951308232

/* ---- Sentry's personality. ---- */
typedef struct {
  double search_arc, lock_arc, lock_memory, range, speed, flip_chance, flip_min, flip_span;
  double hold_to_fire, max_shot, fire_floor, move_floor, governor, flight, slow_flight;
  double relocate, relocate_speed, relocate_below, anchor, aim_tol;
} Params;
static Params P = {50, 30, 12, 300, 25, .5, 1, 2, 30, 700, 30, 0, 40, 1.6, 3, 1.6, 85, .6, .12, 3};

static int three, classic, gravity_on, flat_ground, tick_rate = 20;
static double ground_level;
static double sudden_death, max_health, capacity, top_speed = 95;
static V arena, gravity;
static int32_t target, muzzle_tick = -1000;
static double muzzle_bearing, muzzle_elevation;
static double sweep = -1, flip = 1, next_flip, last_health = -1, relocate_until = -1;
static unsigned long long rng_state = 17;

static double rnd(void) {
  rng_state = (rng_state * 1103515245ULL + 12345ULL) % 2147483648ULL;
  return (double)rng_state / 2147483648.0;
}
static double raw(const char *path, double fallback) {
  uint32_t n = 0;
  while (path[n]) n++;
  double out = fallback;
  return cy_raw_number(path, n, &out) == CY_OK ? out : fallback;
}

static void start(void) {
  int32_t is_3d = 0, control = 0, ruleset = 0, ticks = 0;
  cy_match_is_3d(&is_3d);
  cy_match_control(&control);
  cy_match_ruleset(&ruleset);
  cy_match_tick_rate(&tick_rate);
  cy_match_max_ticks(&ticks);
  cy_arena_size(&arena);
  cy_arena_gravity(&gravity);
  cy_battery_capacity(&capacity);
  three = is_3d;
  classic = control == CY_CONTROL_DIRECT;
  gravity_on = gravity.z != 0;
  /* Flat ground has one height: read it once, not every turn. */
  flat_ground = ruleset == CY_RULESET_GRAVITY_FLAT;
  if (flat_ground) cy_arena_ground_height(arena.x / 2, arena.y / 2, &ground_level);
  if (!three) arena.z = 0;
  sudden_death = raw("hello.suddenDeath.startTick", ticks);
  max_health = raw("hello.robot.maxHealth", 150);
  top_speed = raw("hello.robot.maxSpeed", 95);
  /* Classic and zero-g reward a stationary gunner most, so Sentry holds its
   * spot less firmly there and relocates only when badly hurt; 3D classic
   * suits a turret best of all, so there it relocates only when nearly
   * finished. */
  if (classic) { P.anchor = 0; P.fire_floor = 45; P.relocate_below = three ? .2 : .3; }
  else if (ruleset == CY_RULESET_ZERO_G) P.anchor = 0;
  char id[64];
  uint32_t length_ = 0;
  if (cy_raw_text("hello.self.id", 13, id, sizeof id, &length_) != CY_OK) length_ = 0;
  unsigned long long seed = 0;
  for (uint32_t i = 0; i < length_ && i < sizeof id; i++) seed = (seed * 31 + (unsigned char)id[i]) % 2147483648ULL;
  rng_state = (seed * 7919 + 17) % 2147483648ULL;
}

/* The heaviest shot that lands within the flight budget and that the
 * battery pays for; slow targets get the long, heavy budget. */
static double choose_power(double distance, double speed, double charge) {
  double power = 1, cap = 0;
  cy_gun_power_for_flight_time(distance, speed < 30 ? P.slow_flight : P.flight, &power);
  cy_gun_max_power(&cap);
  if (charge < .3 * capacity) cap = least(cap, 1.5);
  power = clip(least(power, cap), .5, 3);
  return (double)(long long)(power * 10 + .5) / 10;
}

static void turn(void) {
  int32_t tick = 0;
  double heading = 0, pitch_now = 0, health = 0, charge = 0, seconds = 0;
  V pos = {0}, vel = {0};
  /* Every import costs fuel: read only what this turn's path uses. */
  cy_clock_tick(&tick);
  seconds = tick / (double)tick_rate;
  cy_me_position(&pos);
  cy_me_velocity(&vel);
  if (classic || sweep < 0) cy_me_heading(&heading);
  if (classic && three) cy_me_pitch(&pitch_now);
  cy_me_health(&health);
  cy_battery_charge(&charge);
  double charge_pct = 100 * charge / capacity;

  /* Lock: keep the target while the tracker remembers it, else take the
   * nearest in view. A turret ignores cues but for a muzzle flash. */
  cy_contact seen_now, cues[4];
  uint32_t count = 0;
  int visible_target = target && cy_sensors_contact(target, &seen_now) == CY_OK &&
                       (seen_now.source == CY_SOURCE_SCAN || seen_now.source == CY_SOURCE_PROXIMITY);
  int cue_status = cy_sensors_cues(cues, 4, &count);
  if (cue_status == CY_OK) {
    for (uint32_t i = 0; i < count && i < 4; i++)
      if (cues[i].source == CY_SOURCE_MUZZLE) { muzzle_tick = tick; muzzle_bearing = cues[i].bearing; muzzle_elevation = cues[i].elevation; break; }
  } else if (cue_status == CY_SHORT) {
    /* A crowded return leaves the short array untouched. The core's best
     * passive cue still lets a turret reacquire a recent muzzle flash,
     * without copying a large list or spending its fuel on every cue. */
    cy_cue heard;
    if (cy_tracker_cue(&heard) == CY_OK && heard.source == CY_SOURCE_MUZZLE && tick - heard.tick < 12) {
      V offset = sub(heard.position, pos);
      muzzle_tick = heard.tick;
      muzzle_bearing = heading_of(offset);
      muzzle_elevation = three ? pitch_of(offset) : 0;
    }
  }
  int32_t seen = 0;
  V there = {0}, tvel = {0};
  if (target && cy_tracker_last_seen(target, &seen) != CY_OK) target = 0;
  if (!target || !visible_target) {
    cy_contact nearest;
    if (cy_sensors_nearest(&nearest) == CY_OK) { target = nearest.slot; visible_target = 1; }
  }
  int tracked = target && cy_tracker_last_seen(target, &seen) == CY_OK && tick - seen < 60 &&
                cy_tracker_position(target, &there) == CY_OK && cy_tracker_velocity(target, &tvel) == CY_OK;
  if (tracked) there = add(there, mul(tvel, (tick - seen) / (double)tick_rate));

  /* Damage: once hurt, each hit triggers a dash in the other direction. */
  int hit = last_health >= 0 && health < last_health - .01;
  last_health = health;
  if (hit && seconds > relocate_until && health < P.relocate_below * max_health) { relocate_until = seconds + P.relocate; flip = -flip; }
  int relocating = seconds < relocate_until;

  V centre = mul(arena, .5), rel = sub(there, pos);
  double rel_heading = tracked ? heading_of(rel) : 0, rel_pitch = tracked && three ? pitch_of(rel) : 0;
  V desired = mul(sub(centre, pos), .45);
  if (tracked && tick - seen < P.lock_memory) {
    cy_scanner_heading(rel_heading);
    cy_scanner_pitch(rel_pitch);
    cy_scanner_arc(P.lock_arc);
  } else if (tick - muzzle_tick < 12) {
    cy_scanner_heading(muzzle_bearing);
    cy_scanner_pitch(three ? muzzle_elevation : 0);
    cy_scanner_arc(P.search_arc);
  } else {
    sweep = norm((sweep < 0 ? heading : sweep) + P.search_arc * .9);
    int turns = tick / (int)(360 / (P.search_arc * .9) + 1);
    cy_scanner_heading(sweep);
    cy_scanner_pitch(three ? (turns % 3 - 1) * 40.0 : 0);
    cy_scanner_arc(P.search_arc);
  }
  if (tracked) {
    double distance = length(rel), power = choose_power(distance, length(tvel), charge);
    /* Lead on the tracked motion; the core allows for a force-ruleset shot
     * inheriting our velocity and for the drop under gravity. */
    cy_solution aim = {rel_heading, rel_pitch, 0};
    int solved = cy_gun_lead_point(&there, &tvel, power, &aim) == CY_OK;
    cy_gun_aim_heading(aim.heading);
    cy_gun_aim_pitch(three ? aim.pitch : 0);
    V u = mul(rel, 1 / most(1, distance));
    if (seconds >= next_flip) {
      if (rnd() < P.flip_chance) flip = -flip;
      next_flip = seconds + P.flip_min + rnd() * P.flip_span;
    }
    V tangent = {-u.y, u.x, three ? .4 * unit_at(tick / 16.0 * DEG).y : 0};
    double tn = most(.01, length(tangent));
    double radial = clip((distance - P.range) * .65, -70, 70);
    double lateral = relocating ? P.relocate_speed : charge_pct > P.move_floor ? P.speed : P.speed * .25;
    desired = add(mul(tangent, lateral * flip / tn), mul(u, radial));
    if (three && !gravity_on) desired.z += 18 * unit_at(tick / 13.0 * DEG).x;
    if (P.anchor > 0 && !relocating) {
      V home = sub(centre, pos);
      if (!three) home.z = 0;
      desired = add(desired, mul(home, P.anchor));
    }
    cy_direction now = {0};
    cy_gun_aim_now(&now);
    int aligned = absolute(diff(aim.heading, now.heading)) < P.aim_tol && (!three || absolute(aim.pitch - now.pitch) < P.aim_tol);
    int hold = P.hold_to_fire > 0 && !relocating, steady = !hold || length(vel) < P.hold_to_fire;
    int32_t can_fire = 0;
    cy_gun_can_fire(power, .5, &can_fire);
    int affordable = charge_pct >= P.fire_floor;
    /* Never fire from memory alone: the target must be in this scan. */
    if (visible_target && solved && aligned && distance < P.max_shot && affordable && steady && can_fire) {
      cy_gun_fire();
      cy_gun_power(power);
    }
    /* Settle before the gun is ready, so the shot leaves from a still turret. */
    int32_t cooldown = 0;
    cy_gun_cooldown_ticks(&cooldown);
    if (hold && cooldown <= 1 && visible_target && distance < P.max_shot && affordable) desired = mul(desired, .1);
    cy_status_text("TURRET", 6);
    cy_status_value(distance);
  } else {
    cy_status_text("WATCH", 5);
    cy_status_value(charge);
  }
  if (P.governor > 0 && charge_pct < P.governor) {
    double sustainable = 0, speed = length(desired);
    cy_battery_sustainable_drive(&sustainable);
    sustainable *= top_speed;
    if (speed > sustainable) desired = mul(desired, sustainable / speed);
  }

  /* The storm: step out across its path, on the side away from its centre.
   * Walls and the safe zone below still win. */
  cy_storm storm;
  if (cy_nav_storm(&storm) == CY_OK) {
    double ax = pos.x - storm.center.x, ay = pos.y - storm.center.y, d = most(1, planar(ax, ay));
    double sv = planar(storm.velocity.x, storm.velocity.y), margin = storm.radius + 60 + sv;
    if (d < margin) {
      double px = sv > 1 ? -storm.velocity.y / sv : ax / d, py = sv > 1 ? storm.velocity.x / sv : ay / d;
      if (px * ax + py * ay < 0) { px = -px; py = -py; }
      double ex = px + .5 * ax / d, ey = py + .5 * ay / d, en = most(.01, planar(ex, ey)), push = 95 * least(1, (margin - d) / 60);
      desired.x = desired.x * .3 + ex / en * push;
      desired.y = desired.y * .3 + ey / en * push;
    }
  }

  /* Pilot: walls, the safe zone and hover height, then the controls. */
  double p_[3] = {pos.x, pos.y, pos.z}, v_[3] = {vel.x, vel.y, vel.z}, e_[3] = {arena.x, arena.y, arena.z}, d_[3] = {desired.x, desired.y, desired.z};
  for (int i = 0; i < (three ? 3 : 2); i++) {
    double margin = 60 + v_[i] * v_[i] / 180;
    if (p_[i] < margin) d_[i] = most(d_[i], (margin - p_[i]) * 1.5);
    if (p_[i] > e_[i] - margin) d_[i] = least(d_[i], (e_[i] - margin - p_[i]) * 1.5);
  }
  desired = (V){d_[0], d_[1], d_[2]};
  cy_zone zone;
  int has_zone = cy_nav_zone(&zone) == CY_OK;
  V offset = sub(pos, has_zone ? zone.center : centre);
  if (!three || (has_zone && zone.shape == CY_SHAPE_COLUMN)) offset.z = 0;
  double r = length(offset);
  double safe = has_zone ? most(40, zone.radius - 35) : tick < sudden_death - 100 ? 1e18 : 280;
  if (r > safe) desired = mul(offset, -least(95, 25 + r - safe) / r);
  if (gravity_on) {
    double ground = ground_level;
    if (!flat_ground) cy_arena_ground_height(pos.x, pos.y, &ground);
    desired.z = clip((ground + 115 - pos.z) * 1.2, -35, 35);
  }
  if (!three) desired.z = 0;

  if (classic) {
    double speed = length(desired), want = heading_of(desired);
    double turn_ = diff(want, heading), throttle = clip(speed / top_speed, 0, 1);
    /* Reverse rather than turn around when the goal is behind. */
    if (absolute(turn_) > 100) { turn_ = diff(want + 180, heading); throttle = -throttle; }
    cy_drive_throttle(throttle * most(0, unit_at(least(80, absolute(turn_))).x));
    cy_drive_turn(clamp1(turn_ / 15));
    if (three) {
      double pitch = pitch_of(desired) * (throttle < 0 ? -1 : 1);
      cy_drive_pitch_turn(clamp1((pitch - pitch_now) / 10));
    }
  } else {
    V acc = mul(sub(desired, vel), 2);
    acc.z -= gravity.z;
    cy_attitude attitude;
    cy_rotation spin = {0};
    cy_me_attitude(&attitude);
    cy_me_angular_velocity(&spin);
    /* Each local actuator is bounded on its own, so the hover budget survives. */
    cy_drive_thrust(clamp1(dot(acc, attitude.forward) / 100));
    cy_drive_strafe(clamp1(dot(acc, attitude.right) / 100));
    cy_drive_lift(three ? clamp1(dot(acc, attitude.up) / 100) : 0);
    cy_drive_yaw_torque(clamp1(-spin.yaw / 60));
    cy_drive_pitch_torque(three ? clamp1(-spin.pitch / 60) : 0);
    cy_drive_roll_torque(three ? clamp1(-spin.roll / 60) : 0);
    cy_drive_brake(0);
  }
}

CY_ROBOT(start, turn)
