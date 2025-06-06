/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#include "query_language.h"

#include <limits.h>
#include <ctype.h>
#include "rmutil/alloc.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"
#include "config.h"

#define stringify(x) stringify2(x)
#define stringify2(x) #x

#define QUERY_TOKEN_SIZE 9
static const char *QUERY_TOKENS[] = {
    "WITHLABELS", "AGGREGATION",     "LIMIT",        "GROUPBY", "REDUCE",
    "FILTER",     "FILTER_BY_VALUE", "FILTER_BY_TS", "COUNT",
};

static int parseTimestamp(RedisModuleString *string, timestamp_t *out) {
    long long int timestamp_raw = 0;
    if (RedisModule_StringToLongLong(string, &timestamp_raw) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    } else if (timestamp_raw < 0) {
        return REDISMODULE_ERR;
    }
    *out = timestamp_raw;
    return REDISMODULE_OK;
}

int parseLabelsFromArgs(RedisModuleString **argv, int argc, size_t *label_count, Label **labels) {
    int pos = RMUtil_ArgIndex("LABELS", argv, argc);
    int first_label_pos = pos + 1;
    Label *labelsResult = NULL;
    *label_count = 0;
    if (pos < 0) {
        *labels = NULL;
        return REDISMODULE_OK;
    }
    *label_count = (size_t)(max(0, (argc - first_label_pos) / 2));
    if (*label_count > 0) {
        labelsResult = calloc((*label_count), sizeof(Label));
        for (int i = 0; i < *label_count; i++) {
            RedisModuleString *key = argv[first_label_pos + i * 2];
            RedisModuleString *value = argv[first_label_pos + i * 2 + 1];

            // Verify Label Key or Value are not empty strings
            size_t keyLen, valueLen;
            RedisModule_StringPtrLen(key, &keyLen);
            RedisModule_StringPtrLen(value, &valueLen);
            if (keyLen == 0 || valueLen == 0 ||
                strpbrk(RedisModule_StringPtrLen(value, NULL), "(),")) {
                FreeLabels(labelsResult, i); // need to release prior key values too
                return REDISMODULE_ERR;
            }

            labelsResult[i].key = RedisModule_CreateStringFromString(NULL, key);
            labelsResult[i].value = RedisModule_CreateStringFromString(NULL, value);
        };
    }
    *labels = labelsResult;
    return REDISMODULE_OK;
}

bool ValidateChunkSizeSimple(long long chunkSizeBytes) {
    if (chunkSizeBytes < CHUNK_SIZE_BYTES_MIN || chunkSizeBytes > CHUNK_SIZE_BYTES_MAX ||
        chunkSizeBytes % 8 != 0) {
        return false;
    }

    return true;
}

bool ValidateChunkSize(RedisModuleCtx *ctx, long long chunkSizeBytes, RedisModuleString **err) {
    if (ValidateChunkSizeSimple(chunkSizeBytes)) {
        return true;
    }

    if (!err) {
        RTS_ReplyGeneralError(
            ctx,
            "TSDB: CHUNK_SIZE value must be a multiple of 8 in the range [" stringify(
                CHUNK_SIZE_BYTES_MIN) " .. " stringify(CHUNK_SIZE_BYTES_MAX) "]");
    } else {
        const char *errorMessage =
            "TSDB: CHUNK_SIZE value must be a multiple of 8 in the range [" stringify(
                CHUNK_SIZE_BYTES_MIN) " .. " stringify(CHUNK_SIZE_BYTES_MAX) "]";
        *err = RedisModule_CreateString(NULL, errorMessage, strlen(errorMessage));
    }

    return false;
}

int ParseChunkSize(RedisModuleCtx *ctx,
                   RedisModuleString **argv,
                   int argc,
                   const char *arg_prefix,
                   long long *chunkSizeBytes,
                   bool *found) {
    if (RMUtil_ArgIndex(arg_prefix, argv, argc) >= 0) {
        if (found) {
            *found = true;
        }

        if (RMUtil_ParseArgsAfter(arg_prefix, argv, argc, "l", chunkSizeBytes) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse CHUNK_SIZE");
            return TSDB_ERROR;
        }

        if (!ValidateChunkSize(ctx, *chunkSizeBytes, NULL)) {
            return TSDB_ERROR;
        }
    }

    return TSDB_OK;
}

int ParseDuplicatePolicy(RedisModuleCtx *ctx,
                         RedisModuleString **argv,
                         int argc,
                         const char *arg_prefix,
                         DuplicatePolicy *policy,
                         bool *found) {
    RedisModuleString *duplicationPolicyInput = NULL;
    if (RMUtil_ArgIndex(arg_prefix, argv, argc) != -1) {
        if (found) {
            *found = true;
        }

        if (RMUtil_ParseArgsAfter(arg_prefix, argv, argc, "s", &duplicationPolicyInput) !=
            REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse DUPLICATE_POLICY");
            return TSDB_ERROR;
        }

        DuplicatePolicy parsePolicy = RMStringLenDuplicationPolicyToEnum(duplicationPolicyInput);
        if (parsePolicy == DP_INVALID) {
            RTS_ReplyGeneralError(ctx, "TSDB: Unknown DUPLICATE_POLICY");
            return TSDB_ERROR;
        }
        *policy = parsePolicy;
        return TSDB_OK;
    }
    return TSDB_OK;
}

int parseIgnoreArgs(RedisModuleCtx *ctx,
                    RedisModuleString **argv,
                    int argc,
                    long long int *ignoreMaxTimeDiff,
                    double *ignoreMaxValDiff) {
    int idx = RMUtil_ArgIndex("IGNORE", argv, argc);
    if (idx > 0) {
        long long maxTimeDiff;
        double maxValDiff;

        if (idx + 2 >= argc ||
            RMUtil_ParseArgs(argv, argc, idx + 1, "ld", &maxTimeDiff, &maxValDiff) !=
                REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse IGNORE");
            return TSDB_ERROR;
        }

        if (maxTimeDiff < 0 || maxValDiff < 0) {
            RTS_ReplyGeneralError(ctx, "TSDB: IGNORE arguments cannot be negative");
            return TSDB_ERROR;
        }

        *ignoreMaxTimeDiff = maxTimeDiff;
        *ignoreMaxValDiff = maxValDiff;
    }

    return TSDB_OK;
}

int parseCreateArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, CreateCtx *cCtx) {
    cCtx->retentionTime = TSGlobalConfig.retentionPolicy;
    cCtx->chunkSizeBytes = TSGlobalConfig.chunkSizeBytes;
    cCtx->labelsCount = 0;
    cCtx->labels = NULL;
    if (parseLabelsFromArgs(argv, argc, &cCtx->labelsCount, &cCtx->labels) == REDISMODULE_ERR) {
        RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse LABELS");
        goto err_exit;
    }

    if (RMUtil_ArgIndex("RETENTION", argv, argc) > 0 &&
        RMUtil_ParseArgsAfter("RETENTION", argv, argc, "l", &cCtx->retentionTime) !=
            REDISMODULE_OK) {
        RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse RETENTION");
        goto err_exit;
    }

    if (cCtx->retentionTime < 0) {
        RedisModule_ReplyWithError(ctx, "TSDB: Couldn't parse RETENTION");
        goto err_exit;
    }

    if (ParseChunkSize(ctx, argv, argc, "CHUNK_SIZE", &cCtx->chunkSizeBytes, NULL) != TSDB_OK) {
        goto err_exit;
    }

    if (parseEncodingArgs(ctx, argv, argc, &cCtx->options) != TSDB_OK) {
        goto err_exit;
    }

    cCtx->duplicatePolicy = TSGlobalConfig.duplicatePolicy;
    if (ParseDuplicatePolicy(ctx, argv, argc, DUPLICATE_POLICY_ARG, &cCtx->duplicatePolicy, NULL) !=
        TSDB_OK) {
        goto err_exit;
    }

    cCtx->ignoreMaxTimeDiff = TSGlobalConfig.ignoreMaxTimeDiff;
    cCtx->ignoreMaxValDiff = TSGlobalConfig.ignoreMaxValDiff;
    if (parseIgnoreArgs(ctx, argv, argc, &cCtx->ignoreMaxTimeDiff, &cCtx->ignoreMaxValDiff) !=
        TSDB_OK) {
        goto err_exit;
    }

    return REDISMODULE_OK;
err_exit:
    if (cCtx->labelsCount > 0 && cCtx->labels != NULL) {
        FreeLabels(cCtx->labels, cCtx->labelsCount);
    }
    return REDISMODULE_ERR;
}

int parseEncodingArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *options) {
    int encoding_location = RMUtil_ArgIndex("ENCODING", argv, argc);
    if (encoding_location > 0) {
        if (encoding_location + 1 >= argc) {
            RedisModule_WrongArity(ctx);
            return TSDB_ERROR;
        }

        const char *encoding = RedisModule_StringPtrLen(argv[encoding_location + 1], NULL);
        if (strcasecmp(encoding, UNCOMPRESSED_ARG_STR) == 0) {
            *options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            *options |= SERIES_OPT_UNCOMPRESSED;
            return TSDB_OK;
        } else if (strcasecmp(encoding, COMPRESSED_GORILLA_ARG_STR) == 0) {
            *options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            *options |= SERIES_OPT_COMPRESSED_GORILLA;
            return TSDB_OK;
        } else {
            RTS_ReplyGeneralError(ctx, "TSDB: unknown ENCODING parameter");
            return TSDB_ERROR;
        }
    } else {
        // backwards compatible UNCOMPRESSED/COMPRESSED parsing
        if (RMUtil_ArgIndex(UNCOMPRESSED_ARG_STR, argv, argc) > 0) {
            *options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            *options |= SERIES_OPT_UNCOMPRESSED;
        }
        if (RMUtil_ArgIndex(COMPRESSED_GORILLA_ARG_STR, argv, argc) > 0) {
            *options &= ~SERIES_OPT_DEFAULT_COMPRESSION;
            *options |= SERIES_OPT_COMPRESSED_GORILLA;
        }
    }
    return TSDB_OK;
}

static int _parseBucketTS(RedisModuleCtx *ctx,
                          RedisModuleString **argv,
                          int argc,
                          BucketTimestamp *bucketTS,
                          int AggregationOffset) {
    *bucketTS = BucketStartTimestamp;
    int bucketTS_offset = RMUtil_ArgIndex("BUCKETTIMESTAMP", argv, argc);
    if (bucketTS_offset > 0) {
        if (bucketTS_offset != AggregationOffset + 3 && bucketTS_offset != AggregationOffset + 4) {
            RTS_ReplyGeneralError(
                ctx,
                "TSDB: BUCKETTIMESTAMP flag should be the 3rd or 4th flag after AGGREGATION flag");
            return TSDB_ERROR;
        }

        if (bucketTS_offset + 1 >= argc) {
            RedisModule_WrongArity(ctx);
            return TSDB_ERROR;
        }

        const char *bucketTSStr = RedisModule_StringPtrLen(argv[bucketTS_offset + 1], NULL);
        if (strcasecmp(bucketTSStr, "start") == 0 || strcasecmp(bucketTSStr, "-") == 0) {
            *bucketTS = BucketStartTimestamp;
            return TSDB_OK;
        } else if (strcasecmp(bucketTSStr, "end") == 0 || strcasecmp(bucketTSStr, "+") == 0) {
            *bucketTS = BucketEndTimestamp;
            return TSDB_OK;
        } else if (strcasecmp(bucketTSStr, "mid") == 0 || strcasecmp(bucketTSStr, "~") == 0) {
            *bucketTS = BucketMidTimestamp;
            return TSDB_OK;
        } else {
            RTS_ReplyGeneralError(ctx, "TSDB: unknown BUCKETTIMESTAMP parameter");
            return TSDB_ERROR;
        }
    }

    return TSDB_OK;
}

static int _parseAlignmentTS(RedisModuleCtx *ctx,
                             RedisModuleString **argv,
                             int argc,
                             timestamp_t *alignmentTS,
                             int AggregationOffset) {
    *alignmentTS = 0; // the default alignment is 0
    if (argc == 7) {
        int alignmentTS_offset = AggregationOffset + 3;

        if (parseTimestamp(argv[alignmentTS_offset], alignmentTS) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse alignTimestamp");
            return TSDB_ERROR;
        }

        if ((int64_t)(*alignmentTS) < 0) {
            RTS_ReplyGeneralError(ctx, "TSDB: alignTimestamp should be greater or equal to 0");
            return TSDB_ERROR;
        }
    }

    return TSDB_OK;
}

int _parseAggregationArgs(RedisModuleCtx *ctx,
                          RedisModuleString **argv,
                          int argc,
                          api_timestamp_t *time_delta,
                          int *agg_type,
                          bool *empty,
                          BucketTimestamp *bucketTS,
                          timestamp_t *alignmetTS) {
    RedisModuleString *aggTypeStr = NULL;
    int offset = RMUtil_ArgIndex("AGGREGATION", argv, argc);
    if (offset > 0) {
        long long temp_time_delta = 0;
        if (RMUtil_ParseArgs(argv, argc, offset + 1, "sl", &aggTypeStr, &temp_time_delta) !=
            REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse AGGREGATION");
            return TSDB_ERROR;
        }

        if (!aggTypeStr) {
            RTS_ReplyGeneralError(ctx, "TSDB: Unknown aggregation type");
            return TSDB_ERROR;
        }

        *agg_type = RMStringLenAggTypeToEnum(aggTypeStr);

        if (*agg_type < 0 || *agg_type >= TS_AGG_TYPES_MAX) {
            RTS_ReplyGeneralError(ctx, "TSDB: Unknown aggregation type");
            return TSDB_ERROR;
        }

        if (temp_time_delta <= 0) {
            RTS_ReplyGeneralError(ctx, "TSDB: bucketDuration must be greater than zero");
            return TSDB_ERROR;
        } else {
            *time_delta = (api_timestamp_t)temp_time_delta;
        }

        if (empty) {
            int empty_offset = RMUtil_ArgIndex("EMPTY", argv, argc);
            if (empty_offset > 0) {
                if (empty_offset != offset + 3 && empty_offset != offset + 5) {
                    RTS_ReplyGeneralError(
                        ctx,
                        "TSDB: EMPTY flag should be the 3rd or 5th flag after AGGREGATION flag");
                    return TSDB_ERROR;
                }
                RedisModuleString *emptyStr = NULL;
                if (RMUtil_ParseArgs(argv, argc, empty_offset, "s", &emptyStr) != REDISMODULE_OK) {
                    RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse EMPTY flag");
                    return TSDB_ERROR;
                }
                *empty = false;
                if (emptyStr && !RMStringStrCmpUpper(emptyStr, "EMPTY")) {
                    *empty = true;
                }
            }
        }

        if (alignmetTS) {
            if (_parseAlignmentTS(ctx, argv, argc, alignmetTS, offset) != TSDB_OK) {
                return TSDB_ERROR;
            }
        }

        if (bucketTS) {
            if (_parseBucketTS(ctx, argv, argc, bucketTS, offset) != TSDB_OK) {
                return TSDB_ERROR;
            }
        }

        return TSDB_OK;
    }

    return TSDB_NOTEXISTS;
}

int parseAggregationArgs(RedisModuleCtx *ctx,
                         RedisModuleString **argv,
                         int argc,
                         AggregationArgs *out) {
    int agg_type;
    AggregationArgs aggregationArgs = { 0 };
    int result = _parseAggregationArgs(ctx,
                                       argv,
                                       argc,
                                       &aggregationArgs.timeDelta,
                                       &agg_type,
                                       &aggregationArgs.empty,
                                       &aggregationArgs.bucketTS,
                                       NULL);
    if (result == TSDB_OK) {
        aggregationArgs.aggregationClass = GetAggClass(agg_type);
        if (aggregationArgs.aggregationClass == NULL) {
            RTS_ReplyGeneralError(ctx, "TSDB: Failed to retrieve aggregation class");
            return TSDB_ERROR;
        }
        *out = aggregationArgs;
        return TSDB_OK;
    } else {
        return result;
    }
}

static int parseCountArgument(RedisModuleCtx *ctx,
                              RedisModuleString **argv,
                              int argc,
                              long long *count) {
    int offset = RMUtil_ArgIndex("COUNT", argv, argc);
    if (offset > 0) {
        int reduce_offset = RMUtil_ArgIndex("REDUCE", argv, argc);
        int agg_offset = RMUtil_ArgIndex("AGGREGATION", argv, argc);
        if (agg_offset > 0 && offset == agg_offset + 1) {
            offset = RMUtil_ArgIndex("COUNT", argv + agg_offset + 2, argc - agg_offset - 2);
            if (offset < 0) {
                // In this case the count was aggregation type
                return TSDB_OK;
            }
            offset += agg_offset + 2;
        }

        if (reduce_offset > 0 && offset == reduce_offset + 1) {
            offset = RMUtil_ArgIndex("COUNT", argv + reduce_offset + 2, argc - reduce_offset - 2);
            if (offset < 0) {
                // In this case the count was REDUCE type
                return TSDB_OK;
            }
            offset += reduce_offset + 2;
        }

        if (offset + 1 == argc) {
            RTS_ReplyGeneralError(ctx, "TSDB: COUNT argument is missing");
            return TSDB_ERROR;
        }
        if (RedisModule_StringToLongLong(argv[offset + 1], count) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse COUNT");
            return TSDB_ERROR;
        }

        if (*count < 1) {
            RTS_ReplyGeneralError(ctx, "TSDB: Invalid COUNT value");
            return TSDB_ERROR;
        }
    }
    return TSDB_OK;
}

static int parseAlignmentArgs(RedisModuleCtx *ctx,
                              RedisModuleString **argv,
                              int argc,
                              RangeAlignment *alignment,
                              timestamp_t *timestamp) {
    *alignment = DefaultAlignment;
    int align_location = RMUtil_ArgIndex("ALIGN", argv, argc);
    if (align_location > 0) {
        if (align_location + 1 >= argc) {
            RedisModule_WrongArity(ctx);
            return TSDB_ERROR;
        }

        const char *aligment = RedisModule_StringPtrLen(argv[align_location + 1], NULL);
        if (strcasecmp(aligment, "start") == 0 || strcasecmp(aligment, "-") == 0) {
            *alignment = StartAlignment;
            return TSDB_OK;
        } else if (strcasecmp(aligment, "end") == 0 || strcasecmp(aligment, "+") == 0) {
            *alignment = EndAlignment;
            return TSDB_OK;
        } else if (parseTimestamp(argv[align_location + 1], timestamp) == REDISMODULE_OK) {
            *alignment = TimestampAlignment;
            return TSDB_OK;
        } else {
            RTS_ReplyGeneralError(ctx, "TSDB: unknown ALIGN parameter");
            return TSDB_ERROR;
        }
    }
    return TSDB_OK;
}

static int parseFilterByValueArgument(RedisModuleCtx *ctx,
                                      RedisModuleString **argv,
                                      int argc,
                                      FilterByValueArgs *args) {
    int offset = RMUtil_ArgIndex("FILTER_BY_VALUE", argv, argc);
    if (offset > 0) {
        if (offset + 2 >= argc) {
            RTS_ReplyGeneralError(ctx, "TSDB: FILTER_BY_VALUE one or more arguments are missing");
            return TSDB_ERROR;
        }

        if (RedisModule_StringToDouble(argv[offset + 1], &args->min) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse MIN");
            return TSDB_ERROR;
        }

        if (RedisModule_StringToDouble(argv[offset + 2], &args->max) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: Couldn't parse MAX");
            return TSDB_ERROR;
        }
        args->hasValue = true;
    }
    return TSDB_OK;
}

int comp_uint64(const void *a, const void *b) {
    if (*((uint64_t *)a) > *((uint64_t *)b))
        return (+1);
    if (*((uint64_t *)a) < *((uint64_t *)b))
        return (-1);
    return (0);
}

// assumption: values sorted array
// returns the new size
static size_t values_remove_duplicates(timestamp_t *values, size_t size) {
    size_t cur = 0, i;
    for (i = cur + 1; i < size; ++i) {
        if (values[cur] != values[i]) {
            values[++cur] = values[i];
        }
    }

    return cur + 1;
}

static int parseFilterByTimestamp(RedisModuleCtx *ctx,
                                  RedisModuleString **argv,
                                  int argc,
                                  FilterByTSArgs *args) {
    int offset = RMUtil_ArgIndex("FILTER_BY_TS", argv, argc);
    size_t index = 0;
    if (offset > 0) {
        if (offset + 1 == argc) {
            RTS_ReplyGeneralError(ctx, "TSDB: FILTER_BY_TS one or more arguments are missing");
            return TSDB_ERROR;
        }

        while (offset + 1 < argc && index < MAX_TS_VALUES_FILTER) {
            timestamp_t val;
            if (parseTimestamp(argv[offset + 1], &val) == REDISMODULE_OK) {
                args->values[index] = val;
                index++;
                offset++;
            } else {
                // TODO check if the token is a keywork in our query lang or raise an error
                break;
            }
        }
        if (index == 0) {
            RTS_ReplyGeneralError(ctx, "TSDB: FILTER_BY_TS one or more arguments are missing");
            return TSDB_ERROR;
        }

        // We sort the provided timestamps in order to improve query time filtering
        qsort(args->values, index, sizeof(uint64_t), comp_uint64);
        index = values_remove_duplicates(args->values, index);

        args->hasValue = (index > 0);
        args->count = index;
    }
    return TSDB_OK;
}

int parseLatestArg(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, bool *latest) {
    int offset = RMUtil_ArgIndex("LATEST", argv, argc);
    if (offset > 0) {
        *latest = true;
    } else {
        *latest = false;
    }

    return REDISMODULE_OK;
}

int parseRangeArguments(RedisModuleCtx *ctx,
                        int start_index,
                        RedisModuleString **argv,
                        int argc,
                        RangeArgs *out) {
    RangeArgs args = { 0 };
    args.aggregationArgs.timeDelta = 0;
    args.aggregationArgs.aggregationClass = NULL;
    args.filterByValueArgs.hasValue = false;
    args.filterByTSArgs.hasValue = false;

    bool startTimestampMin = false;
    bool endTimestampMax = false;
    size_t start_len;
    const char *start = RedisModule_StringPtrLen(argv[start_index], &start_len);
    if (strcmp(start, "-") == 0) {
        args.startTimestamp = 0;
        startTimestampMin = true;
    } else {
        if (parseTimestamp(argv[start_index], &args.startTimestamp) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: wrong fromTimestamp");
            return REDISMODULE_ERR;
        }
    }

    size_t end_len;
    const char *end = RedisModule_StringPtrLen(argv[start_index + 1], &end_len);
    if (strcmp(end, "+") == 0) {
        args.endTimestamp = LLONG_MAX;
        endTimestampMax = true;
    } else {
        if (parseTimestamp(argv[start_index + 1], &args.endTimestamp) != REDISMODULE_OK) {
            RTS_ReplyGeneralError(ctx, "TSDB: wrong toTimestamp");
            return REDISMODULE_ERR;
        }
    }

    if (parseLatestArg(ctx, argv, argc, &args.latest) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    args.count = -1;
    if (parseCountArgument(ctx, argv, argc, &args.count) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    if (parseAggregationArgs(ctx, argv, argc, &args.aggregationArgs) == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    if (parseAlignmentArgs(ctx, argv, argc, &args.alignment, &args.timestampAlignment) ==
        TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    if (args.alignment != DefaultAlignment) {
        if (args.aggregationArgs.aggregationClass == NULL) {
            RTS_ReplyGeneralError(ctx, "TSDB: ALIGN parameter can only be used with AGGREGATION");
            return TSDB_ERROR;
        }

        if (args.alignment == StartAlignment && startTimestampMin) {
            RTS_ReplyGeneralError(
                ctx, "TSDB: start alignment can only be used with explicit start timestamp");
            return TSDB_ERROR;
        }

        if (args.alignment == EndAlignment && endTimestampMax) {
            RTS_ReplyGeneralError(
                ctx, "TSDB: end alignment can only be used with explicit end timestamp");
            return TSDB_ERROR;
        }
    }

    if (parseFilterByValueArgument(ctx, argv, argc, &args.filterByValueArgs) == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    if (parseFilterByTimestamp(ctx, argv, argc, &args.filterByTSArgs) == TSDB_ERROR) {
        return REDISMODULE_ERR;
    }

    *out = args;

    return REDISMODULE_OK;
}

QueryPredicateList *parseLabelListFromArgs(RedisModuleCtx *ctx,
                                           RedisModuleString **argv,
                                           int start,
                                           int query_count,
                                           int *response) {
    QueryPredicateList *queries = malloc(sizeof(QueryPredicateList));
    queries->count = query_count;
    queries->ref = 1;
    queries->list = calloc(queries->count, sizeof(QueryPredicate));
    memset(queries->list, 0, queries->count * sizeof(QueryPredicate));
    int current_index = 0;
    *response = TSDB_OK;

    for (int i = start; i < start + query_count; i++) {
        size_t label_value_pair_size;
        QueryPredicate *query = &queries->list[current_index];
        const char *label_value_pair = RedisModule_StringPtrLen(argv[i], &label_value_pair_size);
        // l!=(v1,v2,...) key with label l that doesn't equal any of the values in the list
        // Note: order is important! Must be before "!=".
        if (strstr(label_value_pair, "!=(") != NULL) {
            query->type = LIST_NOTMATCH;
            if (parsePredicate(ctx, label_value_pair, label_value_pair_size, query, "!=(") ==
                TSDB_ERROR) {
                *response = TSDB_ERROR;
                break;
            }
            // l!= key has label l
        } else if (strstr(label_value_pair, "!=") != NULL) {
            query->type = NEQ;
            if (parsePredicate(ctx, label_value_pair, label_value_pair_size, query, "!=") ==
                TSDB_ERROR) {
                *response = TSDB_ERROR;
                break;
            }
            if (query->valueListCount == 0) {
                query->type = CONTAINS;
            }
            // l=(v1,v2,...) key with label l that equals one of the values in the list
            // Note: order is important! Must be before "=".
        } else if (strstr(label_value_pair, "=(") != NULL) {
            query->type = LIST_MATCH;
            if (parsePredicate(ctx, label_value_pair, label_value_pair_size, query, "=(") ==
                TSDB_ERROR) {
                *response = TSDB_ERROR;
                break;
            }
            // When we reach this check, it's due to:
            // option 1) l=v label equals value
            // option 2) l= key does not have the label l
        } else if (strstr(label_value_pair, "=") != NULL) {
            query->type = EQ;
            // option 1) l=v label equals value
            if (parsePredicate(ctx, label_value_pair, label_value_pair_size, query, "=") ==
                TSDB_ERROR) {
                *response = TSDB_ERROR;
                break;
            }
            // option 2) l= key does not have the label l
            if (query->valueListCount == 0) {
                query->type = NCONTAINS;
            }
        } else {
            *response = TSDB_ERROR;
            break;
        }
        current_index++;
    }
    return queries;
}

int parseMultiSeriesReduceArgs(RedisModuleCtx *ctx,
                               RedisModuleString *reducerstr,
                               ReducerArgs *reducerArgs) {
    TS_AGG_TYPES_T agg_type = RMStringLenAggTypeToEnum(reducerstr);
    if (agg_type == TS_AGG_FIRST || agg_type == TS_AGG_LAST || agg_type == TS_AGG_TWA ||
        agg_type == TS_AGG_INVALID || agg_type == TS_AGG_NONE) {
        RTS_ReplyGeneralError(ctx, "TSDB: Invalid reducer type");
        return TSDB_ERROR;
    }
    reducerArgs->aggregationClass = GetAggClass(agg_type);
    if (reducerArgs->aggregationClass == NULL) {
        RTS_ReplyGeneralError(ctx, "TSDB: Failed to retrieve reducer class");
        return TSDB_ERROR;
    }
    reducerArgs->agg_type = agg_type;
    return TSDB_OK;
}

int parseLabelQuery(RedisModuleCtx *ctx,
                    RedisModuleString **argv,
                    int argc,
                    bool *withLabels,
                    RedisModuleString **limitLabels,
                    unsigned short *limitLabelsSize) {
    *withLabels = RMUtil_ArgIndex("WITHLABELS", argv, argc) > 0;
    const int limit_location = RMUtil_ArgIndex("SELECTED_LABELS", argv, argc);
    if (limit_location > 0 && *withLabels) {
        RTS_ReplyGeneralError(ctx, "TSDB: cannot accept WITHLABELS and SELECT_LABELS together");
        return REDISMODULE_ERR;
    }

    if (limit_location > 0) {
        size_t count = 0;
        for (int i = limit_location + 1; i < argc; i++) {
            size_t len;
            const char *c_str = RedisModule_StringPtrLen(argv[i], &len);
            bool found = false;
            for (int j = 0; j < QUERY_TOKEN_SIZE; ++j) {
                if (strcasecmp(QUERY_TOKENS[j], c_str) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
            if (count >= LIMIT_LABELS_SIZE) {
                RTS_ReplyGeneralError(ctx, "TSDB: reached max size for SELECT_LABELS");
                return REDISMODULE_ERR;
            }
            limitLabels[count] = argv[i];
            count++;
        }
        if (count == 0) {
            RTS_ReplyGeneralError(ctx, "TSDB: SELECT_LABELS should have at least 1 parameter");
            return REDISMODULE_ERR;
        }
        *limitLabelsSize = count;
    }
    return REDISMODULE_OK;
}

int parseFilter(RedisModuleCtx *ctx,
                RedisModuleString **argv,
                int argc,
                int filter_location,
                int query_count,
                QueryPredicateList **out) {
    int response;
    QueryPredicateList *queries = NULL;

    queries = parseLabelListFromArgs(ctx, argv, filter_location + 1, query_count, &response);
    if (response == TSDB_ERROR) {
        QueryPredicateList_Free(queries);
        RTS_ReplyGeneralError(ctx, "TSDB: failed parsing labels");
        return REDISMODULE_ERR;
    }

    if (CountPredicateType(queries, EQ) + CountPredicateType(queries, LIST_MATCH) == 0) {
        QueryPredicateList_Free(queries);
        RTS_ReplyGeneralError(ctx, "TSDB: please provide at least one matcher");
        return REDISMODULE_ERR;
    }
    *out = queries;
    return REDISMODULE_OK;
}

int parseMGetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, MGetArgs *out) {
    MGetArgs args = { 0 };
    if (argc < 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    if (parseLatestArg(ctx, argv, argc, &args.latest) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    int filter_location = RMUtil_ArgIndex("FILTER", argv, argc);
    if (filter_location == -1) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }
    size_t query_count = argc - 1 - filter_location;

    if (parseLabelQuery(
            ctx, argv, argc, &args.withLabels, args.limitLabels, &args.numLimitLabels) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    QueryPredicateList *queries;
    if (parseFilter(ctx, argv, argc, filter_location, query_count, &queries) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    args.queryPredicates = queries;
    *out = args;
    return REDISMODULE_OK;
}

int parseMRangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, MRangeArgs *out) {
    if (argc < 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    MRangeArgs args;
    args.groupByLabel = NULL;
    args.queryPredicates = NULL;
    args.numLimitLabels = 0;

    if (parseRangeArguments(ctx, 1, argv, argc, &args.rangeArgs) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    const int filter_location = RMUtil_ArgIndex("FILTER", argv, argc);
    if (filter_location == -1) {
        RTS_ReplyGeneralError(ctx, "TSDB: missing FILTER argument");
        return REDISMODULE_ERR;
    }

    if (parseLabelQuery(
            ctx, argv, argc, &args.withLabels, args.limitLabels, &args.numLimitLabels) ==
        REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    const int groupby_location = RMUtil_ArgIndex("GROUPBY", argv, argc);

    if (groupby_location > 0 && groupby_location < filter_location) {
        RTS_ReplyGeneralError(ctx, "TSDB: GROUPBY should always come after filter");
        return REDISMODULE_ERR;
    }

    // If we have GROUPBY <label> REDUCE <reducer> then labels arguments
    // are only up to (GROUPBY pos) - 1.
    const size_t last_filter_pos = groupby_location > 0 ? groupby_location - 1 : argc - 1;
    const size_t query_count = last_filter_pos - filter_location;

    if (query_count == 0) {
        RTS_ReplyGeneralError(ctx, "TSDB: missing labels for filter argument");
        return REDISMODULE_ERR;
    }

    QueryPredicateList *queries = NULL;
    if (parseFilter(ctx, argv, argc, filter_location, query_count, &queries) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    args.queryPredicates = queries;

    if (groupby_location > 0) {
        if (groupby_location + 1 >= argc) {
            // GROUP BY without any argument
            RedisModule_WrongArity(ctx);
            QueryPredicateList_Free(queries);
            return REDISMODULE_ERR;
        }
        args.groupByLabel = RedisModule_StringPtrLen(argv[groupby_location + 1], NULL);

        const int reduce_location = RMUtil_ArgIndex("REDUCE", argv, argc);
        // If we've detected a groupby but not a reduce
        // or we've detected a groupby by the total args don't match
        if (reduce_location < 0 || (argc - groupby_location != 4)) {
            RedisModule_WrongArity(ctx);
            QueryPredicateList_Free(queries);
            return REDISMODULE_ERR;
        }
        if (parseMultiSeriesReduceArgs(ctx, argv[reduce_location + 1], &args.gropuByReducerArgs) !=
            TSDB_OK) {
            QueryPredicateList_Free(queries);
            return REDISMODULE_ERR;
        }
    }
    *out = args;
    return REDISMODULE_OK;
}

void MRangeArgs_Free(MRangeArgs *args) {
    QueryPredicateList_Free(args->queryPredicates);
}

void MGetArgs_Free(MGetArgs *args) {
    QueryPredicateList_Free(args->queryPredicates);
}
