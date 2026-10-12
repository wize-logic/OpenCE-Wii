/*
LEVEL_RUN.C

The scripted player's "objective" step (port/linux/src/test_input.c): a
campaign level played from where it stands to its end, as a person playing
it would, with controller 1, so that every script, cutscene and the move to
the next level are run (on the Wii as on the desktop; with
debug.fixed_ticks, the same on every machine, for the lockstep check).

It reads what the level's scripts wait on: every script thread waiting in a
sleep_until (hs_runtime_port_threads), and in its condition
- a trigger volume tested against the players: he walks there, and stands
  in it until the script has seen him (while its script waits on a device
  too, a lift's ride, until the device has moved);
- a vehicle's seat the players should sit in: he walks to it and holds the
  action button;
- encounters that must die (a living count, strength or fraction): he
  finds them and fights them;
- a device's position or a device group's value: he walks to a control of
  it and presses the action button, turned to it, or stands at the door;
- an object or flag the players should look at, or one of an encounter's
  actors (list_get (ai_actors ...)): he turns to it;
- the player's buttons (player_action_test_*, a10's tutorial): he presses
  them, and moves the right stick for the looking ones.
Of those, he goes first where the story is: what the scripts that moved on
most lately wait on (a level's mission scripts move on as he plays, those
that save the game at a place wait there from the start; a continuous
script counts as never moved), then by kind (a passenger's seat, what to
turn to, a volume, another BSP, a driver's seat, encounters, devices), then
the nearest. Until a script has moved on, he waits a minute (a10's
opening). A goal in another BSP is reached by the engine's BSP switch
nearest it, walking into its volume. Objects the script names with the
destroy step he shoots where he finds them (d40's manifolds' detectors).

He walks over the BSP's walkable surfaces (A* over the whole collision BSP,
from the middle of one edge to the next; the actors' own pathfinding where
that finds no way), turned to where he goes, and fights any enemy he can
see on the way: turned to it, the trigger pulled. Cutscenes play as they
are (not skipped); a screen over the game (a10's help) test_input.c
dismisses with A. Volumes whose script hurts the player once he is in them
(damage_object, unit_kill, game_lost after the wait) are left alone until
nothing else moves on.

What a person could not do, the aids, each counted and logged ("level run:
... aid:"):
- the cheats deathless_player, infinite_ammo and bottomless_clip, on for
  the whole level (the game's own cheat flags; the scripts' triggers do
  not read them);
- the player's view turned to where he goes or aims, as the stick would
  turn it, but at once;
- when he has not come nearer to where he goes for 30 seconds: stood on
  the floor there (a teleport), or for a volume with no floor in it (a
  lift's shaft) held in the air in it until it is seen; fallen out of the
  level: stood back where he last was in it; an encounter, or an object to
  destroy, he has fought for 90 seconds without killing one: killed; a seat
  or a control he cannot reach or use for 10 seconds once beside it: seated
  or used as the action button would; a screen that pauses the game, that
  A does not close in 5 seconds: closed.

Everything it does depends on the game's state alone, so that every machine
playing the same ticks does the same. Each step is logged ("level run:"),
and when nothing has moved on for two minutes, what every script waits on;
at a level's end, a summary line (ticks, checkpoints, aids).

level_run_update is called each frame of a game (network_test.c); the
controller is read by test_input.c (level_run_controller).
*/

#include "cseries.h"
#include "main/main.h"
#include "game/game.h"
#include "game/players.h"
#include "game/cheats.h"
#include "game/game_allegiance.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/bipeds.h"
#include "devices/devices.h"
#include "devices/device_controls.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "physics/collisions.h"
#include "math/real_math.h"
#include "hs/hs.h"
#include "hs/hs_scenario_definitions.h"
#include "ai/ai.h"
#include "ai/ai_script.h"
#include "ai/actors.h"
#include "ai/path.h"
#include "physics/collision_bsp.h"
#include "physics/collision_bsp_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "cutscene/cinematics.h"
#include "interface/ui_widget.h"
#include "tag_files/tag_groups.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's */
void platform_log(char const *format, ...);
/* game_state.c's */
long game_state_port_checkpoints_saved(void);

extern struct data_array *hs_syntax_data;
extern long const hs_function_table_count;

/* (as hs_runtime.c has them) */
#define level_run_syntax_get(index) ((struct hs_syntax_node *)datum_get(hs_syntax_data, (index)))
enum
{
	_level_run_node_primitive_bit = 0,
	_level_run_node_script_bit,
	_level_run_node_global_bit,
};

/* the controller's buttons (test_input.c's) */
enum
{
	_level_run_button_fire = 1 << 0,
	_level_run_button_action = 1 << 1,
	_level_run_button_jump = 1 << 2,
	_level_run_button_back = 1 << 3,
	_level_run_button_zoom = 1 << 4,
	_level_run_button_grenade = 1 << 5,
};

enum
{
	MAXIMUM_LEVEL_RUN_THREADS = 256,
	MAXIMUM_LEVEL_RUN_GOALS = 256,
	MAXIMUM_LEVEL_RUN_VISITS = 1024,
	MAXIMUM_LEVEL_RUN_BSP_SWITCHES = 64,
	MAXIMUM_LEVEL_RUN_SCRIPTS = 1024,
	/* (ticks between readings of the scripts) */
	LEVEL_RUN_READ_TICKS = 10,
	/* (ticks a goal is still taken for awaited after it was last read: a
	thread evaluating its condition is not seen waiting in it) */
	LEVEL_RUN_AWAITED_TICKS = 45,
	/* (ticks with nothing but what the scripts that have not moved wait on,
	before he goes there) */
	LEVEL_RUN_UNMOVED_TICKS = 60 * 30,
	/* (ticks between lines in the log while walking) */
	LEVEL_RUN_WALK_LOG_TICKS = 10 * 30,
	/* (ticks between paths found) */
	LEVEL_RUN_PATH_TICKS = 10,
	/* (ticks a volume is stood in, at most, for its script to see the player;
	when it waits on a device too, a lift's ride) */
	LEVEL_RUN_VOLUME_TICKS = 60,
	LEVEL_RUN_RIDE_TICKS = 30 * 30,
	/* (ticks with no step nearer before jumping and sidestepping, and before
	the aid) */
	LEVEL_RUN_WIGGLE_TICKS = 5 * 30,
	LEVEL_RUN_STUCK_TICKS = 30 * 30,
	/* (ticks beside a seat or a control before the aid) */
	LEVEL_RUN_BESIDE_TICKS = 10 * 30,
	/* (ticks fighting an encounter with none of it killed before the aid) */
	LEVEL_RUN_FIGHT_TICKS = 90 * 30,
	/* (ticks with nothing new before the scripts are logged and visited
	volumes count again) */
	LEVEL_RUN_STALL_TICKS = 120 * 30,
	/* (ticks before a BSP switch is crossed again) */
	LEVEL_RUN_SWITCH_AGAIN_TICKS = 40 * 30,
};

/* (world units) */
#define LEVEL_RUN_ENEMY_RANGE 12.0f
#define LEVEL_RUN_PROGRESS_DISTANCE 0.5f

enum level_run_goal_type
{
	_level_run_goal_volume,
	_level_run_goal_vehicle,
	_level_run_goal_ai,
	_level_run_goal_device,
	_level_run_goal_device_group,
	_level_run_goal_see_object,
	_level_run_goal_see_flag,
	_level_run_goal_action,
};

enum level_run_target_type
{
	_level_run_target_none,
	_level_run_target_volume,
	_level_run_target_bsp_switch,
	_level_run_target_vehicle,
	_level_run_target_ai,
	_level_run_target_control,
	_level_run_target_door,
	_level_run_target_see,
	_level_run_target_destroy,
};

/* (as players.c has it: the engine's own BSP switches, crossing a volume) */
struct level_run_bsp_switch
{
	short trigger_volume_index;
	short source_structure_bsp_index;
	short destination_structure_bsp_index;
	short cutscene_flag_index;
};

struct level_run_goal
{
	short type;
	/* (the volume, the object's name, the flag, the device group, or the
	player_action_test function) */
	short index;
	long ai_index;
	/* (a vehicle's seat: a string constant of the scripts) */
	char const *seat;
	long script_index;
	/* (the sleep_until that waits on it) */
	long sleep_until_index;
};

static struct
{
	/* the objective step runs (level_run_begin), and the level it began in */
	boolean active;
	boolean ended;
	char begun_in[256];
	char scenario_name[256];
	long last_tick;
	long begin_tick;
	long begin_checkpoints;
	long next_read_tick;
	long last_progress_tick;
	/* (frames the game has been paused by a widget: a help screen) */
	short paused_frames;
	/* the target: what, where, since when, and the nearest he has come */
	short target_type;
	short target_index;
	long target_ai;
	long target_object;
	long target_script;
	char const *target_seat;
	real_point3d target_point;
	long target_since;
	real target_best_distance;
	long target_best_tick;
	long beside_since;
	long in_volume_since;
	short fought_living;
	long fought_since;
	/* the path's next point */
	long path_tick;
	boolean path_valid;
	real_point3d path_point;
	/* the controller, for test_input.c */
	real stick_forward, stick_right, look_x, look_y;
	unsigned long buttons;
	long enemy_unit_index;
	/* the aids, and what he did, for the level's summary */
	long aid_teleports, aid_kills, aid_seats, aid_devices, aid_screens;
	long walked_volumes;
	long fights;
	/* (each volume a script waited on, once the player has been in it) */
	short visit_count;
	long visited_sleep_untils[MAXIMUM_LEVEL_RUN_VISITS];
	/* (each sleep_until whose object or flag he has turned to) */
	short tried_count;
	long tried_sleep_untils[MAXIMUM_LEVEL_RUN_VISITS];
	short visited_volumes[MAXIMUM_LEVEL_RUN_VISITS];
	unsigned long dangerous[BIT_VECTOR_SIZE_IN_LONGS(MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO)];
	boolean dangerous_allowed;
	/* (each volume: the BSPs it has no floor in) */
	unsigned long not_in_bsps[MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO];
	long switch_crossed[MAXIMUM_LEVEL_RUN_BSP_SWITCHES];
	/* (the last goals' fingerprint: a change is progress) */
	unsigned long goals_hash;
	short goal_count;
	struct level_run_goal goals[MAXIMUM_LEVEL_RUN_GOALS];
	/* each script's statement as last read, and when it last moved on (0
	never, since the level began): the story's scripts move on as he plays,
	and what they wait on comes first */
	long script_expressions[MAXIMUM_LEVEL_RUN_SCRIPTS];
	long script_moved[MAXIMUM_LEVEL_RUN_SCRIPTS];
	boolean scripts_read;
	long walk_logged_tick;
	/* (what the script says to destroy: destroy <name>, test_input.c) */
	short destroy_count;
	char destroy_names[16][32];
	/* (held in the air in a volume until when, there) */
	long hold_until;
	real_point3d hold_position;
	/* (out of the level since when, and where he last was in it) */
	long outside_since;
	boolean last_inside_valid;
	real_point3d last_inside;
	/* (since when only the scripts that have not moved wait on anything) */
	long unmoved_since;
	/* (the sleep_until being read) */
	long reading_sleep_until;
	/* (the target volume's sleep_until, and when its script last moved on) */
	long target_sleep_until;
	long target_moved;
	/* (when each volume, device name and device group was last read as
	awaited) */
	long volume_awaited[MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO];
	long device_awaited[MAXIMUM_OBJECT_NAMES_PER_SCENARIO];
	long group_awaited[MAXIMUM_DEVICE_GROUPS_PER_SCENARIO];
	/* the script threads, as last read */
	long thread_count;
	long thread_scripts[MAXIMUM_LEVEL_RUN_THREADS];
	long thread_sleep_untils[MAXIMUM_LEVEL_RUN_THREADS];
	long thread_expressions[MAXIMUM_LEVEL_RUN_THREADS];
	long thread_wakes[MAXIMUM_LEVEL_RUN_THREADS];
	/* the functions the conditions are read for */
	short function_players, function_volume_test_objects, function_volume_test_objects_all;
	short function_vehicle_test_seat_list, function_vehicle_test_seat, function_objects_can_see_object;
	short function_objects_can_see_flag, function_device_get_position, function_device_group_get;
	short function_ai_living_count, function_ai_strength, function_ai_living_fraction, function_ai_nonswarm_count;
	short function_not, function_damage_object, function_unit_kill, function_game_lost;
	short function_action_first, function_action_last;
	short function_list_get, function_ai_actors;
} level_run;

/* (a path's search: too large for the stack) */
static struct path_state level_run_path_state;

static short level_run_function(
	char const *name)
{
	short index;

	for (index = 0; index < (short)hs_function_table_count; index++)
	{
		struct hs_function_definition *function = hs_function_get(index);

		if (function && function->name && !strcmp(function->name, name))
			return index;
	}
	return NONE;
}

static void level_run_read_functions(
	void)
{
	short index;

	level_run.function_players = level_run_function("players");
	level_run.function_list_get = level_run_function("list_get");
	level_run.function_ai_actors = level_run_function("ai_actors");
	level_run.function_volume_test_objects = level_run_function("volume_test_objects");
	level_run.function_volume_test_objects_all = level_run_function("volume_test_objects_all");
	level_run.function_vehicle_test_seat_list = level_run_function("vehicle_test_seat_list");
	level_run.function_vehicle_test_seat = level_run_function("vehicle_test_seat");
	level_run.function_objects_can_see_object = level_run_function("objects_can_see_object");
	level_run.function_objects_can_see_flag = level_run_function("objects_can_see_flag");
	level_run.function_device_get_position = level_run_function("device_get_position");
	level_run.function_device_group_get = level_run_function("device_group_get");
	level_run.function_ai_living_count = level_run_function("ai_living_count");
	level_run.function_ai_strength = level_run_function("ai_strength");
	level_run.function_ai_living_fraction = level_run_function("ai_living_fraction");
	level_run.function_ai_nonswarm_count = level_run_function("ai_nonswarm_count");
	level_run.function_not = level_run_function("not");
	level_run.function_damage_object = level_run_function("damage_object");
	level_run.function_unit_kill = level_run_function("unit_kill");
	level_run.function_game_lost = level_run_function("game_lost");
	/* (player_action_test_* are listed together) */
	level_run.function_action_first = level_run.function_action_last = NONE;
	for (index = 0; index < (short)hs_function_table_count; index++)
	{
		struct hs_function_definition *function = hs_function_get(index);

		if (function && function->name && !strncmp(function->name, "player_action_test_", 19) &&
			strcmp(function->name, "player_action_test_reset"))
		{
			if (level_run.function_action_first == NONE)
				level_run.function_action_first = index;
			level_run.function_action_last = index;
		}
	}
}

/* ---------- reading the scripts */

/* a node's text in the scripts' source (a primitive's name) */
static char const *level_run_node_text(
	struct hs_syntax_node const *node)
{
	struct scenario *scenario = global_scenario_get();

	if (!scenario->hs_string_constants.address || node->source_offset < 0 ||
		node->source_offset >= scenario->hs_string_constants.size)
	{
		return "?";
	}
	return (char const *)scenario->hs_string_constants.address + node->source_offset;
}

static char const *level_run_script_name(
	long script_index)
{
	struct scenario *scenario = global_scenario_get();

	if (script_index < 0 || script_index >= scenario->hs_scripts.count)
		return "?";
	return TAG_BLOCK_GET_ELEMENT(&scenario->hs_scripts, script_index, struct hs_script)->name;
}

static boolean level_run_is_call(
	struct hs_syntax_node const *node)
{
	return !TEST_FLAG(node->flags, _level_run_node_primitive_bit) && !TEST_FLAG(node->flags, _level_run_node_script_bit);
}

/* a call's arguments: the node after its function's name */
static long level_run_first_argument(
	struct hs_syntax_node const *call)
{
	if (call->data == NONE)
		return NONE;
	return level_run_syntax_get(call->data)->next_node_index;
}

static long level_run_next(
	long expression_index)
{
	return expression_index == NONE ? NONE : level_run_syntax_get(expression_index)->next_node_index;
}

/* whether an expression is about the players: (players), (player0), (list_get (players) 0), ... */
static boolean level_run_mentions_players(
	long expression_index,
	short depth)
{
	struct hs_syntax_node *node;
	long argument_index;

	if (expression_index == NONE || depth > 8)
		return FALSE;
	node = level_run_syntax_get(expression_index);
	if (TEST_FLAG(node->flags, _level_run_node_script_bit))
		return !strncmp(level_run_script_name(node->script_index), "player", 6);
	if (TEST_FLAG(node->flags, _level_run_node_primitive_bit))
		return FALSE;
	if (node->function_index == level_run.function_players)
		return TRUE;
	for (argument_index = level_run_first_argument(node); argument_index != NONE;
		argument_index = level_run_next(argument_index))
	{
		if (level_run_mentions_players(argument_index, (short)(depth + 1)))
			return TRUE;
	}
	return FALSE;
}

/* a constant's value (an object's name, a volume, an encounter), else NONE */
static long level_run_constant(
	long expression_index,
	short type)
{
	struct hs_syntax_node *node;

	if (expression_index == NONE)
		return NONE;
	node = level_run_syntax_get(expression_index);
	/* (a constant's own type, or the parameter's it was compiled to: an
	object's name passed as an object keeps its name's index) */
	if (!TEST_FLAG(node->flags, _level_run_node_primitive_bit) || TEST_FLAG(node->flags, _level_run_node_global_bit) ||
		(node->type != type && node->constant_type != type &&
			!(type == _hs_type_object_name && HS_TYPE_IS_OBJECT_NAME(node->constant_type))))
	{
		return NONE;
	}
	/* (a short's in the low half of the cell, as the game reads it) */
	return type == _hs_type_ai ? node->data : (long)(short)node->data;
}

/* whether the statements after a sleep_until (its script's next few) hurt
the players: a volume whose script then damages or kills them, or loses
the game, is not one to go to */
static boolean level_run_hurts_players(
	long expression_index,
	short depth)
{
	struct hs_syntax_node *node;
	long argument_index;

	if (expression_index == NONE || depth > 10)
		return FALSE;
	node = level_run_syntax_get(expression_index);
	if (!level_run_is_call(node))
		return FALSE;
	if (node->function_index == level_run.function_game_lost)
		return TRUE;
	if ((node->function_index == level_run.function_damage_object || node->function_index == level_run.function_unit_kill) &&
		level_run_mentions_players(level_run_first_argument(node), 0))
	{
		return TRUE;
	}
	/* (damage_object's object is its second argument) */
	if (node->function_index == level_run.function_damage_object &&
		level_run_mentions_players(level_run_next(level_run_first_argument(node)), 0))
	{
		return TRUE;
	}
	for (argument_index = level_run_first_argument(node); argument_index != NONE;
		argument_index = level_run_next(argument_index))
	{
		if (level_run_hurts_players(argument_index, (short)(depth + 1)))
			return TRUE;
	}
	return FALSE;
}

static void level_run_add_goal(
	short type,
	short index,
	long ai_index,
	char const *seat,
	long script_index)
{
	short goal_index;
	struct level_run_goal *goal;

	for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
	{
		goal = &level_run.goals[goal_index];
		if (goal->type == type && goal->index == index && goal->ai_index == ai_index && goal->script_index == script_index)
			return;
	}
	if (level_run.goal_count >= MAXIMUM_LEVEL_RUN_GOALS)
		return;
	goal = &level_run.goals[level_run.goal_count++];
	goal->type = type;
	goal->index = index;
	goal->ai_index = ai_index;
	goal->seat = seat;
	goal->script_index = script_index;
	goal->sleep_until_index = level_run.reading_sleep_until;
}

/* whether he has stood in the volume for the sleep_until that waits on it */
static boolean level_run_visited(
	short volume_index,
	long sleep_until_index)
{
	short visit_index;

	for (visit_index = 0; visit_index < level_run.visit_count; visit_index++)
	{
		if (level_run.visited_volumes[visit_index] == volume_index &&
			level_run.visited_sleep_untils[visit_index] == sleep_until_index)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* the goals in a condition: `positive` is FALSE under a (not ...) */
static void level_run_read_condition(
	long expression_index,
	boolean positive,
	long script_index,
	boolean hurts,
	short depth)
{
	struct hs_syntax_node *node;
	long first;
	short function_index;

	if (expression_index == NONE || depth > 12)
		return;
	node = level_run_syntax_get(expression_index);
	if (!level_run_is_call(node))
		return;
	function_index = node->function_index;
	first = level_run_first_argument(node);
	if (function_index == level_run.function_not)
	{
		level_run_read_condition(first, !positive, script_index, hurts, (short)(depth + 1));
		return;
	}
	if (function_index == level_run.function_volume_test_objects ||
		function_index == level_run.function_volume_test_objects_all)
	{
		long volume_index = level_run_constant(first, _hs_type_trigger_volume);

		if (positive && volume_index != NONE && volume_index < MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO &&
			level_run_mentions_players(level_run_next(first), 0))
		{
			if (hurts && !level_run.dangerous_allowed)
				BIT_VECTOR_SET_FLAG(level_run.dangerous, volume_index, TRUE);
			else
				level_run_add_goal(_level_run_goal_volume, (short)volume_index, NONE, NULL, script_index);
		}
		return;
	}
	if (function_index == level_run.function_vehicle_test_seat_list ||
		function_index == level_run.function_vehicle_test_seat)
	{
		long name_index = level_run_constant(first, _hs_type_vehicle_name);
		long seat_index = level_run_next(first);

		if (positive && name_index != NONE && seat_index != NONE && level_run_mentions_players(level_run_next(seat_index), 0))
		{
			struct hs_syntax_node *seat = level_run_syntax_get(seat_index);

			if (TEST_FLAG(seat->flags, _level_run_node_primitive_bit) && seat->type == _hs_type_string)
				level_run_add_goal(_level_run_goal_vehicle, (short)name_index, NONE, level_run_node_text(seat), script_index);
		}
		return;
	}
	if (function_index == level_run.function_objects_can_see_object && positive && level_run_mentions_players(first, 0))
	{
		long object_expression = level_run_next(first);
		long name_index = level_run_constant(object_expression, _hs_type_object_name);

		if (name_index != NONE)
		{
			level_run_add_goal(_level_run_goal_see_object, (short)name_index, NONE, NULL, script_index);
		}
		else if (object_expression != NONE && level_run_is_call(level_run_syntax_get(object_expression)) &&
			level_run_syntax_get(object_expression)->function_index == level_run.function_list_get)
		{
			/* (list_get (ai_actors <encounter>) <n>): one of an encounter's actors */
			long list = level_run_first_argument(level_run_syntax_get(object_expression));

			if (list != NONE && level_run_is_call(level_run_syntax_get(list)) &&
				level_run_syntax_get(list)->function_index == level_run.function_ai_actors)
			{
				long ai_index = level_run_constant(level_run_first_argument(level_run_syntax_get(list)), _hs_type_ai);

				if (ai_index != NONE)
					level_run_add_goal(_level_run_goal_see_object, NONE, ai_index, NULL, script_index);
			}
		}
		return;
	}
	if (function_index == level_run.function_objects_can_see_flag && positive && level_run_mentions_players(first, 0))
	{
		long flag_index = level_run_constant(level_run_next(first), _hs_type_cutscene_flag);

		if (flag_index != NONE)
			level_run_add_goal(_level_run_goal_see_flag, (short)flag_index, NONE, NULL, script_index);
		return;
	}
	if (function_index == level_run.function_device_get_position)
	{
		long name_index = level_run_constant(first, _hs_type_device_name);

		if (name_index != NONE)
			level_run_add_goal(_level_run_goal_device, (short)name_index, NONE, NULL, script_index);
		return;
	}
	if (function_index == level_run.function_device_group_get)
	{
		long group_index = level_run_constant(first, _hs_type_device_group);

		if (group_index != NONE)
			level_run_add_goal(_level_run_goal_device_group, (short)group_index, NONE, NULL, script_index);
		return;
	}
	if (function_index == level_run.function_ai_living_count || function_index == level_run.function_ai_strength ||
		function_index == level_run.function_ai_living_fraction || function_index == level_run.function_ai_nonswarm_count)
	{
		long ai_index = level_run_constant(first, _hs_type_ai);

		if (ai_index != NONE)
			level_run_add_goal(_level_run_goal_ai, NONE, ai_index, NULL, script_index);
		return;
	}
	if (positive && level_run.function_action_first != NONE && function_index >= level_run.function_action_first &&
		function_index <= level_run.function_action_last)
	{
		level_run_add_goal(_level_run_goal_action, function_index, NONE, NULL, script_index);
		return;
	}
	/* (and, or, comparisons: their arguments) */
	{
		long argument_index;

		for (argument_index = first; argument_index != NONE; argument_index = level_run_next(argument_index))
			level_run_read_condition(argument_index, positive, script_index, hurts, (short)(depth + 1));
	}
}

static void level_run_read_goals(
	void)
{
	long thread_index;

	level_run.thread_count = hs_runtime_port_threads(level_run.thread_scripts, level_run.thread_sleep_untils,
		level_run.thread_expressions, level_run.thread_wakes, MAXIMUM_LEVEL_RUN_THREADS);
	level_run.goal_count = 0;
	for (thread_index = 0; thread_index < level_run.thread_count; thread_index++)
	{
		struct hs_syntax_node *call;
		long condition;
		boolean hurts = FALSE;
		long statement_index;
		short statements;

		/* (where each script is: moved on since the last reading) */
		{
			long script_index = level_run.thread_scripts[thread_index];

			if (script_index >= 0 && script_index < MAXIMUM_LEVEL_RUN_SCRIPTS)
			{
				if (level_run.scripts_read && level_run.script_expressions[script_index] != level_run.thread_expressions[thread_index])
					level_run.script_moved[script_index] = game_time_get();
				level_run.script_expressions[script_index] = level_run.thread_expressions[thread_index];
			}
		}
		if (level_run.thread_sleep_untils[thread_index] == NONE)
			continue;
		call = level_run_syntax_get(level_run.thread_sleep_untils[thread_index]);
		condition = level_run_first_argument(call);

		/* (the script's next few statements) */
		for (statement_index = call->next_node_index, statements = 0; statement_index != NONE && statements < 4;
			statement_index = level_run_next(statement_index), statements++)
		{
			hurts |= level_run_hurts_players(statement_index, 0);
		}
		level_run.reading_sleep_until = level_run.thread_sleep_untils[thread_index];
		level_run_read_condition(condition, TRUE, level_run.thread_scripts[thread_index], hurts, 0);
	}
	level_run.scripts_read = TRUE;
	{
		short goal_index;

		for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
		{
			struct level_run_goal *goal = &level_run.goals[goal_index];

			if (goal->type == _level_run_goal_volume && goal->index >= 0 && goal->index < MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO)
				level_run.volume_awaited[goal->index] = game_time_get();
			else if (goal->type == _level_run_goal_device && goal->index >= 0 && goal->index < MAXIMUM_OBJECT_NAMES_PER_SCENARIO)
				level_run.device_awaited[goal->index] = game_time_get();
			else if (goal->type == _level_run_goal_device_group && goal->index >= 0 &&
				goal->index < MAXIMUM_DEVICE_GROUPS_PER_SCENARIO)
			{
				level_run.group_awaited[goal->index] = game_time_get();
			}
		}
	}
}

/* when a script last moved on: the more lately, the sooner what it waits on */
static long level_run_script_moved(
	long script_index)
{
	struct scenario *scenario = global_scenario_get();

	if (script_index < 0 || script_index >= MAXIMUM_LEVEL_RUN_SCRIPTS || script_index >= scenario->hs_scripts.count)
		return 0;
	/* (a continuous script, which starts again each time it ends, saving the
	game at a place, is no part of the story however lately it moved) */
	if (TAG_BLOCK_GET_ELEMENT(&scenario->hs_scripts, script_index, struct hs_script)->script_type == _hs_script_continuous)
		return 0;
	return level_run.script_moved[script_index];
}

/* whether a volume, device or device group is awaited: read so lately */
static boolean level_run_goal_awaited(
	short type,
	short index)
{
	long tick = game_time_get();
	long read = NONE;

	if (index < 0)
		return FALSE;
	if (type == _level_run_goal_volume && index < MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO)
		read = level_run.volume_awaited[index];
	else if (type == _level_run_goal_device && index < MAXIMUM_OBJECT_NAMES_PER_SCENARIO)
		read = level_run.device_awaited[index];
	else if (type == _level_run_goal_device_group && index < MAXIMUM_DEVICE_GROUPS_PER_SCENARIO)
		read = level_run.group_awaited[index];
	return read != NONE && tick - read <= LEVEL_RUN_AWAITED_TICKS;
}

/* ---------- the level's places */

static long level_run_player_unit(
	void)
{
	long player_index = local_player_get_player_index(0);

	if (player_index == NONE)
		return NONE;
	return player_get(player_index)->unit_index;
}

static struct scenario_trigger_volume *level_run_volume_get(
	short volume_index)
{
	return TAG_BLOCK_GET_ELEMENT(&global_scenario_get()->trigger_volumes, volume_index, struct scenario_trigger_volume);
}

/* a point of a volume, by fractions of its extents */
static void level_run_volume_point(
	struct scenario_trigger_volume const *volume,
	real x,
	real y,
	real z,
	real_point3d *point)
{
	if (volume->type == _scenario_trigger_volume_type_axis_aligned)
	{
		point->x = volume->bounds.x0 + (volume->bounds.x1 - volume->bounds.x0) * x;
		point->y = volume->bounds.y0 + (volume->bounds.y1 - volume->bounds.y0) * y;
		point->z = volume->bounds.z0 + (volume->bounds.z1 - volume->bounds.z0) * z;
	}
	else
	{
		real_matrix4x3 matrix;
		real_point3d local;

		matrix4x3_from_point_and_vectors(&matrix, &volume->position, &volume->forward, &volume->up);
		local.x = volume->extents.i * x;
		local.y = volume->extents.j * y;
		local.z = volume->extents.k * z;
		matrix4x3_transform_point(&matrix, &local, point);
	}
}

static boolean level_run_point_in_world(
	real_point3d const *point)
{
	struct location location;

	scenario_location_from_point(&location, point);
	return location.cluster_index != NONE;
}

/* where an object's center is from its position (the volumes test the
center); an object outside the BSP may keep a center from before, so a
far one is not believed */
static void level_run_center_offset(
	long object_index,
	real_vector3d *offset)
{
	struct object_datum *object = object_get(object_index);

	offset->i = object->object.bounding_sphere_center.x - object->object.position.x;
	offset->j = object->object.bounding_sphere_center.y - object->object.position.y;
	offset->k = object->object.bounding_sphere_center.z - object->object.position.z;
	if (offset->i * offset->i + offset->j * offset->j + offset->k * offset->k > 4.0f * 4.0f)
	{
		offset->i = 0.0f;
		offset->j = 0.0f;
		offset->k = 0.0f;
	}
}

/* where the player (or what he rides) stands to be in a volume: on the
floor under its middle (or nearer its edges); FALSE when the volume has no
floor in this BSP */
static boolean level_run_volume_position(
	short volume_index,
	long object_index,
	real_point3d *position)
{
	struct scenario_trigger_volume *volume = level_run_volume_get(volume_index);
	real_vector3d center_offset;
	real_point3d middle, center;
	real_vector3d down;
	struct collision_result collision;
	/* (its middle first, then points further out) */
	static real const fractions[] = { 0.5f, 0.3f, 0.7f, 0.15f, 0.85f };
	short point_index;

	level_run_center_offset(object_index, &center_offset);
	for (point_index = 0; point_index < (short)(NUMBEROF(fractions) * NUMBEROF(fractions) * NUMBEROF(fractions)); point_index++)
	{
		short count = (short)NUMBEROF(fractions);

		level_run_volume_point(volume, fractions[point_index / count % count], fractions[point_index / (count * count)],
			fractions[point_index % count], &middle);
		if (!level_run_point_in_world(&middle))
			continue;
		/* (the floor below, up to 20 world units down) */
		down.i = 0.0f;
		down.j = 0.0f;
		down.k = -20.0f;
		if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			&middle, &down, NONE, &collision))
		{
			position->x = collision.point.x;
			position->y = collision.point.y;
			position->z = collision.point.z + 0.05f + MAX(0.0f, -center_offset.k);
			center.x = position->x + center_offset.i;
			center.y = position->y + center_offset.j;
			center.z = position->z + center_offset.k;
			if (scenario_trigger_volume_test_point(volume_index, &center) && level_run_point_in_world(position))
				return TRUE;
		}
	}
	/* (no floor in it: its middle, if that is in this BSP; he goes as near
	as he can) */
	level_run_volume_point(volume, 0.5f, 0.5f, 0.5f, &middle);
	if (level_run_point_in_world(&middle))
	{
		*position = middle;
		return TRUE;
	}
	return FALSE;
}

static void level_run_place(
	long object_index,
	real_point3d const *position)
{
	struct object_datum *object = object_get(object_index);

	object_set_position(object_index, position, NULL, NULL);
	object->object.translational_velocity.i = 0.0f;
	object->object.translational_velocity.j = 0.0f;
	object->object.translational_velocity.k = 0.0f;
}

static char const *level_run_volume_name(
	short volume_index)
{
	return level_run_volume_get(volume_index)->name;
}

static char const *level_run_object_name(
	short name_index)
{
	struct scenario *scenario = global_scenario_get();

	if (name_index < 0 || name_index >= scenario->object_names.count)
		return "?";
	return TAG_BLOCK_GET_ELEMENT(&scenario->object_names, name_index, struct scenario_object_name)->name;
}

static boolean level_run_in_volume(
	short volume_index,
	long object_index)
{
	return scenario_trigger_volume_test_point(volume_index, &object_get(object_index)->object.bounding_sphere_center);
}

/* ---------- walking */

/* the floor's surface under a point, for the pathfinding */
static long level_run_floor(
	real_point3d const *point,
	real_point3d *floor)
{
	struct collision_result collision;
	real_point3d above;
	real_vector3d down;

	above = *point;
	above.z += 0.5f;
	down.i = 0.0f;
	down.j = 0.0f;
	down.k = -4.0f;
	if (!collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
		&above, &down, NONE, &collision))
	{
		return NONE;
	}
	*floor = collision.point;
	return collision.surface_index;
}

/* ---------- the way through the level

The actors' pathfinding searches at most 1024 surfaces and stops where it
gets no nearer, which a level's long ways and spirals (b30's shaft) defeat.
The way to a target is found over the whole BSP instead: A* over its
collision surfaces that the BSP marks walkable, from edge to edge (as
path.c's traverse goes, by the middle of the edge they share), the
distance to the target its estimate. He then walks from the middle of one
edge to the next; found again every few seconds, or when he strays. */

enum
{
	_level_run_pathfinding_walkable_bit = 6,
	MAXIMUM_LEVEL_RUN_ROUTE = 4096,
	/* (ticks between ways found) */
	LEVEL_RUN_ROUTE_TICKS = 5 * 30,
};

struct level_run_route_heap_entry
{
	real cost;
	long surface_index;
};

static struct
{
	/* the BSP the arrays were made for */
	long surface_count;
	real *costs;
	long *parents;
	real_point3d *entries;
	byte *closed;
	struct level_run_route_heap_entry *heap;
	long heap_capacity;
	/* the way: its points, the next one, and when and to where it was found */
	short point_count;
	short point_index;
	real_point3d points[MAXIMUM_LEVEL_RUN_ROUTE];
	long found_tick;
	real_point3d found_target;
	short found_bsp;
} level_run_route;

static void level_run_route_free(
	void)
{
	/* (the game's free takes no NULL) */
	if (level_run_route.costs)
		free(level_run_route.costs);
	if (level_run_route.parents)
		free(level_run_route.parents);
	if (level_run_route.entries)
		free(level_run_route.entries);
	if (level_run_route.closed)
		free(level_run_route.closed);
	if (level_run_route.heap)
		free(level_run_route.heap);
	level_run_route.costs = NULL;
	level_run_route.parents = NULL;
	level_run_route.entries = NULL;
	level_run_route.closed = NULL;
	level_run_route.heap = NULL;
	level_run_route.surface_count = 0;
	level_run_route.point_count = 0;
}

static void level_run_route_push(
	long *heap_count,
	real cost,
	long surface_index)
{
	struct level_run_route_heap_entry *heap = level_run_route.heap;
	long index = (*heap_count)++;

	while (index > 0 && heap[(index - 1) / 2].cost > cost)
	{
		heap[index] = heap[(index - 1) / 2];
		index = (index - 1) / 2;
	}
	heap[index].cost = cost;
	heap[index].surface_index = surface_index;
}

static long level_run_route_pop(
	long *heap_count)
{
	struct level_run_route_heap_entry *heap = level_run_route.heap;
	struct level_run_route_heap_entry last;
	long surface_index = heap[0].surface_index;
	long index = 0;

	last = heap[--(*heap_count)];
	while (TRUE)
	{
		long child = index * 2 + 1;

		if (child >= *heap_count)
			break;
		if (child + 1 < *heap_count && heap[child + 1].cost < heap[child].cost)
			child++;
		if (heap[child].cost >= last.cost)
			break;
		heap[index] = heap[child];
		index = child;
	}
	if (*heap_count > 0)
		heap[index] = last;
	return surface_index;
}

/* the way over the BSP's walkable surfaces from one surface to another:
FALSE when there is none */
static boolean level_run_route_find(
	real_point3d const *start,
	long start_surface,
	real_point3d const *goal,
	long goal_surface)
{
	struct structure_bsp *structure = global_structure_bsp_get();
	struct collision_bsp const *bsp;
	byte const *pathfinding_surfaces;
	long heap_count = 0;
	long surface_index;
	boolean found = FALSE;

	level_run_route.point_count = 0;
	level_run_route.point_index = 0;
	if (!structure || structure->collision_bsp.count <= 0)
		return FALSE;
	bsp = TAG_BLOCK_GET_ELEMENT(&structure->collision_bsp, 0, struct collision_bsp);
	pathfinding_surfaces = structure->pathfinding_surfaces.address;
	if (!pathfinding_surfaces || structure->pathfinding_surfaces.count < bsp->surfaces.count ||
		!collision_bsp_valid_surface_index(bsp, start_surface) || !collision_bsp_valid_surface_index(bsp, goal_surface))
	{
		return FALSE;
	}
	if (level_run_route.surface_count != bsp->surfaces.count)
	{
		level_run_route_free();
		level_run_route.surface_count = bsp->surfaces.count;
		level_run_route.heap_capacity = bsp->surfaces.count * 4;
		level_run_route.costs = malloc(sizeof(real) * bsp->surfaces.count);
		level_run_route.parents = malloc(sizeof(long) * bsp->surfaces.count);
		level_run_route.entries = malloc(sizeof(real_point3d) * bsp->surfaces.count);
		level_run_route.closed = malloc(bsp->surfaces.count);
		level_run_route.heap = malloc(sizeof(struct level_run_route_heap_entry) * level_run_route.heap_capacity);
		if (!level_run_route.costs || !level_run_route.parents || !level_run_route.entries || !level_run_route.closed ||
			!level_run_route.heap)
		{
			platform_log("level run: no memory for the way through a BSP of %ld surfaces", (long)bsp->surfaces.count);
			level_run_route_free();
			return FALSE;
		}
	}
	for (surface_index = 0; surface_index < bsp->surfaces.count; surface_index++)
	{
		level_run_route.costs[surface_index] = REAL_MAX;
		level_run_route.closed[surface_index] = FALSE;
	}
	level_run_route.costs[start_surface] = 0.0f;
	level_run_route.parents[start_surface] = NONE;
	level_run_route.entries[start_surface] = *start;
	level_run_route_push(&heap_count, distance3d(start, goal), start_surface);
	while (heap_count > 0)
	{
		long edge_index, first_edge_index;
		short edge_count = 0;
		struct collision_surface const *surface;

		surface_index = level_run_route_pop(&heap_count);
		if (level_run_route.closed[surface_index])
			continue;
		level_run_route.closed[surface_index] = TRUE;
		if (surface_index == goal_surface)
		{
			found = TRUE;
			break;
		}
		surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct collision_surface);
		edge_index = first_edge_index = surface->first_edge_index;
		do
		{
			struct collision_edge const *edge;
			boolean right_surface;
			long adjacent;
			real_point3d entry;
			real cost;

			if (!collision_surface_edge_ring_continues(bsp, edge_index, edge_count))
				break;
			edge = TAG_BLOCK_GET_ELEMENT(&bsp->edges, edge_index, struct collision_edge);
			right_surface = surface_index == edge->surface_indices[1];
			adjacent = edge->surface_indices[!right_surface];
			edge_index = edge->edge_indices[right_surface];
			edge_count++;
			if (adjacent == NONE || !collision_bsp_valid_surface_index(bsp, adjacent) || level_run_route.closed[adjacent] ||
				!TEST_FLAG(pathfinding_surfaces[adjacent], _level_run_pathfinding_walkable_bit) ||
				!VALID_INDEX(edge->vertex_indices[0], bsp->vertices.count) ||
				!VALID_INDEX(edge->vertex_indices[1], bsp->vertices.count))
			{
				continue;
			}
			{
				real_point3d const *a = &TAG_BLOCK_GET_ELEMENT(&bsp->vertices, edge->vertex_indices[0], struct collision_vertex)->point;
				real_point3d const *b = &TAG_BLOCK_GET_ELEMENT(&bsp->vertices, edge->vertex_indices[1], struct collision_vertex)->point;

				entry.x = 0.5f * (a->x + b->x);
				entry.y = 0.5f * (a->y + b->y);
				entry.z = 0.5f * (a->z + b->z);
			}
			cost = level_run_route.costs[surface_index] + distance3d(&level_run_route.entries[surface_index], &entry);
			if (cost < level_run_route.costs[adjacent] && heap_count < level_run_route.heap_capacity)
			{
				level_run_route.costs[adjacent] = cost;
				level_run_route.parents[adjacent] = surface_index;
				level_run_route.entries[adjacent] = entry;
				level_run_route_push(&heap_count, cost + distance3d(&entry, goal), adjacent);
			}
		}
		while (edge_index != first_edge_index && edge_count < MAXIMUM_PATH_EDGES_PER_COLLISION_SURFACE);
	}
	if (!found)
		return FALSE;
	/* (the points from the goal back, then turned about: those nearest the
	start kept, if more) */
	{
		short count = 0, index;

		level_run_route.points[count++] = *goal;
		for (surface_index = goal_surface; surface_index != start_surface && surface_index != NONE;
			surface_index = level_run_route.parents[surface_index])
		{
			if (count == MAXIMUM_LEVEL_RUN_ROUTE)
			{
				csmemmove(&level_run_route.points[0], &level_run_route.points[1], sizeof(real_point3d) * (count - 1));
				count--;
			}
			level_run_route.points[count++] = level_run_route.entries[surface_index];
		}
		for (index = 0; index < count / 2; index++)
		{
			real_point3d point = level_run_route.points[index];

			level_run_route.points[index] = level_run_route.points[count - 1 - index];
			level_run_route.points[count - 1 - index] = point;
		}
		level_run_route.point_count = count;
	}
	return TRUE;
}

/* the next point of the way to the target, by the actors' pathfinding:
FALSE where it finds none (the way is then straight at the target) */
static boolean level_run_find_path(
	long unit_index,
	long mover_index,
	real_point3d const *target,
	real_point3d *next)
{
	struct path_input input;
	struct path_result path;
	real_point3d start, goal;
	long start_surface, goal_surface;
	boolean riding = mover_index != unit_index;
	short step_index;

	if (!riding && TEST_FLAG(_object_mask_biped, object_get(unit_index)->object.type))
		start_surface = biped_find_pathfinding_surface_index(unit_index, &start);
	else
		start_surface = level_run_floor(&object_get(mover_index)->object.position, &start);
	goal_surface = level_run_floor(target, &goal);
	if (start_surface == NONE || goal_surface == NONE)
		return FALSE;
	path_input_new(&input, riding ? 1.0f : 0.2f, FALSE, mover_index);
	path_input_set_start(&input, &start, start_surface);
	path_state_new(&input, &level_run_path_state, NULL);
	/* (any distance accepted: a long way's search ends nearest the target,
	and he walks there) */
	path_state_destination(&level_run_path_state, &goal, goal_surface, 10000.0f);
	path_state_find(&level_run_path_state);
	if (!path_state_build_path(&level_run_path_state, &path) || path.step_count <= 0)
		return FALSE;
	for (step_index = 0; step_index < path.step_count; step_index++)
	{
		real dx = path.steps[step_index].point.x - start.x;
		real dy = path.steps[step_index].point.y - start.y;

		if (dx * dx + dy * dy > 0.3f * 0.3f || step_index == path.step_count - 1)
		{
			*next = path.steps[step_index].point;
			return TRUE;
		}
	}
	return FALSE;
}

/* turned to a point, as the stick would turn him */
static void level_run_face(
	long unit_index,
	real_point3d const *target,
	boolean level)
{
	real_point3d eye;
	real_vector3d direction;
	real flat;

	unit_get_head_position(unit_index, &eye);
	direction.i = target->x - eye.x;
	direction.j = target->y - eye.y;
	direction.k = level ? 0.0f : target->z - eye.z;
	/* (not straight up or down, which no facing is: at most 80 degrees, and
	none for a point right above or below him) */
	flat = square_root(direction.i * direction.i + direction.j * direction.j);
	if (flat < 0.01f)
		return;
	direction.k = PIN(direction.k, -5.6f * flat, 5.6f * flat);
	if (normalize3d(&direction) > 0.0f)
		player_control_set_facing(0, &direction);
}

/* the left stick toward a point, in the frame he faces */
static void level_run_move_toward(
	long unit_index,
	long mover_index,
	real_point3d const *point,
	real speed)
{
	real_vector3d facing;
	real dx, dy, length;

	player_control_get_facing_direction(0, &facing);
	dx = point->x - object_get(mover_index)->object.position.x;
	dy = point->y - object_get(mover_index)->object.position.y;
	length = square_root(dx * dx + dy * dy);
	if (length < 0.0001f)
		return;
	dx /= length;
	dy /= length;
	{
		real flat = square_root(facing.i * facing.i + facing.j * facing.j);

		if (flat < 0.0001f)
			return;
		/* (forward along his facing, right a quarter clockwise from it) */
		level_run.stick_forward = speed * (dx * facing.i + dy * facing.j) / flat;
		level_run.stick_right = speed * (dx * facing.j - dy * facing.i) / flat;
	}
}

/* ---------- fighting */

/* the nearest enemy he can see within range (of an encounter, if given) */
static long level_run_enemy(
	long unit_index,
	long ai_index,
	real range,
	boolean must_see)
{
	struct data_iterator iterator;
	struct actor_datum *actor;
	short team = object_get(unit_index)->object.owner_team_index;
	real_point3d eye;
	long best_index = NONE;
	real best_distance = range * range;

	unit_get_head_position(unit_index, &eye);
	data_iterator_new(&iterator, actor_data);
	while ((actor = (struct actor_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *object;
		real distance;

		if (actor->meta.swarm || actor->meta.unit_index == NONE || !game_team_is_enemy(team, actor->meta.team_index))
			continue;
		if (ai_index != NONE)
		{
			struct ai_script_actor_reference_iterator members;
			struct actor_datum *member;
			boolean found = FALSE;

			ai_index_actor_iterator_new(ai_index, &members);
			while (!found && (member = ai_index_actor_iterator_next(&members)) != NULL)
				found = member == actor;
			if (!found)
				continue;
		}
		object = object_get(actor->meta.unit_index);
		if (TEST_FLAG(object->object.damage_flags, _object_dead_bit))
			continue;
		distance = distance_squared3d(&eye, &object->object.bounding_sphere_center);
		if (distance >= best_distance)
			continue;
		if (must_see)
		{
			struct collision_result collision;
			real_vector3d line;

			vector_from_points3d(&eye, &object->object.bounding_sphere_center, &line);
			if (collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
				&eye, &line, NONE, &collision))
			{
				continue;
			}
		}
		best_index = actor->meta.unit_index;
		best_distance = distance;
	}
	return best_index;
}

/* turned to an enemy, the trigger pulled (and let go, for a weapon that
fires once to a pull) */
static void level_run_fight(
	long unit_index,
	long enemy_unit_index,
	long tick)
{
	level_run_face(unit_index, &object_get(enemy_unit_index)->object.bounding_sphere_center, FALSE);
	if ((tick / 3) % 2 == 0)
		level_run.buttons |= _level_run_button_fire;
	if (level_run.enemy_unit_index != enemy_unit_index)
	{
		level_run.enemy_unit_index = enemy_unit_index;
		level_run.fights++;
	}
}

/* ---------- the BSP switches */

/* the engine's BSP switch from this BSP nearest a place not in it (a
volume's, NONE for a device's), not one crossed lately: its volume */
static short level_run_bsp_switch_toward(
	short const *volumes,
	real_point3d const *points,
	short volume_count)
{
	struct scenario *scenario = global_scenario_get();
	long tick = game_time_get();
	short switch_index;
	short best_index = NONE;
	real best_distance = 0.0f;

	for (switch_index = 0; switch_index < MIN(scenario->bsp_switch_trigger_volumes.count, MAXIMUM_LEVEL_RUN_BSP_SWITCHES);
		switch_index++)
	{
		struct level_run_bsp_switch *bsp_switch = TAG_BLOCK_GET_ELEMENT(&scenario->bsp_switch_trigger_volumes,
			switch_index, struct level_run_bsp_switch);
		short volume_index;
		real_point3d middle;

		if (bsp_switch->source_structure_bsp_index != global_structure_bsp_index ||
			bsp_switch->trigger_volume_index < 0 || bsp_switch->trigger_volume_index >= scenario->trigger_volumes.count ||
			(level_run.switch_crossed[switch_index] != NONE &&
				tick - level_run.switch_crossed[switch_index] < LEVEL_RUN_SWITCH_AGAIN_TICKS))
		{
			continue;
		}
		level_run_volume_point(level_run_volume_get(bsp_switch->trigger_volume_index), 0.5f, 0.5f, 0.5f, &middle);
		for (volume_index = 0; volume_index < volume_count; volume_index++)
		{
			real distance = distance_squared3d(&points[volume_index], &middle);

			/* (not to a BSP the volume is known not to be in) */
			if (volumes[volume_index] != NONE &&
				(level_run.not_in_bsps[volumes[volume_index]] & (1UL << (bsp_switch->destination_structure_bsp_index & 31))))
			{
				continue;
			}
			if (best_index == NONE || distance < best_distance)
			{
				best_index = switch_index;
				best_distance = distance;
			}
		}
	}
	return best_index;
}

/* ---------- the log */

static unsigned long level_run_goals_hash(
	void)
{
	unsigned long hash = 2166136261UL;
	short goal_index;
	struct scenario *scenario = global_scenario_get();
	long thread_index;

	for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
	{
		struct level_run_goal *goal = &level_run.goals[goal_index];

		hash = (hash ^ (unsigned long)goal->type) * 16777619UL;
		hash = (hash ^ (unsigned long)(unsigned short)goal->index) * 16777619UL;
		hash = (hash ^ (unsigned long)goal->ai_index) * 16777619UL;
	}
	/* (and where every script but the continuous ones is: a level's long
	scripted scenes move on without waiting for the player) */
	for (thread_index = 0; thread_index < level_run.thread_count; thread_index++)
	{
		long script_index = level_run.thread_scripts[thread_index];

		if (script_index >= 0 && script_index < scenario->hs_scripts.count &&
			TAG_BLOCK_GET_ELEMENT(&scenario->hs_scripts, script_index, struct hs_script)->script_type == _hs_script_continuous)
		{
			continue;
		}
		hash = (hash ^ (unsigned long)script_index) * 16777619UL;
		hash = (hash ^ (unsigned long)level_run.thread_expressions[thread_index]) * 16777619UL;
	}
	return hash;
}

/* an expression written out as script source, cut short */
static void level_run_print(
	long expression_index,
	char *buffer,
	int size,
	int *length,
	short depth)
{
	struct hs_syntax_node *node;
	char text[64];

	if (*length >= size - 1)
		return;
	if (expression_index == NONE || depth > 30)
	{
		text[0] = 0;
	}
	else
	{
		node = level_run_syntax_get(expression_index);
		if (TEST_FLAG(node->flags, _level_run_node_script_bit))
		{
			snprintf(text, sizeof(text), "(%s)", level_run_script_name(node->script_index));
		}
		else if (TEST_FLAG(node->flags, _level_run_node_primitive_bit))
		{
			if (!TEST_FLAG(node->flags, _level_run_node_global_bit) && node->type == _hs_type_real)
				snprintf(text, sizeof(text), "%g", (double)node->real_value);
			else if (!TEST_FLAG(node->flags, _level_run_node_global_bit) &&
				(node->type == _hs_type_short_integer || node->type == _hs_type_boolean))
			{
				snprintf(text, sizeof(text), "%d", (int)(short)node->data);
			}
			else if (!TEST_FLAG(node->flags, _level_run_node_global_bit) && node->type == _hs_type_long_integer)
				snprintf(text, sizeof(text), "%ld", (long)node->data);
			else
				snprintf(text, sizeof(text), "%.48s", level_run_node_text(node));
		}
		else
		{
			long argument_index;
			int written;

			written = snprintf(buffer + *length, (size_t)(size - *length), "(%s",
				(word)node->function_index < (word)hs_function_table_count ? hs_function_get(node->function_index)->name : "?");
			if (written > 0)
				*length = MIN(*length + written, size - 1);
			for (argument_index = level_run_first_argument(node); argument_index != NONE;
				argument_index = level_run_next(argument_index))
			{
				if (*length < size - 1)
					buffer[(*length)++] = ' ';
				level_run_print(argument_index, buffer, size, length, (short)(depth + 1));
			}
			if (*length < size - 1)
				buffer[(*length)++] = ')';
			buffer[*length] = 0;
			return;
		}
	}
	{
		int written = snprintf(buffer + *length, (size_t)(size - *length), "%s", text);

		if (written > 0)
			*length = MIN(*length + written, size - 1);
	}
}

/* every script thread that is not dormant: what it waits on */
static void level_run_log_threads(
	void)
{
	long thread_index;

	for (thread_index = 0; thread_index < level_run.thread_count; thread_index++)
	{
		char line[400];
		int length = 0;
		long wake = level_run.thread_wakes[thread_index];

		/* (-2: dormant) */
		if (wake == NONE - 1)
			continue;
		line[0] = 0;
		level_run_print(level_run.thread_sleep_untils[thread_index] != NONE ?
			level_run.thread_sleep_untils[thread_index] : level_run.thread_expressions[thread_index],
			line, (int)sizeof(line), &length, 0);
		platform_log("level run:   thread %s, wakes %ld: %s", level_run_script_name(level_run.thread_scripts[thread_index]),
			wake > 0 ? wake - (long)game_time_get() : wake, line);
	}
}

static void level_run_goal_name(
	struct level_run_goal const *goal,
	char *name,
	int size)
{
	switch (goal->type)
	{
	case _level_run_goal_volume:
	{
		long unit_index = level_run_player_unit();
		real_point3d position;
		boolean here = unit_index != NONE &&
			level_run_volume_position(goal->index, object_get_ultimate_parent(unit_index), &position);

		snprintf(name, (size_t)size, "volume %s%s%s, moved %ld", level_run_volume_name(goal->index),
			level_run_visited(goal->index, goal->sleep_until_index) ? " (visited)" : "",
			here ? "" : " (not in this BSP)", level_run_script_moved(goal->script_index));
		break;
	}
	case _level_run_goal_vehicle:
		snprintf(name, (size_t)size, "vehicle %s seat %s", level_run_object_name(goal->index), goal->seat);
		break;
	case _level_run_goal_ai:
		ai_index_to_string(goal->ai_index, global_scenario_get(), name, size);
		break;
	case _level_run_goal_device:
		snprintf(name, (size_t)size, "device %s", level_run_object_name(goal->index));
		break;
	case _level_run_goal_device_group:
		snprintf(name, (size_t)size, "device group %d", (int)goal->index);
		break;
	case _level_run_goal_see_object:
		snprintf(name, (size_t)size, "see %s", level_run_object_name(goal->index));
		break;
	case _level_run_goal_see_flag:
		snprintf(name, (size_t)size, "see flag %d", (int)goal->index);
		break;
	default:
		snprintf(name, (size_t)size, "%s", hs_function_get(goal->index)->name);
		break;
	}
}

static void level_run_log_goals(
	char const *why)
{
	short goal_index;

	platform_log("level run: tick %ld: %s; %d goals", (long)game_time_get(), why, (int)level_run.goal_count);
	for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
	{
		char name[256];

		level_run_goal_name(&level_run.goals[goal_index], name, (int)sizeof(name));
		platform_log("level run:   %s (%s)", name, level_run_script_name(level_run.goals[goal_index].script_index));
	}
	/* (what the script said to destroy: where it is) */
	for (goal_index = 0; goal_index < level_run.destroy_count; goal_index++)
	{
		struct scenario *scenario = global_scenario_get();
		short name_index;
		long object_index = NONE;

		for (name_index = 0; name_index < scenario->object_names.count && object_index == NONE; name_index++)
		{
			if (!csstrcmp(TAG_BLOCK_GET_ELEMENT(&scenario->object_names, name_index, struct scenario_object_name)->name,
				level_run.destroy_names[goal_index]))
			{
				object_index = object_index_from_name_index(name_index);
			}
		}
		if (object_index == NONE)
		{
			platform_log("level run:   destroy %s: none", level_run.destroy_names[goal_index]);
		}
		else
		{
			struct object_datum *object = object_get(object_index);

			platform_log("level run:   destroy %s: type %d, vitality %.2f%s%s%s", level_run.destroy_names[goal_index],
				(int)object->object.type, object->object.body_vitality,
				TEST_FLAG(object->object.damage_flags, _object_dead_bit) ? ", dead" : "",
				TEST_FLAG(object->object.flags, _object_outside_of_map_bit) ? ", outside the map" : "",
				level_run_point_in_world(&object->object.bounding_sphere_center) ? "" : ", not in this BSP");
		}
	}
	level_run_log_threads();
}

static void level_run_summary(
	char const *why)
{
	platform_log("level run: %s %s at tick %ld (%ld ticks played), %ld checkpoints; %ld volumes walked to, %ld enemies fought; "
		"aids: %ld teleports, %ld encounters killed, %ld seats taken, %ld controls used, %ld screens closed",
		level_run.scenario_name, why, level_run.last_tick, level_run.last_tick - level_run.begin_tick,
		game_state_port_checkpoints_saved() - level_run.begin_checkpoints, level_run.walked_volumes, level_run.fights,
		level_run.aid_teleports, level_run.aid_kills, level_run.aid_seats, level_run.aid_devices, level_run.aid_screens);
}

/* ---------- choosing where to go */

static void level_run_set_target(
	short type,
	short index,
	long ai_index,
	long object_index,
	real_point3d const *point,
	long script_index,
	char const *seat)
{
	long tick = game_time_get();
	boolean same = level_run.target_type == type && level_run.target_index == index &&
		level_run.target_ai == ai_index && level_run.target_object == object_index;

	level_run.target_point = *point;
	if (same)
		return;
	level_run.target_type = type;
	level_run.target_index = index;
	level_run.target_ai = ai_index;
	level_run.target_object = object_index;
	level_run.target_script = script_index;
	level_run.target_seat = seat;
	level_run.target_since = tick;
	level_run.target_best_distance = REAL_MAX;
	level_run.target_best_tick = tick;
	level_run.beside_since = NONE;
	level_run.in_volume_since = NONE;
	level_run.path_tick = NONE;
	level_run.path_valid = FALSE;
	level_run_route.found_tick = NONE;
	switch (type)
	{
	case _level_run_target_volume:
		platform_log("level run: tick %ld: walks to volume %s (%.1f %.1f %.1f) for %s", tick, level_run_volume_name(index),
			point->x, point->y, point->z, level_run_script_name(script_index));
		break;
	case _level_run_target_bsp_switch:
		platform_log("level run: tick %ld: walks to volume %s (%.1f %.1f %.1f), into BSP %d", tick, level_run_volume_name(index),
			point->x, point->y, point->z, (int)ai_index);
		break;
	case _level_run_target_vehicle:
		platform_log("level run: tick %ld: walks to vehicle %s, seat %s, for %s", tick, level_run_object_name(index), seat,
			level_run_script_name(script_index));
		break;
	case _level_run_target_ai:
	{
		char name[128];

		ai_index_to_string(ai_index, global_scenario_get(), name, sizeof(name));
		platform_log("level run: tick %ld: goes to fight %s (%d living) for %s", tick, name,
			(int)ai_scripting_living_count(ai_index), level_run_script_name(script_index));
		level_run.fought_living = ai_scripting_living_count(ai_index);
		level_run.fought_since = tick;
		break;
	}
	case _level_run_target_control:
		platform_log("level run: tick %ld: walks to control %s for %s", tick, tag_get_name(object_get(object_index)->definition_index),
			level_run_script_name(script_index));
		break;
	case _level_run_target_door:
		platform_log("level run: tick %ld: walks to door %s for %s", tick, level_run_object_name(index),
			level_run_script_name(script_index));
		break;
	case _level_run_target_destroy:
		platform_log("level run: tick %ld: goes to destroy %s", tick, level_run.destroy_names[index]);
		level_run.fought_since = tick;
		break;
	case _level_run_target_see:
		platform_log("level run: tick %ld: turns to %s for %s", tick, ai_index == _level_run_goal_see_object ?
			level_run_object_name(index) : "a flag", level_run_script_name(script_index));
		break;
	default:
		break;
	}
}

/* a control that moves the device (or its group), in this BSP: where to
stand to use it */
static long level_run_find_control(
	long device_index,
	short group_index,
	boolean elsewhere,
	real_point3d *stand)
{
	struct object_iterator iterator;
	struct control_datum *control;

	object_iterator_new(&iterator, _object_mask_control, 0);
	while ((control = (struct control_datum *)object_iterator_next(&iterator)) != NULL)
	{
		boolean matches = iterator.index == device_index;

		if (!matches && device_index != NONE)
		{
			struct device_datum *device = device_get(device_index);

			matches = device->device.position_group_index != NONE &&
				control->device.position_group_index == device->device.position_group_index;
		}
		if (!matches && group_index != NONE)
			matches = control->device.position_group_index == group_index;
		/* (elsewhere: one in another BSP, where it is) */
		if (elsewhere && matches && device_can_change_position(iterator.index) &&
			(TEST_FLAG(control->object.flags, _object_outside_of_map_bit) ||
				!level_run_point_in_world(&control->object.bounding_sphere_center)))
		{
			*stand = control->object.bounding_sphere_center;
			return iterator.index;
		}
		if (!elsewhere && matches && device_can_change_position(iterator.index) &&
			!TEST_FLAG(control->object.flags, _object_outside_of_map_bit) &&
			level_run_point_in_world(&control->object.bounding_sphere_center))
		{
			stand->x = control->object.position.x + control->object.forward.i * 0.6f;
			stand->y = control->object.position.y + control->object.forward.j * 0.6f;
			stand->z = control->object.position.z;
			if (!level_run_point_in_world(stand))
				*stand = control->object.position;
			return iterator.index;
		}
	}
	return NONE;
}

/* whether he has turned to what a sleep_until wants seen */
static boolean level_run_tried(
	long sleep_until_index)
{
	short tried_index;

	for (tried_index = 0; tried_index < level_run.tried_count; tried_index++)
	{
		if (level_run.tried_sleep_untils[tried_index] == sleep_until_index)
			return TRUE;
	}
	return FALSE;
}

/* where what the scripts want seen is: an object's center (a named one, or
an encounter's first living actor), a flag */
static boolean level_run_see_point(
	short goal_type,
	short index,
	long ai_index,
	real_point3d *point)
{
	if (goal_type == _level_run_goal_see_object && index == NONE)
	{
		struct ai_script_actor_reference_iterator iterator;
		struct actor_datum *actor;

		ai_index_actor_iterator_new(ai_index, &iterator);
		while ((actor = ai_index_actor_iterator_next(&iterator)) != NULL)
		{
			if (actor->meta.unit_index != NONE &&
				!TEST_FLAG(object_get(actor->meta.unit_index)->object.damage_flags, _object_dead_bit))
			{
				*point = object_get(actor->meta.unit_index)->object.bounding_sphere_center;
				return TRUE;
			}
		}
		return FALSE;
	}
	if (goal_type == _level_run_goal_see_object)
	{
		long object_index = object_index_from_name_index(index);

		if (object_index == NONE)
			return FALSE;
		*point = object_get(object_index)->object.bounding_sphere_center;
		return TRUE;
	}
	if (index < 0 || index >= global_scenario_get()->cutscene_flags.count)
		return FALSE;
	*point = TAG_BLOCK_GET_ELEMENT(&global_scenario_get()->cutscene_flags, index, struct scenario_cutscene_flag)->position;
	return TRUE;
}

/* an object the script says to destroy, if it is here and not yet destroyed */
static long level_run_destroy_object(
	short destroy_index)
{
	struct scenario *scenario = global_scenario_get();
	short name_index;

	for (name_index = 0; name_index < scenario->object_names.count; name_index++)
	{
		if (!csstrcmp(TAG_BLOCK_GET_ELEMENT(&scenario->object_names, name_index, struct scenario_object_name)->name,
			level_run.destroy_names[destroy_index]))
		{
			long object_index = object_index_from_name_index(name_index);
			struct object_datum *object;

			if (object_index == NONE)
				return NONE;
			object = object_get(object_index);
			if (TEST_FLAG(object->object.damage_flags, _object_dead_bit) || object->object.body_vitality <= 0.1f ||
				TEST_FLAG(object->object.flags, _object_outside_of_map_bit) ||
				!level_run_point_in_world(&object->object.bounding_sphere_center))
			{
				return NONE;
			}
			return object_index;
		}
	}
	return NONE;
}

/* a place to go, and how it ranks: by the story (the latest a script that
waits on it moved on: the level's mission scripts move on as he plays,
while those that save the game at a place wait there from the start), then
its kind (the order of the enum below), then the distance */
enum
{
	_level_run_class_passenger,
	_level_run_class_see,
	_level_run_class_volume,
	_level_run_class_bsp_switch,
	_level_run_class_driver,
	_level_run_class_ai,
	_level_run_class_device,
	_level_run_class_destroy,
	_level_run_class_none,
};

struct level_run_candidate
{
	short class_index;
	long moved;
	real distance;
	short type;
	short index;
	long ai_index;
	long object_index;
	real_point3d point;
	long script_index;
	long sleep_until_index;
	char const *seat;
};

static void level_run_consider(
	struct level_run_candidate *best,
	short class_index,
	long script_index,
	real distance,
	short type,
	short index,
	long ai_index,
	long object_index,
	real_point3d const *point,
	char const *seat,
	long sleep_until_index)
{
	long moved = level_run_script_moved(script_index);

	if (moved < best->moved ||
		(moved == best->moved && (class_index > best->class_index ||
			(class_index == best->class_index && distance >= best->distance))))
	{
		return;
	}
	best->class_index = class_index;
	best->moved = moved;
	best->distance = distance;
	best->type = type;
	best->index = index;
	best->ai_index = ai_index;
	best->object_index = object_index;
	best->point = *point;
	best->script_index = script_index;
	best->seat = seat;
	best->sleep_until_index = sleep_until_index;
}

static short level_run_target_class(
	void)
{
	switch (level_run.target_type)
	{
	case _level_run_target_vehicle:
		return strstr(level_run.target_seat, "driver") ? _level_run_class_driver : _level_run_class_passenger;
	case _level_run_target_volume:
		return _level_run_class_volume;
	case _level_run_target_bsp_switch:
		return _level_run_class_bsp_switch;
	case _level_run_target_ai:
		return _level_run_class_ai;
	case _level_run_target_control:
	case _level_run_target_door:
		return _level_run_class_device;
	case _level_run_target_see:
		return _level_run_class_see;
	case _level_run_target_destroy:
		return _level_run_class_destroy;
	default:
		return _level_run_class_none;
	}
}

/* where he goes next, of what the scripts wait on: kept until it is
reached or no longer waited on, unless something ranks above it */
static void level_run_choose(
	long unit_index,
	long mover_index)
{
	struct scenario *scenario = global_scenario_get();
	real_point3d const *from = &object_get(mover_index)->object.position;
	struct level_run_candidate best;
	short goal_index;
	short elsewhere_count = 0;
	short elsewhere[MAXIMUM_LEVEL_RUN_GOALS];
	real_point3d elsewhere_points[MAXIMUM_LEVEL_RUN_GOALS];
	long elsewhere_moved = -1;

	best.class_index = _level_run_class_none;
	best.moved = 0;
	best.distance = REAL_MAX;
	for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
	{
		struct level_run_goal *goal = &level_run.goals[goal_index];

		switch (goal->type)
		{
		case _level_run_goal_vehicle:
		{
			long vehicle_index = object_index_from_name_index(goal->index);

			/* (one in this BSP, that he is not in) */
			if (vehicle_index != NONE && object_get(unit_index)->object.parent_object_index != vehicle_index &&
				!TEST_FLAG(object_get(vehicle_index)->object.flags, _object_outside_of_map_bit) &&
				level_run_point_in_world(&object_get(vehicle_index)->object.bounding_sphere_center))
			{
				level_run_consider(&best, strstr(goal->seat, "driver") ? _level_run_class_driver : _level_run_class_passenger,
					goal->script_index, distance_squared3d(from, &object_get(vehicle_index)->object.position),
					_level_run_target_vehicle, goal->index, NONE, vehicle_index, &object_get(vehicle_index)->object.position, goal->seat, NONE);
			}
			break;
		}
		case _level_run_goal_volume:
		{
			real_point3d middle, position;

			if (level_run_visited(goal->index, goal->sleep_until_index) || BIT_VECTOR_TEST_FLAG(level_run.dangerous, goal->index))
				break;
			level_run_volume_point(level_run_volume_get(goal->index), 0.5f, 0.5f, 0.5f, &middle);
			/* (one with a floor in this BSP; else another BSP's) */
			if (level_run_volume_position(goal->index, mover_index, &position))
			{
				level_run_consider(&best, _level_run_class_volume, goal->script_index, distance_squared3d(from, &middle),
					_level_run_target_volume, goal->index, NONE, NONE, &position, NULL, goal->sleep_until_index);
			}
			else
			{
				/* (those of the latest script to move on) */
				long moved = level_run_script_moved(goal->script_index);

				level_run.not_in_bsps[goal->index] |= 1UL << (global_structure_bsp_index & 31);
				if (moved > elsewhere_moved)
				{
					elsewhere_moved = moved;
					elsewhere_count = 0;
				}
				if (moved == elsewhere_moved)
				{
					elsewhere_points[elsewhere_count] = middle;
					elsewhere[elsewhere_count++] = goal->index;
				}
			}
			break;
		}
		case _level_run_goal_ai:
		{
			long enemy_index;

			if (ai_scripting_living_count(goal->ai_index) <= 0)
				break;
			enemy_index = level_run_enemy(unit_index, goal->ai_index, 1000.0f, FALSE);
			if (enemy_index != NONE && level_run_point_in_world(&object_get(enemy_index)->object.bounding_sphere_center))
			{
				level_run_consider(&best, _level_run_class_ai, goal->script_index,
					distance_squared3d(from, &object_get(enemy_index)->object.position), _level_run_target_ai, NONE,
					goal->ai_index, NONE, &object_get(enemy_index)->object.position, NULL, NONE);
			}
			break;
		}
		case _level_run_goal_device:
		case _level_run_goal_device_group:
		{
			/* (a control of it, else the door itself) */
			long device_index = NONE;
			long control_index;
			real_point3d stand;

			/* (a door he has stood at, that did not open: others first) */
			if (level_run_tried(goal->sleep_until_index))
				break;
			if (goal->type == _level_run_goal_device)
			{
				device_index = object_index_from_name_index(goal->index);
				if (device_index == NONE || !TEST_FLAG(_object_mask_device, object_get(device_index)->object.type))
					break;
			}
			control_index = level_run_find_control(device_index, goal->type == _level_run_goal_device_group ? goal->index : NONE,
				FALSE, &stand);
			if (control_index != NONE)
			{
				level_run_consider(&best, _level_run_class_device, goal->script_index, distance_squared3d(from, &stand),
					_level_run_target_control, goal->index, NONE, control_index, &stand, NULL, goal->sleep_until_index);
			}
			else if (level_run_find_control(device_index, goal->type == _level_run_goal_device_group ? goal->index : NONE,
				TRUE, &stand) != NONE)
			{
				/* (a control in another BSP: by its BSP switch, as a volume's) */
				long moved = level_run_script_moved(goal->script_index);

				if (moved > elsewhere_moved)
				{
					elsewhere_moved = moved;
					elsewhere_count = 0;
				}
				if (moved == elsewhere_moved)
				{
					elsewhere_points[elsewhere_count] = stand;
					elsewhere[elsewhere_count++] = NONE;
				}
			}
			else if (device_index != NONE && object_get(device_index)->object.type == _object_type_machine &&
				!TEST_FLAG(object_get(device_index)->object.flags, _object_outside_of_map_bit) &&
				level_run_point_in_world(&object_get(device_index)->object.bounding_sphere_center))
			{
				level_run_consider(&best, _level_run_class_device, goal->script_index,
					distance_squared3d(from, &object_get(device_index)->object.bounding_sphere_center), _level_run_target_door,
					goal->index, NONE, device_index, &object_get(device_index)->object.bounding_sphere_center, NULL,
					goal->sleep_until_index);
			}
			break;
		}
		case _level_run_goal_see_object:
		case _level_run_goal_see_flag:
		{
			/* (turned to, from where he stands, once for each wait) */
			real_point3d point;

			if (level_run_tried(goal->sleep_until_index) || !level_run_see_point(goal->type, goal->index, goal->ai_index, &point))
				break;
			level_run_consider(&best, _level_run_class_see, goal->script_index, distance_squared3d(from, &point),
				_level_run_target_see, goal->index, goal->type, goal->ai_index, &point, NULL, goal->sleep_until_index);
			break;
		}
		default:
			break;
		}
	}
	/* what the script says to destroy (d40's manifolds' detectors), alive here */
	{
		short destroy_index;

		for (destroy_index = 0; destroy_index < level_run.destroy_count; destroy_index++)
		{
			long object_index = level_run_destroy_object(destroy_index);

			if (object_index != NONE)
			{
				level_run_consider(&best, _level_run_class_destroy, NONE,
					distance_squared3d(from, &object_get(object_index)->object.bounding_sphere_center), _level_run_target_destroy,
					destroy_index, NONE, object_index, &object_get(object_index)->object.bounding_sphere_center, NULL, NONE);
			}
		}
	}
	/* no volume here: another BSP's, by the engine's switch nearest one of them */
	if (elsewhere_count > 0 && (elsewhere_moved > best.moved ||
		(elsewhere_moved == best.moved && best.class_index > _level_run_class_bsp_switch)))
	{
		short switch_index = level_run_bsp_switch_toward(elsewhere, elsewhere_points, elsewhere_count);

		if (switch_index != NONE)
		{
			struct level_run_bsp_switch *bsp_switch = TAG_BLOCK_GET_ELEMENT(&scenario->bsp_switch_trigger_volumes,
				switch_index, struct level_run_bsp_switch);
			real_point3d position;

			if (level_run_volume_position(bsp_switch->trigger_volume_index, mover_index, &position))
			{
				best.class_index = _level_run_class_bsp_switch;
				best.moved = elsewhere_moved;
				best.distance = 0.0f;
				best.type = _level_run_target_bsp_switch;
				best.index = bsp_switch->trigger_volume_index;
				best.ai_index = bsp_switch->destination_structure_bsp_index;
				best.object_index = switch_index;
				best.point = position;
				best.script_index = NONE;
				best.seat = NULL;
				best.sleep_until_index = NONE;
			}
			else
			{
				level_run.switch_crossed[switch_index] = game_time_get();
			}
		}
	}
	/* (only what the story's scripts wait on, once they have moved on: what
	the others wait on, a minute later; a10's opening has nothing to walk to) */
	if (best.class_index != _level_run_class_none && best.class_index != _level_run_class_see &&
		best.class_index != _level_run_class_destroy && best.moved <= 0)
	{
		if (level_run.unmoved_since == NONE)
			level_run.unmoved_since = game_time_get();
		if (game_time_get() - level_run.unmoved_since < LEVEL_RUN_UNMOVED_TICKS)
		{
			if (level_run.target_type != _level_run_target_none)
				platform_log("level run: tick %ld: waits for the story", (long)game_time_get());
			level_run.target_type = _level_run_target_none;
			return;
		}
	}
	else
	{
		level_run.unmoved_since = NONE;
	}
	/* (the target kept unless this ranks above it) */
	if (level_run.target_type != _level_run_target_none)
	{
		short class_index = level_run_target_class();

		long moved = level_run.target_type == _level_run_target_bsp_switch ? level_run.target_moved :
			level_run_script_moved(level_run.target_script);

		if (best.moved < moved || (best.moved == moved && best.class_index >= class_index))
			return;
	}
	if (best.class_index == _level_run_class_none)
	{
		if (elsewhere_count > 0 && game_time_get() % 900 == 0)
		{
			platform_log("level run: tick %ld: %d volumes are in another BSP, and no BSP switch from %d leads there",
				(long)game_time_get(), (int)elsewhere_count, (int)global_structure_bsp_index);
		}
		level_run.target_type = _level_run_target_none;
		return;
	}
	level_run_set_target(best.type, best.index, best.ai_index, best.object_index, &best.point, best.script_index, best.seat);
	level_run.target_sleep_until = best.sleep_until_index;
	level_run.target_moved = best.moved;
}

/* ---------- the aids */

static void level_run_aid_teleport(
	long mover_index,
	char const *why)
{
	real_point3d position = level_run.target_point;
	real_point3d above = level_run.target_point;
	real_vector3d down;
	struct collision_result collision;

	/* (on the floor under the target: none, and he is not put in the air, to
	fall out of the level; the target is given up) */
	above.z += 1.0f;
	down.i = 0.0f;
	down.j = 0.0f;
	down.k = -20.0f;
	/* (the floor may be a machine's: a lift's platform, b40's) */
	if (!collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_machines_bit) | FLAG(_collision_test_objects_scenery_bit),
		&above, &down, mover_index, &collision))
	{
		/* (a volume in the air, a lift's shaft or a gap: held in it, as passing
		through it would, until its script or the BSP has seen him; then he
		falls, and is stood back if he falls out of the level) */
		if (level_run.target_type == _level_run_target_volume || level_run.target_type == _level_run_target_bsp_switch)
		{
			level_run.aid_teleports++;
			platform_log("level run: tick %ld: aid: held in the air at %.1f %.1f %.1f (%s), no nearer for %d seconds",
				(long)game_time_get(), position.x, position.y, position.z, why, LEVEL_RUN_STUCK_TICKS / 30);
			level_run.hold_position = position;
			level_run.hold_until = game_time_get() + LEVEL_RUN_VOLUME_TICKS;
			level_run_place(mover_index, &position);
			level_run.target_best_distance = REAL_MAX;
			level_run.target_best_tick = game_time_get();
			return;
		}
		platform_log("level run: tick %ld: no floor at %.1f %.1f %.1f (%s), given up", (long)game_time_get(),
			position.x, position.y, position.z, why);
		if (level_run.target_type == _level_run_target_volume && level_run.visit_count < MAXIMUM_LEVEL_RUN_VISITS)
		{
			level_run.visited_volumes[level_run.visit_count] = level_run.target_index;
			level_run.visited_sleep_untils[level_run.visit_count] = level_run.target_sleep_until;
			level_run.visit_count++;
		}
		else if (level_run.tried_count < MAXIMUM_LEVEL_RUN_VISITS)
		{
			level_run.tried_sleep_untils[level_run.tried_count++] = level_run.target_sleep_until;
		}
		if (level_run.target_type == _level_run_target_bsp_switch)
			level_run.switch_crossed[level_run.target_object] = game_time_get();
		level_run.target_type = _level_run_target_none;
		return;
	}
	position = collision.point;
	position.z += 0.1f;
	level_run.aid_teleports++;
	platform_log("level run: tick %ld: aid: stood at %.1f %.1f %.1f (%s), no nearer for %d seconds", (long)game_time_get(),
		position.x, position.y, position.z, why, LEVEL_RUN_STUCK_TICKS / 30);
	level_run_place(mover_index, &position);
	level_run.target_best_distance = REAL_MAX;
	level_run.target_best_tick = game_time_get();
}

static void level_run_aid_kill(
	long ai_index,
	char const *why)
{
	char name[128];

	ai_index_to_string(ai_index, global_scenario_get(), name, sizeof(name));
	level_run.aid_kills++;
	platform_log("level run: tick %ld: aid: %s killed (%d living), %s", (long)game_time_get(), name,
		(int)ai_scripting_living_count(ai_index), why);
	ai_scripting_kill(ai_index);
}

/* whether a script waits on a device too: he stands in its volume while
the device moves (c20's lifts: in the volume, and the platform risen) */
static boolean level_run_script_waits_device(
	long script_index)
{
	short goal_index;

	for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
	{
		if (level_run.goals[goal_index].script_index == script_index &&
			(level_run.goals[goal_index].type == _level_run_goal_device ||
				level_run.goals[goal_index].type == _level_run_goal_device_group))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* ---------- each tick */

/* a cutscene, or the scripts holding him: no time stuck or fighting */
static void level_run_pause(
	long tick)
{
	level_run.target_best_tick = tick;
	level_run.fought_since = tick;
	if (level_run.beside_since != NONE)
		level_run.beside_since = tick;
}

static void level_run_begin_level(
	void)
{
	struct scenario *scenario = global_scenario_get();
	short switch_index;

	if (level_run.scenario_name[0])
		level_run_summary("left");
	platform_log("level run: %s begins", tag_get_name(global_scenario_index));
	level_run_read_functions();
	snprintf(level_run.scenario_name, sizeof(level_run.scenario_name), "%s", tag_get_name(global_scenario_index));
	level_run.begin_tick = game_time_get();
	level_run.begin_checkpoints = game_state_port_checkpoints_saved();
	level_run.next_read_tick = game_time_get();
	level_run.last_progress_tick = game_time_get();
	level_run.target_type = _level_run_target_none;
	level_run.unmoved_since = NONE;
	level_run.outside_since = NONE;
	level_run.last_inside_valid = FALSE;
	level_run.hold_until = NONE;
	level_run.goals_hash = 0;
	level_run.visit_count = 0;
	level_run.tried_count = 0;
	level_run.dangerous_allowed = FALSE;
	level_run.enemy_unit_index = NONE;
	level_run.aid_teleports = level_run.aid_kills = level_run.aid_seats = level_run.aid_devices = level_run.aid_screens = 0;
	level_run.walked_volumes = level_run.fights = 0;
	for (switch_index = 0; switch_index < MAXIMUM_LEVEL_RUN_BSP_SWITCHES; switch_index++)
		level_run.switch_crossed[switch_index] = NONE;
	csmemset(level_run.dangerous, 0, sizeof(level_run.dangerous));
	csmemset(level_run.not_in_bsps, 0, sizeof(level_run.not_in_bsps));
	/* (NONE throughout: -1 in every byte) */
	csmemset(level_run.volume_awaited, 0xFF, sizeof(level_run.volume_awaited));
	csmemset(level_run.device_awaited, 0xFF, sizeof(level_run.device_awaited));
	csmemset(level_run.group_awaited, 0xFF, sizeof(level_run.group_awaited));
	csmemset(level_run.script_expressions, 0xFF, sizeof(level_run.script_expressions));
	csmemset(level_run.script_moved, 0, sizeof(level_run.script_moved));
	level_run.scripts_read = FALSE;
	for (switch_index = 0; switch_index < scenario->bsp_switch_trigger_volumes.count; switch_index++)
	{
		struct level_run_bsp_switch *bsp_switch = TAG_BLOCK_GET_ELEMENT(&scenario->bsp_switch_trigger_volumes,
			switch_index, struct level_run_bsp_switch);

		platform_log("level run: BSP switch %d: %d to %d by volume %s", (int)switch_index,
			(int)bsp_switch->source_structure_bsp_index, (int)bsp_switch->destination_structure_bsp_index,
			bsp_switch->trigger_volume_index >= 0 && bsp_switch->trigger_volume_index < scenario->trigger_volumes.count ?
				level_run_volume_name(bsp_switch->trigger_volume_index) : "?");
	}
}

/* the buttons a script waits on (all of them: a10's tutorial waits on
looking left and right), but a question's yes, not its no */
static void level_run_press_actions(
	long tick)
{
	short goal_index;
	short look_count = 0;
	short look_goals[16];

	for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
	{
		struct level_run_goal *goal = &level_run.goals[goal_index];
		char const *name;

		if (goal->type != _level_run_goal_action)
			continue;
		name = hs_function_get(goal->index)->name + 19;
		if (!strncmp(name, "look_relative", 13))
		{
			if (look_count < (short)NUMBEROF(look_goals))
				look_goals[look_count++] = goal_index;
			continue;
		}
		/* (pressed and let go) */
		if ((tick / 4) % 2)
			continue;
		if (!strcmp(name, "action"))
			level_run.buttons |= _level_run_button_action;
		else if (!strcmp(name, "jump") || !strcmp(name, "accept"))
			level_run.buttons |= _level_run_button_jump;
		else if (!strcmp(name, "back"))
		{
			short other_index;
			boolean asked = FALSE;

			for (other_index = 0; other_index < level_run.goal_count; other_index++)
			{
				asked |= level_run.goals[other_index].type == _level_run_goal_action &&
					level_run.goals[other_index].script_index == goal->script_index &&
					!strcmp(hs_function_get(level_run.goals[other_index].index)->name + 19, "accept");
			}
			if (!asked)
				level_run.buttons |= _level_run_button_back;
		}
		else if (!strcmp(name, "primary_trigger"))
			level_run.buttons |= _level_run_button_fire;
		else if (!strcmp(name, "grenade_trigger"))
			level_run.buttons |= _level_run_button_grenade;
		else if (!strcmp(name, "zoom"))
			level_run.buttons |= _level_run_button_zoom;
		else if (!strcmp(name, "move_relative_all_directions"))
			level_run.stick_forward = (tick / 20) % 2 ? 0.8f : -0.8f;
	}
	/* (the looking ones in turn, a second each) */
	if (look_count > 0)
	{
		char const *name = hs_function_get(level_run.goals[look_goals[(tick / 30) % look_count]].index)->name + 19;

		if (!strcmp(name, "look_relative_up"))
			level_run.look_y = 0.7f;
		else if (!strcmp(name, "look_relative_down"))
			level_run.look_y = -0.7f;
		else if (!strcmp(name, "look_relative_left"))
			level_run.look_x = -0.7f;
		else if (!strcmp(name, "look_relative_right"))
			level_run.look_x = 0.7f;
		else
		{
			/* (every way, a quarter second each) */
			switch ((tick / 8) % 4)
			{
			case 0: level_run.look_x = 0.7f; break;
			case 1: level_run.look_y = 0.7f; break;
			case 2: level_run.look_x = -0.7f; break;
			default: level_run.look_y = -0.7f; break;
			}
		}
	}
}

/* toward the target: the path's next point, he turned to it and walking;
the target's distance kept, for being stuck */
static void level_run_walk(
	long unit_index,
	long mover_index,
	long tick,
	boolean fighting)
{
	real_point3d const *position = &object_get(mover_index)->object.position;
	real distance = distance3d(position, &level_run.target_point);
	real_point3d toward;

	if (distance < level_run.target_best_distance - LEVEL_RUN_PROGRESS_DISTANCE)
	{
		level_run.target_best_distance = distance;
		level_run.target_best_tick = tick;
	}
	/* the way over the BSP: found again every few seconds, or for another
	target, BSP, or when he strays from it */
	if (level_run_route.found_tick == NONE || tick - level_run_route.found_tick >= LEVEL_RUN_ROUTE_TICKS ||
		level_run_route.found_bsp != global_structure_bsp_index ||
		distance3d(&level_run_route.found_target, &level_run.target_point) > 1.0f ||
		(level_run_route.point_count > 0 && level_run_route.point_index < level_run_route.point_count &&
			distance3d(position, &level_run_route.points[level_run_route.point_index]) > 6.0f))
	{
		real_point3d start, goal;
		long start_surface, goal_surface;

		if (mover_index == unit_index && TEST_FLAG(_object_mask_biped, object_get(unit_index)->object.type))
			start_surface = biped_find_pathfinding_surface_index(unit_index, &start);
		else
			start_surface = level_run_floor(position, &start);
		goal_surface = level_run_floor(&level_run.target_point, &goal);
		/* (in the air, a jump: the way kept until he lands) */
		if (start_surface != NONE || level_run_route.point_count == 0)
		{
			level_run_route.found_tick = tick;
			level_run_route.found_target = level_run.target_point;
			level_run_route.found_bsp = global_structure_bsp_index;
			if (start_surface == NONE || goal_surface == NONE || !level_run_route_find(&start, start_surface, &goal, goal_surface))
				level_run_route.point_count = 0;
		}
	}
	if (level_run_route.point_count > 0)
	{
		short index, last = MIN(level_run_route.point_count - 1, level_run_route.point_index + 8);

		/* (on to the next point once at one, or past it: the nearest of the
		next few) */
		for (index = level_run_route.point_index + 1; index <= last; index++)
		{
			if (distance3d(position, &level_run_route.points[index]) <
				distance3d(position, &level_run_route.points[level_run_route.point_index]))
			{
				level_run_route.point_index = index;
			}
		}
		while (level_run_route.point_index < level_run_route.point_count - 1 &&
			distance3d(position, &level_run_route.points[level_run_route.point_index]) < 0.8f)
		{
			level_run_route.point_index++;
		}
		level_run.path_valid = TRUE;
		level_run.path_point = level_run_route.points[level_run_route.point_index];
	}
	else if (level_run.path_tick == NONE || tick - level_run.path_tick >= LEVEL_RUN_PATH_TICKS)
	{
		level_run.path_tick = tick;
		level_run.path_valid = level_run_find_path(unit_index, mover_index, &level_run.target_point, &level_run.path_point);
	}
	toward = level_run.path_valid ? level_run.path_point : level_run.target_point;
	if (tick - level_run.walk_logged_tick >= LEVEL_RUN_WALK_LOG_TICKS)
	{
		level_run.walk_logged_tick = tick;
		platform_log("level run: tick %ld: at %.1f %.1f %.1f, %.1f from the target (nearest %.1f, %ld ticks ago), %s %.1f %.1f %.1f%s",
			tick, position->x, position->y, position->z, distance, level_run.target_best_distance, tick - level_run.target_best_tick,
			level_run_route.point_count > 0 ? "way to" : level_run.path_valid ? "path to" : "no path, straight to", toward.x, toward.y,
			toward.z, fighting ? ", fighting" : "");
	}
	if (!fighting)
		level_run_face(unit_index, &toward, TRUE);
	level_run_move_toward(unit_index, mover_index, &toward, distance < 1.0f ? 0.4f + 0.6f * distance : 1.0f);
	/* (no nearer for a while: a jump, and a step aside, now one way and now
	the other) */
	if (tick - level_run.target_best_tick >= LEVEL_RUN_WIGGLE_TICKS)
	{
		long stuck = (tick - level_run.target_best_tick) / 30;

		if (stuck % 3 == 0 && tick % 30 < 4)
			level_run.buttons |= _level_run_button_jump;
		if (stuck % 4 >= 2)
			level_run.stick_right = (stuck / 4) % 2 ? 1.0f : -1.0f;
	}
}

static void level_run_act(
	long unit_index,
	long tick)
{
	long mover_index = object_get_ultimate_parent(unit_index);
	long parent_index = object_get(unit_index)->object.parent_object_index;
	long enemy_index;
	boolean fighting = FALSE;

	/* held by a device (a10's cryotube): as long as the scripts hold him */
	if (parent_index != NONE && !TEST_FLAG(_object_mask_unit, object_get(parent_index)->object.type))
	{
		level_run_pause(tick);
		return;
	}
	/* riding as a passenger (a dropship's extraction): staying aboard, unless
	nothing has moved on for a long time */
	if (parent_index != NONE && unit_get(parent_index)->unit.driver_object_index != unit_index &&
		!(level_run.target_type == _level_run_target_vehicle && level_run.target_object != parent_index))
	{
		level_run_pause(tick);
		if (tick - level_run.last_progress_tick < LEVEL_RUN_STALL_TICKS / 2)
			return;
		if (tick % 30 < 15)
			level_run.buttons |= _level_run_button_action;
		return;
	}
	/* (a volume no longer waited on: another) */
	if (level_run.target_type == _level_run_target_volume &&
		!level_run_goal_awaited(_level_run_goal_volume, level_run.target_index))
	{
		platform_log("level run: tick %ld: volume %s no longer awaited", tick, level_run_volume_name(level_run.target_index));
		level_run.target_type = _level_run_target_none;
	}
	/* (held in the air in a volume: until the scripts or the BSP have seen
	him there) */
	if (tick < level_run.hold_until)
	{
		boolean seen = level_run.target_type == _level_run_target_volume ?
			!level_run_goal_awaited(_level_run_goal_volume, level_run.target_index) :
			level_run.target_type == _level_run_target_bsp_switch && global_structure_bsp_index == (short)level_run.target_ai;

		if (!seen && level_run.target_type != _level_run_target_none)
		{
			level_run_place(mover_index, &level_run.hold_position);
			return;
		}
		level_run.hold_until = NONE;
	}
	if (tick % 30 == 0 || level_run.target_type == _level_run_target_none)
		level_run_choose(unit_index, mover_index);

	/* an enemy in sight, near: fought, walking on */
	enemy_index = level_run_enemy(unit_index, NONE, LEVEL_RUN_ENEMY_RANGE, TRUE);
	if (enemy_index != NONE)
	{
		level_run_fight(unit_index, enemy_index, tick);
		fighting = TRUE;
	}
	else
	{
		level_run.enemy_unit_index = NONE;
	}

	switch (level_run.target_type)
	{
	case _level_run_target_volume:
	case _level_run_target_bsp_switch:
		if (level_run_in_volume(level_run.target_index, mover_index))
		{
			/* (in it: standing until the scripts have seen him) */
			if (level_run.in_volume_since == NONE)
				level_run.in_volume_since = tick;
			if (level_run.target_type == _level_run_target_bsp_switch)
			{
				if (global_structure_bsp_index == (short)level_run.target_ai || tick - level_run.in_volume_since >= LEVEL_RUN_VOLUME_TICKS)
				{
					platform_log("level run: tick %ld: in BSP %d", tick, (int)global_structure_bsp_index);
					level_run.switch_crossed[level_run.target_object] = tick;
					level_run.target_type = _level_run_target_none;
				}
				else
				{
					level_run_move_toward(unit_index, mover_index, &level_run.target_point, 0.3f);
				}
				break;
			}
			if (!level_run_goal_awaited(_level_run_goal_volume, level_run.target_index) ||
				tick - level_run.in_volume_since >= (level_run_script_waits_device(level_run.target_script) ?
					LEVEL_RUN_RIDE_TICKS : LEVEL_RUN_VOLUME_TICKS))
			{
				short goal_index;
				boolean awaited = level_run_goal_awaited(_level_run_goal_volume, level_run.target_index);

				platform_log("level run: tick %ld: in volume %s, %s", tick, level_run_volume_name(level_run.target_index),
					awaited ? "still awaited" : "seen");
				level_run.walked_volumes++;
				/* (visited for the sleep_untils that wait on it: the target's,
				and any other read now) */
				if (level_run.visit_count < MAXIMUM_LEVEL_RUN_VISITS)
				{
					level_run.visited_volumes[level_run.visit_count] = level_run.target_index;
					level_run.visited_sleep_untils[level_run.visit_count] = level_run.target_sleep_until;
					level_run.visit_count++;
				}
				for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
				{
					struct level_run_goal *goal = &level_run.goals[goal_index];

					if (goal->type == _level_run_goal_volume && goal->index == level_run.target_index &&
						!level_run_visited(goal->index, goal->sleep_until_index) && level_run.visit_count < MAXIMUM_LEVEL_RUN_VISITS)
					{
						level_run.visited_volumes[level_run.visit_count] = goal->index;
						level_run.visited_sleep_untils[level_run.visit_count] = goal->sleep_until_index;
						level_run.visit_count++;
					}
				}
				level_run.target_type = _level_run_target_none;
			}
			break;
		}
		level_run_walk(unit_index, mover_index, tick, fighting);
		if (tick - level_run.target_best_tick >= LEVEL_RUN_STUCK_TICKS)
			level_run_aid_teleport(mover_index, level_run_volume_name(level_run.target_index));
		break;
	case _level_run_target_vehicle:
	{
		long vehicle_index = level_run.target_object;

		if (!object_try_and_get_and_verify_type(vehicle_index, _object_mask_unit) ||
			object_get(unit_index)->object.parent_object_index == vehicle_index)
		{
			level_run.target_type = _level_run_target_none;
			break;
		}
		level_run.target_point = object_get(vehicle_index)->object.position;
		if (distance3d(&object_get(mover_index)->object.bounding_sphere_center, &object_get(vehicle_index)->object.bounding_sphere_center) <
			0.9f * (object_get(vehicle_index)->object.bounding_sphere_radius + object_get(mover_index)->object.bounding_sphere_radius))
		{
			/* (beside it: the action button held, as getting in is) */
			if (level_run.beside_since == NONE)
				level_run.beside_since = tick;
			level_run.buttons |= _level_run_button_action;
			if (tick - level_run.beside_since >= LEVEL_RUN_BESIDE_TICKS)
			{
				level_run.aid_seats++;
				platform_log("level run: tick %ld: aid: seated in %s (%s)", tick, level_run_object_name(level_run.target_index),
					level_run.target_seat);
				unit_scripting_enter_vehicle(unit_index, vehicle_index, level_run.target_seat);
				level_run.target_type = _level_run_target_none;
			}
			break;
		}
		level_run_walk(unit_index, mover_index, tick, fighting);
		if (tick - level_run.target_best_tick >= LEVEL_RUN_STUCK_TICKS)
			level_run_aid_teleport(mover_index, level_run_object_name(level_run.target_index));
		break;
	}
	case _level_run_target_ai:
	{
		long target_enemy;
		short living = ai_scripting_living_count(level_run.target_ai);

		if (living <= 0)
		{
			level_run.target_type = _level_run_target_none;
			break;
		}
		if (living < level_run.fought_living)
		{
			level_run.fought_living = living;
			level_run.fought_since = tick;
		}
		if (tick - level_run.fought_since >= LEVEL_RUN_FIGHT_TICKS)
		{
			level_run_aid_kill(level_run.target_ai, "none of it killed for 90 seconds");
			level_run.target_type = _level_run_target_none;
			break;
		}
		/* (the nearest of it, fought once seen) */
		target_enemy = level_run_enemy(unit_index, level_run.target_ai, LEVEL_RUN_ENEMY_RANGE * 2.0f, TRUE);
		if (target_enemy != NONE)
		{
			level_run_fight(unit_index, target_enemy, tick);
			fighting = TRUE;
		}
		target_enemy = level_run_enemy(unit_index, level_run.target_ai, 1000.0f, FALSE);
		if (target_enemy == NONE)
		{
			level_run.target_type = _level_run_target_none;
			break;
		}
		level_run.target_point = object_get(target_enemy)->object.position;
		if (distance3d(&object_get(mover_index)->object.position, &level_run.target_point) > 3.0f)
			level_run_walk(unit_index, mover_index, tick, fighting);
		break;
	}
	case _level_run_target_control:
	case _level_run_target_door:
	{
		boolean awaited = level_run_goal_awaited(_level_run_goal_device, level_run.target_index) ||
			level_run_goal_awaited(_level_run_goal_device_group, level_run.target_index);

		if (!awaited)
		{
			level_run.target_type = _level_run_target_none;
			break;
		}
		/* (at a door, near it; at a control, touching it: the game looks for
		controls where his bounding sphere is) */
		if (level_run.target_type == _level_run_target_door ?
			distance3d(&object_get(mover_index)->object.position, &level_run.target_point) < 2.0f :
			distance3d(&object_get(mover_index)->object.bounding_sphere_center,
				&object_get(level_run.target_object)->object.bounding_sphere_center) <
				0.9f * (object_get(mover_index)->object.bounding_sphere_radius +
					object_get(level_run.target_object)->object.bounding_sphere_radius) ||
			distance3d(&object_get(mover_index)->object.position, &level_run.target_point) < 0.25f)
		{
			if (level_run.beside_since == NONE)
				level_run.beside_since = tick;
			if (level_run.target_type == _level_run_target_control)
			{
				/* (turned to it, the action button pressed now and then) */
				level_run_face(unit_index, &object_get(level_run.target_object)->object.bounding_sphere_center, FALSE);
				if ((tick / 10) % 2 == 0)
					level_run.buttons |= _level_run_button_action;
			}
			if (tick - level_run.beside_since >= LEVEL_RUN_BESIDE_TICKS)
			{
				if (level_run.target_type == _level_run_target_control)
				{
					level_run.aid_devices++;
					platform_log("level run: tick %ld: aid: control %s used", tick,
						tag_get_name(object_get(level_run.target_object)->definition_index));
					device_touched(level_run.target_object, unit_index);
				}
				else
				{
					platform_log("level run: tick %ld: door %s does not open", tick, level_run_object_name(level_run.target_index));
					if (level_run.tried_count < MAXIMUM_LEVEL_RUN_VISITS)
						level_run.tried_sleep_untils[level_run.tried_count++] = level_run.target_sleep_until;
				}
				level_run.target_type = _level_run_target_none;
			}
			break;
		}
		level_run_walk(unit_index, mover_index, tick, fighting);
		if (tick - level_run.target_best_tick >= LEVEL_RUN_STUCK_TICKS)
			level_run_aid_teleport(mover_index, "a control");
		break;
	}
	case _level_run_target_destroy:
	{
		long object_index = level_run_destroy_object(level_run.target_index);
		real_point3d eye;
		struct collision_result collision;
		real_vector3d line;
		boolean seen;

		if (object_index == NONE)
		{
			platform_log("level run: tick %ld: %s destroyed", tick, level_run.destroy_names[level_run.target_index]);
			level_run.target_type = _level_run_target_none;
			break;
		}
		if (tick - level_run.fought_since >= LEVEL_RUN_FIGHT_TICKS)
		{
			level_run.aid_kills++;
			platform_log("level run: tick %ld: aid: %s destroyed, not destroyed in 90 seconds", tick,
				level_run.destroy_names[level_run.target_index]);
			if (TEST_FLAG(_object_mask_unit, object_get(object_index)->object.type))
				unit_kill(object_index);
			level_run.target_type = _level_run_target_none;
			break;
		}
		level_run.target_point = object_get(object_index)->object.bounding_sphere_center;
		unit_get_head_position(unit_index, &eye);
		vector_from_points3d(&eye, &level_run.target_point, &line);
		seen = !collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
			&eye, &line, NONE, &collision);
		/* (shot at once in sight and near; walked to until then) */
		if (seen && distance3d(&eye, &level_run.target_point) < LEVEL_RUN_ENEMY_RANGE)
		{
			level_run_face(unit_index, &level_run.target_point, FALSE);
			if ((tick / 3) % 2 == 0)
				level_run.buttons |= _level_run_button_fire;
		}
		else
		{
			level_run_walk(unit_index, mover_index, tick, fighting);
		}
		break;
	}
	case _level_run_target_see:
	{
		/* (turned to it, standing, for at most three seconds) */
		boolean awaited = FALSE;
		short goal_index;

		for (goal_index = 0; goal_index < level_run.goal_count; goal_index++)
			awaited |= level_run.goals[goal_index].sleep_until_index == level_run.target_sleep_until;
		if (!fighting && level_run_see_point((short)level_run.target_ai, level_run.target_index, level_run.target_object,
			&level_run.target_point))
			level_run_face(unit_index, &level_run.target_point, FALSE);
		if (level_run.beside_since == NONE)
			level_run.beside_since = tick;
		if ((!awaited && tick - level_run.beside_since > LEVEL_RUN_AWAITED_TICKS) || tick - level_run.beside_since >= 90)
		{
			platform_log("level run: tick %ld: turned to %s, %s", tick, level_run.target_ai == _level_run_goal_see_object ?
				level_run_object_name(level_run.target_index) : "a flag", awaited ? "still awaited" : "seen");
			if (level_run.tried_count < MAXIMUM_LEVEL_RUN_VISITS)
				level_run.tried_sleep_untils[level_run.tried_count++] = level_run.target_sleep_until;
			level_run.target_type = _level_run_target_none;
		}
		break;
	}
	default:
	{
		/* nowhere to go: turned to what the scripts want seen */
		short goal_index;

		for (goal_index = 0; goal_index < level_run.goal_count && !fighting; goal_index++)
		{
			struct level_run_goal *goal = &level_run.goals[goal_index];

			if (goal->type == _level_run_goal_see_object)
			{
				long object_index = object_index_from_name_index(goal->index);

				if (object_index != NONE)
					level_run_face(unit_index, &object_get(object_index)->object.bounding_sphere_center, FALSE);
			}
			else if (goal->type == _level_run_goal_see_flag && goal->index >= 0 &&
				goal->index < global_scenario_get()->cutscene_flags.count)
			{
				level_run_face(unit_index, &TAG_BLOCK_GET_ELEMENT(&global_scenario_get()->cutscene_flags, goal->index,
					struct scenario_cutscene_flag)->position, FALSE);
			}
		}
		break;
	}
	}
}

void level_run_update(
	void)
{
	long tick;
	long unit_index;

	if (!level_run.active || !game_in_progress() || global_scenario_index == NONE)
		return;
	tick = game_time_get();
	/* a level begins: everything forgotten; the one the step began in left:
	the step's end */
	if (strcmp(tag_get_name(global_scenario_index), level_run.scenario_name))
	{
		if (level_run.begun_in[0] && strcmp(tag_get_name(global_scenario_index), level_run.begun_in))
		{
			level_run_summary("ended");
			platform_log("level run: %s begins: the end of %s", tag_get_name(global_scenario_index), level_run.begun_in);
			level_run.ended = TRUE;
			level_run.active = FALSE;
			level_run_route_free();
			return;
		}
		level_run_begin_level();
		if (!level_run.begun_in[0])
			snprintf(level_run.begun_in, sizeof(level_run.begun_in), "%s", level_run.scenario_name);
	}
	else if (tick < level_run.last_tick)
	{
		platform_log("level run: tick %ld: the level goes back (from %ld): a checkpoint loaded", tick, level_run.last_tick);
		level_run.target_type = _level_run_target_none;
		level_run.last_progress_tick = tick;
	}
	if (tick == level_run.last_tick)
		return;
	level_run.last_tick = tick;
	level_run.stick_forward = level_run.stick_right = level_run.look_x = level_run.look_y = 0.0f;
	level_run.buttons = 0;

	/* a help screen pauses the game until the player dismisses it, with A */
	if (ui_widgets_active())
	{
		level_run.paused_frames++;
		if ((level_run.paused_frames / 4) % 2)
			level_run.buttons |= _level_run_button_jump;
		if (level_run.paused_frames >= 150)
		{
			level_run.aid_screens++;
			platform_log("level run: tick %ld: aid: the screen that pauses the game closed", tick);
			ui_widgets_close_all();
			level_run.paused_frames = 0;
		}
		return;
	}
	level_run.paused_frames = 0;
	/* (the scripts turn them off around their cinematics) */
	cheat.deathless_player = TRUE;
	cheat.infinite_ammo = TRUE;
	cheat.bottomless_clip = TRUE;
	unit_index = level_run_player_unit();
	if (unit_index == NONE || TEST_FLAG(object_get(unit_index)->object.damage_flags, _object_dead_bit))
		return;

	/* fallen out of the level (no BSP under him for two seconds): stood back
	where he last was in it (on foot, outside a cutscene) */
	if (!cinematic_in_progress() && object_get(unit_index)->object.parent_object_index == NONE)
	{
		long mover_index = object_get_ultimate_parent(unit_index);
		real_point3d const *center = &object_get(mover_index)->object.bounding_sphere_center;

		if (level_run_point_in_world(center))
		{
			level_run.outside_since = NONE;
			if (tick % 30 == 0)
			{
				level_run.last_inside = object_get(mover_index)->object.position;
				level_run.last_inside_valid = TRUE;
			}
		}
		else if (level_run.outside_since == NONE)
		{
			level_run.outside_since = tick;
		}
		else if (tick - level_run.outside_since >= 60 && level_run.last_inside_valid)
		{
			level_run.aid_teleports++;
			platform_log("level run: tick %ld: aid: fell out of the level, stood back at %.1f %.1f %.1f", tick,
				level_run.last_inside.x, level_run.last_inside.y, level_run.last_inside.z);
			level_run_place(mover_index, &level_run.last_inside);
			level_run.outside_since = NONE;
			level_run.target_type = _level_run_target_none;
		}
	}
	/* what the scripts wait on */
	if (tick >= level_run.next_read_tick)
	{
		unsigned long hash;

		level_run.next_read_tick = tick + LEVEL_RUN_READ_TICKS;
		level_run_read_goals();
		hash = level_run_goals_hash();
		if (hash != level_run.goals_hash)
		{
			level_run.goals_hash = hash;
			level_run.last_progress_tick = tick;
			level_run.dangerous_allowed = FALSE;
		}
	}
	/* (a cinematic's time is not time with nothing new) */
	if (cinematic_in_progress())
	{
		level_run.last_progress_tick = tick;
		level_run_pause(tick);
		return;
	}
	/* nothing new for a long time: say what the scripts wait on, and visit
	the volumes again, those whose scripts then hurt him too */
	if (tick - level_run.last_progress_tick >= LEVEL_RUN_STALL_TICKS)
	{
		level_run_log_goals("nothing new for two minutes");
		csmemset(level_run.dangerous, 0, sizeof(level_run.dangerous));
		level_run.dangerous_allowed = TRUE;
		level_run.visit_count = 0;
		level_run.tried_count = 0;
		level_run.last_progress_tick = tick;
		level_run.target_type = _level_run_target_none;
	}
	level_run_press_actions(tick);
	/* (the scripts have taken his controls: a10's cryotube) */
	if (players_globals->input_disabled)
	{
		level_run_pause(tick);
		return;
	}
	level_run_act(unit_index, tick);
}

/* ---------- the scripted player's (test_input.c) */

/* the objective step begins: the level now played is played to its end */
void level_run_begin(
	void)
{
	/* (all forgotten but what the script said to destroy) */
	short destroy_count = level_run.destroy_count;
	char destroy_names[NUMBEROF(level_run.destroy_names)][NUMBEROF(level_run.destroy_names[0])];

	csmemcpy(destroy_names, level_run.destroy_names, sizeof(destroy_names));
	csmemset(&level_run, 0, sizeof(level_run));
	level_run.destroy_count = destroy_count;
	csmemcpy(level_run.destroy_names, destroy_names, sizeof(destroy_names));
	level_run.active = TRUE;
	level_run.target_type = _level_run_target_none;
	level_run.enemy_unit_index = NONE;
	level_run_route_free();
	level_run_route.found_tick = NONE;
	platform_log("level run: the objective, to the level's end");
}

/* an object the objective is to destroy where it finds it (the script's
destroy step) */
void level_run_destroy(
	char const *name)
{
	if (level_run.destroy_count < (short)NUMBEROF(level_run.destroy_names))
	{
		snprintf(level_run.destroy_names[level_run.destroy_count], sizeof(level_run.destroy_names[0]), "%s", name);
		level_run.destroy_count++;
	}
}

/* the step stops (its time ran out) */
void level_run_stop(
	void)
{
	if (level_run.active && level_run.scenario_name[0])
	{
		level_run_read_goals();
		level_run_log_goals("the objective's time ran out");
		level_run_summary("not ended");
	}
	level_run.active = FALSE;
	level_run_route_free();
}

/* the controller this frame: the sticks (left x and y, right x and y, each
-1 to 1) and buttons; FALSE once the level the step began in has ended */
int level_run_controller(
	float *sticks,
	unsigned long *buttons)
{
	sticks[0] = (float)level_run.stick_right;
	sticks[1] = (float)level_run.stick_forward;
	sticks[2] = (float)level_run.look_x;
	sticks[3] = (float)level_run.look_y;
	*buttons = level_run.buttons;
	return !level_run.ended;
}
