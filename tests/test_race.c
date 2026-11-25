#include "race.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static char events[65536];
static size_t event_length;

static void collect_event(void *context, const char *event)
{
    size_t remaining = sizeof(events) - event_length;
    int written;

    (void)context;
    written = snprintf(events + event_length, remaining, "%s\n", event);
    if (written > 0 && (size_t)written < remaining)
        event_length += (size_t)written;
}

static void reset_events(void)
{
    memset(events, 0, sizeof(events));
    event_length = 0;
}

static void set_only_car_active(Race *race, unsigned index)
{
    for (unsigned i = 0; i < race->car_count; ++i) {
        race->cars[i].active = i == index;
        race->cars[i].finished = 0;
        race->cars[i].retired = i != index;
    }
}

static void assert_route_has_no_repeated_cells(const RaceRoute *route)
{
    for (unsigned i = 0; i <= route->steps; ++i) {
        for (unsigned j = i + 1; j <= route->steps; ++j) {
            assert(route->lane[i] != route->lane[j] ||
                   route->cell[i] != route->cell[j]);
        }
    }
}

static void test_inward_and_outward_costs(void)
{
    RaceConfig config;
    Race race;
    RaceRoute route;
    char error[128];

    race_config_defaults(&config);
    config.lanes = 2;
    config.cells = 4;
    config.laps = 2;
    config.teams = 2;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    set_only_car_active(&race, 0);
    race.cars[0].lane = 1;
    race.cars[0].progress = 0;
    race.cars[1].lane = 1;
    race.cars[1].progress = 1;
    race.cars[1].active = 1;
    race.cars[1].retired = 0;
    for (unsigned i = 2; i < race.car_count; ++i) {
        race.cars[i].active = 0;
        race.cars[i].retired = 1;
    }
    assert(race_find_route(&race, 0, 2, &route) == 1);
    assert(route.lane[1] == 0);
    assert(route.forward == 1);
    assert(route.spent == 2);
    assert_route_has_no_repeated_cells(&route);

    set_only_car_active(&race, 0);
    race.cars[0].lane = 0;
    race.cars[0].progress = 0;
    race.cars[1].lane = 0;
    race.cars[1].progress = 1;
    race.cars[1].active = 1;
    race.cars[1].retired = 0;
    assert(race_find_route(&race, 0, 3, &route) == 0);
    assert(race_find_route(&race, 0, 4, &route) == 1);
    assert(route.lane[1] == 1);
    assert(route.forward == 1);
    assert(route.spent == 4);
    assert_route_has_no_repeated_cells(&route);
}

static void test_zero_move_and_round_retirement(void)
{
    RaceConfig config;
    Race race;
    RaceRoute route;
    char error[128];

    reset_events();
    race_config_defaults(&config);
    config.lanes = 2;
    config.cells = 4;
    config.laps = 1;
    config.teams = 2;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    race.cars[0].lane = 0;
    race.cars[0].progress = 0;
    race.cars[1].lane = 0;
    race.cars[1].progress = -3;
    race.cars[2].lane = 1;
    race.cars[2].progress = -4;
    race.cars[3].lane = 1;
    race.cars[3].progress = -2;
    assert(race_find_route(&race, 0, 4, &route) == 0);
    race_start(&race);
    race_run_round(&race);
    assert(race.cars[0].retired == 1);
    assert(strstr(events, "ZERO round=1 car=1") != NULL);
    assert(strstr(events, "RETIRED round=1 car=1 reason=blocked") != NULL);
    assert(strstr(events, "BLOCKED round=1 car=1") != NULL);
}

static void test_lap_bonus_and_finish(void)
{
    RaceConfig config;
    Race race;
    char error[128];

    reset_events();
    race_config_defaults(&config);
    config.lanes = 2;
    config.cells = 4;
    config.laps = 1;
    config.teams = 1;
    config.bonus_points = 7;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    set_only_car_active(&race, 0);
    race.cars[0].lane = 1;
    race.cars[0].progress = 3;
    race.cars[0].completed_laps = 0;
    race_start(&race);
    race_run_round(&race);
    assert(race.cars[0].finished == 1);
    assert(race.cars[0].points == 7);
    assert(race.cars[0].final_rank == 1);
    assert(race_is_done(&race));
    assert(strstr(events, "LAP round=1 car=1 lap=1") != NULL);
    assert(strstr(events, "BONUS round=1 car=1 lap=1 points=+7") != NULL);
    assert(strstr(events, "FINISH round=1 order=1 car=1") != NULL);
}

static void test_seeded_start_order(void)
{
    RaceConfig config;
    Race first;
    Race second;
    char error[128];

    race_config_defaults(&config);
    assert(race_init(&first, &config, NULL, NULL, error, sizeof(error)) == 0);
    assert(race_init(&second, &config, NULL, NULL, error, sizeof(error)) == 0);
    for (unsigned i = 0; i < first.car_count; ++i)
        assert(first.cars[i].start_order == second.cars[i].start_order);
}

static void test_inner_lane_breaks_leader_ties(void)
{
    RaceConfig config;
    Race race;
    char error[128];

    reset_events();
    race_config_defaults(&config);
    config.teams = 1;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    race.cars[0].active = 1;
    race.cars[0].lane = 2;
    race.cars[0].progress = 0;
    race.cars[1].active = 1;
    race.cars[1].lane = 0;
    race.cars[1].progress = 0;
    race_start(&race);
    race_run_round(&race);
    assert(strstr(events, "TURN_ORDER round=1 cars=2,1") != NULL);
}

static void test_points_classification(void)
{
    RaceConfig config;
    Race race;
    char error[128];

    reset_events();
    race_config_defaults(&config);
    config.teams = 1;
    config.points_classification = 1;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    race.cars[0].active = 0;
    race.cars[0].retired = 1;
    race.cars[0].points = 10;
    race.cars[1].active = 0;
    race.cars[1].finished = 1;
    race.cars[1].points = 3;
    race.cars[1].finish_round = 1;
    race.cars[1].finish_order = 1;
    race_start(&race);
    race_run_round(&race);
    assert(race.cars[0].final_rank == 1);
    assert(race.cars[1].final_rank == 2);
    assert(strstr(events, "FINAL rank=1 car=1") != NULL);
}

static void test_lagging_retirement_and_round_limit(void)
{
    RaceConfig config;
    Race race;
    char error[128];

    reset_events();
    race_config_defaults(&config);
    config.lanes = 3;
    config.cells = 32;
    config.laps = 100;
    config.teams = 2;
    config.retire_gap = 5;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    for (unsigned i = 2; i < race.car_count; ++i) {
        race.cars[i].active = 0;
        race.cars[i].retired = 1;
    }
    race.cars[0].lane = 2;
    race.cars[0].progress = 20;
    race.cars[1].lane = 0;
    race.cars[1].progress = 0;
    race_start(&race);
    race_run_round(&race);
    assert(race.cars[1].retired == 1);
    assert(strstr(events, "reason=lap-behind") != NULL);

    reset_events();
    race_config_defaults(&config);
    config.lanes = 2;
    config.cells = 4;
    config.laps = 100;
    config.teams = 1;
    config.max_rounds = 1;
    assert(race_init(&race, &config, collect_event, NULL,
                     error, sizeof(error)) == 0);
    race_start(&race);
    race_run_round(&race);
    assert(!race_is_done(&race));
    race_run_round(&race);
    assert(race_is_done(&race));
    assert(race.cars[0].retired == 1);
    assert(race.cars[1].retired == 1);
    assert(strstr(events, "reason=round-limit") != NULL);
}

int main(void)
{
    test_inward_and_outward_costs();
    test_zero_move_and_round_retirement();
    test_lap_bonus_and_finish();
    test_seeded_start_order();
    test_inner_lane_breaks_leader_ties();
    test_points_classification();
    test_lagging_retirement_and_round_limit();
    puts("race tests passed");
    return 0;
}
