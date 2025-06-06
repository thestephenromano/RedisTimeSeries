/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "compaction.h"
#include "config.h"
#include "generic_chunk.h"
#include "indexer.h"

#include "RedisModulesSDK/redismodule.h"
#include "rmutil/alloc.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"

#ifndef REDISTIMESERIES_QUERY_LANGUAGE_H
#define REDISTIMESERIES_QUERY_LANGUAGE_H

typedef enum BucketTimestamp
{
    BucketStartTimestamp = 0,
    BucketMidTimestamp,
    BucketEndTimestamp // 2
} BucketTimestamp;

typedef struct AggregationArgs
{
    bool empty; // Should return empty buckets
    api_timestamp_t timeDelta;
    BucketTimestamp bucketTS;
    AggregationClass *aggregationClass;
} AggregationArgs;

// GroupBy reducer args
typedef struct ReducerArgs
{
    AggregationClass *aggregationClass;
    TS_AGG_TYPES_T agg_type;
} ReducerArgs;

typedef struct FilterByValueArgs
{
    bool hasValue;
    double min;
    double max;
} FilterByValueArgs;

#define MAX_TS_VALUES_FILTER 128

typedef struct FilterByTSArgs
{
    bool hasValue;
    size_t count;
    timestamp_t values[MAX_TS_VALUES_FILTER];
} FilterByTSArgs;

typedef enum RangeAlignment
{
    DefaultAlignment,
    StartAlignment,
    EndAlignment,
    TimestampAlignment
} RangeAlignment;

typedef struct RangeArgs
{
    api_timestamp_t startTimestamp;
    api_timestamp_t endTimestamp;
    bool latest;     // get also the latest unfinalized bucket from the src series
    long long count; // AKA limit
    AggregationArgs aggregationArgs;
    FilterByValueArgs filterByValueArgs;
    FilterByTSArgs filterByTSArgs;
    RangeAlignment alignment;
    timestamp_t timestampAlignment;
} RangeArgs;

#define LIMIT_LABELS_SIZE 50
typedef struct MRangeArgs
{
    RangeArgs rangeArgs;
    bool withLabels;
    unsigned short numLimitLabels;
    RedisModuleString *limitLabels[LIMIT_LABELS_SIZE];
    QueryPredicateList *queryPredicates;
    const char *groupByLabel;
    ReducerArgs gropuByReducerArgs;
    bool reverse;
} MRangeArgs;

typedef struct MGetArgs
{
    bool withLabels;
    unsigned short numLimitLabels;
    RedisModuleString *limitLabels[LIMIT_LABELS_SIZE];
    QueryPredicateList *queryPredicates;
    bool latest;
} MGetArgs;

typedef struct CreateCtx
{
    long long retentionTime;
    long long chunkSizeBytes;
    size_t labelsCount;
    Label *labels;
    int options;
    DuplicatePolicy duplicatePolicy;
    bool skipChunkCreation;
    long long ignoreMaxTimeDiff;
    double ignoreMaxValDiff;
} CreateCtx;

int parseLabelsFromArgs(RedisModuleString **argv, int argc, size_t *label_count, Label **labels);

int ParseChunkSize(RedisModuleCtx *ctx,
                   RedisModuleString **argv,
                   int argc,
                   const char *arg_prefix,
                   long long *chunkSizeBytes,
                   bool *found);

int ParseDuplicatePolicy(RedisModuleCtx *ctx,
                         RedisModuleString **argv,
                         int argc,
                         const char *arg_prefix,
                         DuplicatePolicy *policy,
                         bool *found);

int parseEncodingArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *options);

int parseCreateArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, CreateCtx *cCtx);

int _parseAggregationArgs(RedisModuleCtx *ctx,
                          RedisModuleString **argv,
                          int argc,
                          api_timestamp_t *time_delta,
                          int *agg_type,
                          bool *empty,
                          BucketTimestamp *bucketTS,
                          timestamp_t *alignmetTS);

int parseLabelQuery(RedisModuleCtx *ctx,
                    RedisModuleString **argv,
                    int argc,
                    bool *withLabels,
                    RedisModuleString **limitLabels,
                    unsigned short *limitLabelsSize);

int parseAggregationArgs(RedisModuleCtx *ctx,
                         RedisModuleString **argv,
                         int argc,
                         AggregationArgs *out);

int parseRangeArguments(RedisModuleCtx *ctx,
                        int start_index,
                        RedisModuleString **argv,
                        int argc,
                        RangeArgs *out);

QueryPredicateList *parseLabelListFromArgs(RedisModuleCtx *ctx,
                                           RedisModuleString **argv,
                                           int start,
                                           int query_count,
                                           int *response);

int parseMRangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, MRangeArgs *out);
void MRangeArgs_Free(MRangeArgs *args);

int parseMGetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, MGetArgs *out);
void MGetArgs_Free(MGetArgs *args);
bool ValidateChunkSize(RedisModuleCtx *ctx, long long chunkSizeBytes, RedisModuleString **err);
bool ValidateChunkSizeSimple(long long chunkSizeBytes);
int parseLatestArg(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool *latest);

#endif // REDISTIMESERIES_QUERY_LANGUAGE_H
