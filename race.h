#ifndef RACE_H
#define RACE_H

#include <stddef.h>
#include <stdint.h>

#define RACE_MAX_LANES 8
#define RACE_MAX_CELLS 512
#define RACE_MAX_TEAMS 8
#define RACE_MAX_CARS (RACE_MAX_TEAMS * 2)
#define RACE_MAX_ROUTE_STEPS 64

typedef struct {
    unsigned lanes;
    unsigned cells;
    unsigned laps;
    unsigned teams;
    unsigned bonus_points;
    unsigned retire_gap;
    unsigned max_rounds;
    int points_classification;
    uint64_t seed;
} RaceConfig;

typedef struct {
    unsigned id;
    unsigned team;
    unsigned vehicle;
    unsigned start_order;
    unsigned lane;
    int64_t progress;
    unsigned completed_laps;
    uint64_t points;
    unsigned finish_round;
    unsigned finish_order;
    unsigned final_rank;
    int active;
    int finished;
    int retired;
    int zero_pending;
} RaceCar;

typedef struct {
    unsigned lane[RACE_MAX_ROUTE_STEPS + 1];
    unsigned cell[RACE_MAX_ROUTE_STEPS + 1];
    unsigned steps;
    unsigned forward;
    unsigned spent;
    unsigned limit;
    int blocked_by;
    unsigned blocked_lane;
    unsigned blocked_cell;
} RaceRoute;

typedef void (*RaceEventFn)(void *context, const char *event);

typedef enum {
    RACE_WAITING,
    RACE_RUNNING,
    RACE_FINISHED
} RaceState;

typedef struct {
    RaceConfig config;
    RaceCar cars[RACE_MAX_CARS];
    unsigned car_count;
    unsigned rounds;
    unsigned finish_count;
    uint64_t rng;
    RaceState state;
    RaceEventFn event_fn;
    void *event_context;
} Race;

void race_config_defaults(RaceConfig *config);
int race_config_validate(const RaceConfig *config, char *error, size_t error_size);
int race_init(Race *race, const RaceConfig *config, RaceEventFn event_fn,
              void *event_context, char *error, size_t error_size);
void race_start(Race *race);
void race_run_round(Race *race);
int race_is_done(const Race *race);
int race_find_route(const Race *race, unsigned car_index, unsigned limit,
                    RaceRoute *route);
unsigned race_cell(const Race *race, int64_t progress);

#endif
