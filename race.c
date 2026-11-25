#include "race.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUTE_BEAM_WIDTH 96
#define ROUTE_CANDIDATES (ROUTE_BEAM_WIDTH * 3)

typedef struct {
    unsigned lane[RACE_MAX_ROUTE_STEPS + 1];
    unsigned cell[RACE_MAX_ROUTE_STEPS + 1];
    unsigned steps;
    unsigned forward;
    unsigned spent;
} RouteState;

static void emit_event(const Race *race, const char *format, ...)
{
    char text[2048];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    if (race->event_fn != NULL)
        race->event_fn(race->event_context, text);
}

static uint64_t random_next(Race *race)
{
    uint64_t x = race->rng;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    race->rng = x;
    return x * UINT64_C(2685821657736338717);
}

static unsigned random_bounded(Race *race, unsigned bound)
{
    return (unsigned)(random_next(race) % bound);
}

unsigned race_cell(const Race *race, int64_t progress)
{
    int64_t cell = progress % (int64_t)race->config.cells;

    if (cell < 0)
        cell += race->config.cells;
    return (unsigned)cell;
}

void race_config_defaults(RaceConfig *config)
{
    config->lanes = 3;
    config->cells = 32;
    config->laps = 3;
    config->teams = 3;
    config->bonus_points = 5;
    config->retire_gap = 0;
    config->max_rounds = 1000;
    config->points_classification = 0;
    config->seed = UINT64_C(20261125);
}

int race_config_validate(const RaceConfig *config, char *error, size_t error_size)
{
    if (config->lanes < 2 || config->lanes > RACE_MAX_LANES) {
        snprintf(error, error_size, "lanes must be between 2 and %d", RACE_MAX_LANES);
        return -1;
    }
    if (config->teams == 0 || config->teams > RACE_MAX_TEAMS) {
        snprintf(error, error_size, "teams must be between 1 and %d", RACE_MAX_TEAMS);
        return -1;
    }
    if (config->cells < config->teams * 2 || config->cells > RACE_MAX_CELLS) {
        snprintf(error, error_size, "cells must fit all cars and be at most %d",
                 RACE_MAX_CELLS);
        return -1;
    }
    if (config->laps == 0 || config->laps > 10000) {
        snprintf(error, error_size, "laps must be between 1 and 10000");
        return -1;
    }
    if (config->bonus_points > 1000000) {
        snprintf(error, error_size, "bonus_points must be at most 1000000");
        return -1;
    }
    if (config->max_rounds == 0 || config->max_rounds > 1000000) {
        snprintf(error, error_size, "max_rounds must be between 1 and 1000000");
        return -1;
    }
    if (error_size > 0)
        error[0] = '\0';
    return 0;
}

int race_init(Race *race, const RaceConfig *config, RaceEventFn event_fn,
              void *event_context, char *error, size_t error_size)
{
    unsigned order[RACE_MAX_CARS];

    if (race_config_validate(config, error, error_size) != 0)
        return -1;

    memset(race, 0, sizeof(*race));
    race->config = *config;
    race->car_count = config->teams * 2;
    race->event_fn = event_fn;
    race->event_context = event_context;
    race->state = RACE_WAITING;
    race->rng = config->seed == 0 ? UINT64_C(1) : config->seed;

    for (unsigned i = 0; i < race->car_count; ++i)
        order[i] = i;
    for (unsigned i = race->car_count - 1; i > 0; --i) {
        unsigned j = random_bounded(race, i + 1);
        unsigned swap = order[i];

        order[i] = order[j];
        order[j] = swap;
    }

    for (unsigned i = 0; i < race->car_count; ++i) {
        RaceCar *car = &race->cars[order[i]];

        car->id = order[i] + 1;
        car->team = order[i] / 2 + 1;
        car->vehicle = order[i] % 2 + 1;
        car->start_order = i;
        car->lane = config->lanes - 1;
        car->progress = -(int64_t)i;
        car->active = 1;
    }
    return 0;
}

static int occupied_by_other(const Race *race, unsigned car_index,
                             unsigned lane, unsigned cell)
{
    for (unsigned i = 0; i < race->car_count; ++i) {
        const RaceCar *car = &race->cars[i];

        if (i == car_index || !car->active)
            continue;
        if (car->lane == lane && race_cell(race, car->progress) == cell)
            return (int)i;
    }
    return -1;
}

static int route_contains(const RouteState *state, unsigned lane, unsigned cell)
{
    for (unsigned i = 0; i <= state->steps; ++i) {
        if (state->lane[i] == lane && state->cell[i] == cell)
            return 1;
    }
    return 0;
}

static int state_precedes(const void *left, const void *right)
{
    const RouteState *a = left;
    const RouteState *b = right;

    if (a->forward != b->forward)
        return a->forward > b->forward ? -1 : 1;
    if (a->lane[a->steps] != b->lane[b->steps])
        return a->lane[a->steps] < b->lane[b->steps] ? -1 : 1;
    if (a->spent != b->spent)
        return a->spent < b->spent ? -1 : 1;
    if (a->steps != b->steps)
        return a->steps < b->steps ? -1 : 1;
    return 0;
}

static int route_is_better(const RouteState *candidate, const RouteState *best)
{
    return state_precedes(candidate, best) < 0;
}

static int find_first_forward(const Race *race, unsigned car_index,
                              unsigned limit, RouteState *state,
                              RaceRoute *route)
{
    unsigned current_lane = state->lane[state->steps];
    unsigned current_cell = state->cell[state->steps];
    unsigned target_cell = (current_cell + 1) % race->config.cells;
    int blocker = occupied_by_other(race, car_index, current_lane, target_cell);

    if (blocker >= 0) {
        if (route->blocked_by < 0) {
            route->blocked_by = blocker;
            route->blocked_lane = current_lane;
            route->blocked_cell = target_cell;
        }
    } else if (state->spent < limit &&
               !route_contains(state, current_lane, target_cell)) {
        ++state->steps;
        state->lane[state->steps] = current_lane;
        state->cell[state->steps] = target_cell;
        ++state->forward;
        ++state->spent;
        memcpy(route->lane, state->lane,
               (state->steps + 1) * sizeof(route->lane[0]));
        memcpy(route->cell, state->cell,
               (state->steps + 1) * sizeof(route->cell[0]));
        route->steps = state->steps;
        route->forward = state->forward;
        route->spent = state->spent;
        return 1;
    }

    for (unsigned direction = 0; direction < 2; ++direction) {
        unsigned next_lane;
        unsigned cost;
        int occupied;

        if (direction == 0) {
            if (current_lane == 0)
                continue;
            next_lane = current_lane - 1;
            cost = 1;
        } else {
            if (current_lane + 1 >= race->config.lanes)
                continue;
            next_lane = current_lane + 1;
            cost = 3;
        }
        if (state->spent + cost > limit ||
            route_contains(state, next_lane, current_cell))
            continue;
        occupied = occupied_by_other(race, car_index, next_lane, current_cell);
        if (occupied >= 0) {
            if (route->blocked_by < 0) {
                route->blocked_by = occupied;
                route->blocked_lane = next_lane;
                route->blocked_cell = current_cell;
            }
            continue;
        }
        ++state->steps;
        state->lane[state->steps] = next_lane;
        state->cell[state->steps] = current_cell;
        state->spent += cost;
        if (find_first_forward(race, car_index, limit, state, route))
            return 1;
        state->spent -= cost;
        --state->steps;
    }
    return 0;
}

int race_find_route(const Race *race, unsigned car_index, unsigned limit,
                    RaceRoute *route)
{
    RouteState frontier[ROUTE_BEAM_WIDTH];
    RouteState candidates[ROUTE_CANDIDATES];
    RouteState best;
    unsigned frontier_count = 1;
    unsigned max_steps = limit;
    int found = 0;

    if (car_index >= race->car_count || route == NULL)
        return 0;

    memset(route, 0, sizeof(*route));
    route->limit = limit;
    route->blocked_by = -1;
    memset(&frontier[0], 0, sizeof(frontier[0]));
    frontier[0].lane[0] = race->cars[car_index].lane;
    frontier[0].cell[0] = race_cell(race, race->cars[car_index].progress);

    if (max_steps > RACE_MAX_ROUTE_STEPS)
        max_steps = RACE_MAX_ROUTE_STEPS;
    if (max_steps > race->config.lanes * race->config.cells)
        max_steps = race->config.lanes * race->config.cells;

    for (unsigned depth = 0; depth < max_steps && frontier_count > 0; ++depth) {
        unsigned candidate_count = 0;

        for (unsigned i = 0; i < frontier_count; ++i) {
            const RouteState *state = &frontier[i];
            unsigned current_lane = state->lane[state->steps];
            unsigned current_cell = state->cell[state->steps];
            unsigned next_lane[3];
            unsigned next_cell[3];
            unsigned move_cost[3];
            unsigned choices = 0;
            unsigned forward_cell = (current_cell + 1) % race->config.cells;
            int blocker = occupied_by_other(race, car_index, current_lane, forward_cell);

            if (blocker >= 0) {
                if (route->blocked_by < 0) {
                    route->blocked_by = blocker;
                    route->blocked_lane = current_lane;
                    route->blocked_cell = forward_cell;
                }
            } else {
                next_lane[choices] = current_lane;
                next_cell[choices] = forward_cell;
                move_cost[choices++] = 1;
            }

            if (current_lane > 0) {
                next_lane[choices] = current_lane - 1;
                next_cell[choices] = current_cell;
                move_cost[choices++] = 1;
            }
            if (current_lane + 1 < race->config.lanes) {
                next_lane[choices] = current_lane + 1;
                next_cell[choices] = current_cell;
                move_cost[choices++] = 3;
            }

            for (unsigned j = 0; j < choices; ++j) {
                RouteState candidate;
                int occupied;

                if (state->spent + move_cost[j] > limit)
                    continue;
                if (route_contains(state, next_lane[j], next_cell[j]))
                    continue;
                occupied = occupied_by_other(race, car_index, next_lane[j], next_cell[j]);
                if (occupied >= 0) {
                    if (route->blocked_by < 0) {
                        route->blocked_by = occupied;
                        route->blocked_lane = next_lane[j];
                        route->blocked_cell = next_cell[j];
                    }
                    continue;
                }
                if (candidate_count >= ROUTE_CANDIDATES)
                    continue;

                candidate = *state;
                ++candidate.steps;
                candidate.lane[candidate.steps] = next_lane[j];
                candidate.cell[candidate.steps] = next_cell[j];
                candidate.spent += move_cost[j];
                if (next_lane[j] == current_lane) {
                    ++candidate.forward;
                }
                if (candidate.forward > 0 &&
                    (!found || route_is_better(&candidate, &best))) {
                    best = candidate;
                    found = 1;
                }
                candidates[candidate_count++] = candidate;
            }
        }

        if (candidate_count == 0)
            break;
        qsort(candidates, candidate_count, sizeof(candidates[0]), state_precedes);
        frontier_count = candidate_count < ROUTE_BEAM_WIDTH
            ? candidate_count
            : ROUTE_BEAM_WIDTH;
        memcpy(frontier, candidates, frontier_count * sizeof(frontier[0]));
    }

    if (!found) {
        RouteState state;

        memset(&state, 0, sizeof(state));
        state.lane[0] = race->cars[car_index].lane;
        state.cell[0] = race_cell(race, race->cars[car_index].progress);
        if (!find_first_forward(race, car_index, limit, &state, route))
            return 0;
        return 1;
    }
    memcpy(route->lane, best.lane, (best.steps + 1) * sizeof(route->lane[0]));
    memcpy(route->cell, best.cell, (best.steps + 1) * sizeof(route->cell[0]));
    route->steps = best.steps;
    route->forward = best.forward;
    route->spent = best.spent;
    return 1;
}

static int car_precedes(const RaceCar *a, const RaceCar *b)
{
    if (a->progress != b->progress)
        return a->progress > b->progress;
    if (a->lane != b->lane)
        return a->lane < b->lane;
    return a->start_order < b->start_order;
}

static void ordered_active(const Race *race, unsigned *order, unsigned *count)
{
    *count = 0;
    for (unsigned i = 0; i < race->car_count; ++i) {
        if (!race->cars[i].active)
            continue;
        unsigned at = *count;
        while (at > 0 &&
               car_precedes(&race->cars[i], &race->cars[order[at - 1]])) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = i;
        ++*count;
    }
}

static unsigned completed_laps(int64_t progress, unsigned cells)
{
    if (progress < 0)
        return 0;
    return (unsigned)(progress / (int64_t)cells);
}

static void emit_blocked(const Race *race, const RaceCar *car,
                         const RaceRoute *route)
{
    if (route->blocked_by >= 0) {
        emit_event(race,
                   "BLOCKED round=%u car=%u at=%u:%u by=%u",
                   race->rounds, car->id, route->blocked_lane,
                   route->blocked_cell, race->cars[route->blocked_by].id);
    }
}

static void emit_positions(const Race *race)
{
    unsigned order[RACE_MAX_CARS];
    unsigned count;

    for (unsigned i = 0; i < race->car_count; ++i)
        order[i] = i;
    for (unsigned i = 1; i < race->car_count; ++i) {
        unsigned value = order[i];
        unsigned j = i;

        while (j > 0 && car_precedes(&race->cars[value],
                                    &race->cars[order[j - 1]])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }
    count = race->car_count;

    for (unsigned rank = 0; rank < count; ++rank) {
        const RaceCar *car = &race->cars[order[rank]];
        const char *status = car->finished ? "FINISH" :
            car->retired ? "RETIRED" : "RUNNING";

        emit_event(race,
                   "POSITION round=%u rank=%u car=%u team=%u vehicle=%u "
                   "status=%s progress=%lld lane=%u cell=%u points=%llu",
                   race->rounds, rank + 1, car->id, car->team, car->vehicle,
                   status, (long long)car->progress, car->lane,
                   race_cell(race, car->progress),
                   (unsigned long long)car->points);
    }
}

static void emit_final(Race *race)
{
    unsigned order[RACE_MAX_CARS];

    for (unsigned i = 0; i < race->car_count; ++i)
        order[i] = i;
    for (unsigned i = 1; i < race->car_count; ++i) {
        unsigned value = order[i];
        unsigned j = i;

        while (j > 0) {
            const RaceCar *a = &race->cars[value];
            const RaceCar *b = &race->cars[order[j - 1]];
            int precedes;

            if (race->config.points_classification && a->points != b->points)
                precedes = a->points > b->points;
            else if (a->finished != b->finished)
                precedes = a->finished;
            else if (a->finished && a->finish_round != b->finish_round)
                precedes = a->finish_round < b->finish_round;
            else if (a->finished && a->finish_order != b->finish_order)
                precedes = a->finish_order < b->finish_order;
            else if (a->progress != b->progress)
                precedes = a->progress > b->progress;
            else if (a->lane != b->lane)
                precedes = a->lane < b->lane;
            else
                precedes = a->start_order < b->start_order;

            if (!precedes)
                break;
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }

    emit_event(race, "CLASSIFICATION cars=%u rounds=%u",
               race->car_count, race->rounds);
    for (unsigned rank = 0; rank < race->car_count; ++rank) {
        RaceCar *car = &race->cars[order[rank]];
        const char *status = car->finished ? "FINISH" : "RETIRED";

        car->final_rank = rank + 1;
        emit_event(race,
                   "FINAL rank=%u car=%u team=%u vehicle=%u result=%s "
                   "progress=%lld lane=%u cell=%u points=%llu",
                   rank + 1, car->id, car->team, car->vehicle, status,
                   (long long)car->progress, car->lane,
                   race_cell(race, car->progress),
                   (unsigned long long)car->points);
    }
}

void race_start(Race *race)
{
    unsigned order[RACE_MAX_CARS];

    if (race->state != RACE_WAITING)
        return;
    race->state = RACE_RUNNING;
    emit_event(race,
               "RACE_START lanes=%u cells=%u laps=%u teams=%u cars=%u seed=%llu",
               race->config.lanes, race->config.cells, race->config.laps,
               race->config.teams, race->car_count,
               (unsigned long long)race->config.seed);
    for (unsigned i = 0; i < race->car_count; ++i)
        order[i] = i;
    for (unsigned i = 1; i < race->car_count; ++i) {
        unsigned value = order[i];
        unsigned j = i;

        while (j > 0 &&
               race->cars[order[j - 1]].start_order >
                   race->cars[value].start_order) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }
    for (unsigned i = 0; i < race->car_count; ++i) {
        const RaceCar *car = &race->cars[order[i]];

        emit_event(race,
                   "DRAW order=%u car=%u team=%u vehicle=%u lane=%u cell=%u",
                   i + 1, car->id, car->team, car->vehicle, car->lane,
                   race_cell(race, car->progress));
    }
}

static unsigned move_car(Race *race, unsigned car_index, unsigned turn_index,
                         unsigned previous_forward)
{
    RaceCar *car = &race->cars[car_index];
    unsigned limit;
    const char *kind;
    RaceRoute route;
    char path[1024];
    size_t used = 0;

    if (turn_index == 0) {
        limit = random_bounded(race, 4) + 1;
        kind = "leader-choice";
    } else {
        limit = previous_forward + (turn_index == 1 ? 2 : 1);
        kind = "following-cap";
    }
    emit_event(race, "PACE round=%u car=%u kind=%s limit=%u previous=%u",
               race->rounds, car->id, kind, limit, previous_forward);

    if (!race_find_route(race, car_index, limit, &route)) {
        emit_blocked(race, car, &route);
        car->zero_pending = 1;
        emit_event(race, "ZERO round=%u car=%u limit=%u",
                   race->rounds, car->id, limit);
        return 0;
    }

    emit_blocked(race, car, &route);
    for (unsigned i = 0; i <= route.steps; ++i) {
        int written = snprintf(path + used, sizeof(path) - used,
                               "%s%u:%u", i == 0 ? "" : ">", route.lane[i],
                               route.cell[i]);

        if (written < 0 || (size_t)written >= sizeof(path) - used)
            break;
        used += (size_t)written;
    }
    emit_event(race,
               "PATH round=%u car=%u limit=%u used=%u forward=%u route=%s",
               race->rounds, car->id, limit, route.spent, route.forward, path);

    for (unsigned i = 1; i <= route.steps; ++i) {
        if (route.lane[i] != route.lane[i - 1]) {
            emit_event(race,
                       "LANE round=%u car=%u from=%u to=%u cost=%u",
                       race->rounds, car->id, route.lane[i - 1],
                       route.lane[i], route.lane[i] < route.lane[i - 1] ? 1 : 3);
        }
    }

    car->lane = route.lane[route.steps];
    car->progress += route.forward;
    {
        unsigned laps = completed_laps(car->progress, race->config.cells);

        while (car->completed_laps < laps) {
            ++car->completed_laps;
            car->points += race->config.bonus_points;
            emit_event(race, "LAP round=%u car=%u lap=%u",
                       race->rounds, car->id, car->completed_laps);
            emit_event(race,
                       "BONUS round=%u car=%u lap=%u points=+%u total=%llu",
                       race->rounds, car->id, car->completed_laps,
                       race->config.bonus_points,
                       (unsigned long long)car->points);
        }
    }

    if (car->progress >= (int64_t)race->config.laps * race->config.cells) {
        car->active = 0;
        car->finished = 1;
        car->finish_round = race->rounds;
        car->finish_order = ++race->finish_count;
        emit_event(race,
                   "FINISH round=%u order=%u car=%u progress=%lld",
                   race->rounds, car->finish_order, car->id,
                   (long long)car->progress);
    }
    return route.forward;
}

static void retire_car(Race *race, RaceCar *car, const char *reason)
{
    car->active = 0;
    car->retired = 1;
    car->zero_pending = 0;
    emit_event(race,
               "RETIRED round=%u car=%u reason=%s progress=%lld lane=%u cell=%u",
               race->rounds, car->id, reason, (long long)car->progress,
               car->lane, race_cell(race, car->progress));
}

void race_run_round(Race *race)
{
    unsigned order[RACE_MAX_CARS];
    unsigned count;
    unsigned previous_forward = 0;

    if (race->state != RACE_RUNNING)
        return;
    if (race->rounds >= race->config.max_rounds) {
        for (unsigned i = 0; i < race->car_count; ++i) {
            if (race->cars[i].active)
                retire_car(race, &race->cars[i], "round-limit");
        }
        race->state = RACE_FINISHED;
        emit_final(race);
        return;
    }

    ++race->rounds;
    ordered_active(race, order, &count);
    {
        char turn_order[256];
        size_t used = 0;

        for (unsigned i = 0; i < count; ++i) {
            int written = snprintf(turn_order + used, sizeof(turn_order) - used,
                                   "%s%u", i == 0 ? "" : ",",
                                   race->cars[order[i]].id);

            if (written < 0 || (size_t)written >= sizeof(turn_order) - used)
                break;
            used += (size_t)written;
        }
        emit_event(race, "TURN_ORDER round=%u cars=%s",
                   race->rounds, turn_order);
    }

    for (unsigned i = 0; i < count; ++i) {
        RaceCar *car = &race->cars[order[i]];

        if (car->active)
            previous_forward = move_car(race, order[i], i, previous_forward);
        else
            previous_forward = 0;
    }

    for (unsigned i = 0; i < race->car_count; ++i) {
        if (race->cars[i].zero_pending)
            retire_car(race, &race->cars[i], "blocked");
    }

    if (race->config.retire_gap > 0) {
        int64_t leader_progress = INT64_MIN;

        for (unsigned i = 0; i < race->car_count; ++i) {
            if (race->cars[i].active && race->cars[i].progress > leader_progress)
                leader_progress = race->cars[i].progress;
        }
        if (leader_progress != INT64_MIN) {
            for (unsigned i = 0; i < race->car_count; ++i) {
                RaceCar *car = &race->cars[i];

                if (car->active &&
                    leader_progress - car->progress >= race->config.retire_gap)
                    retire_car(race, car, "lap-behind");
            }
        }
    }

    emit_positions(race);
    {
        int any_active = 0;

        for (unsigned i = 0; i < race->car_count; ++i)
            any_active |= race->cars[i].active;
        if (!any_active) {
            race->state = RACE_FINISHED;
            emit_final(race);
        }
    }
}

int race_is_done(const Race *race)
{
    return race->state == RACE_FINISHED;
}
