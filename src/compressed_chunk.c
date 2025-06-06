/*
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "compressed_chunk.h"
#include "common.h"

#include "LibMR/src/mr.h"
#include "chunk.h"
#include "generic_chunk.h"

#include <assert.h> // assert
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>  // printf
#include <stdlib.h> // malloc
#include "rmutil/alloc.h"

#define BIT 8
#define CHUNK_RESIZE_STEP 32

/*********************
 *  Chunk functions  *
 *********************/
Chunk_t *Compressed_NewChunk(size_t size) {
    _log_if(size % 8 != 0, "chunk size isn't multiplication of 8");
    CompressedChunk *chunk = (CompressedChunk *)calloc(1, sizeof(CompressedChunk));
    chunk->size = size;
    chunk->data = (uint64_t *)calloc(chunk->size, sizeof(char));
#ifdef DEBUG
    memset(chunk->data, 0, chunk->size);
#endif
    chunk->prevLeading = 32;
    chunk->prevTrailing = 32;
    chunk->prevTimestamp = 0;
    return chunk;
}

void Compressed_FreeChunk(Chunk_t *chunk) {
    CompressedChunk *cmpChunk = chunk;
    if (cmpChunk->data) {
        free(cmpChunk->data);
    }
    cmpChunk->data = NULL;
    free(chunk);
}

Chunk_t *Compressed_CloneChunk(const Chunk_t *chunk) {
    const CompressedChunk *oldChunk = chunk;
    CompressedChunk *newChunk = malloc(sizeof(CompressedChunk));
    memcpy(newChunk, oldChunk, sizeof(CompressedChunk));
    newChunk->data = malloc(newChunk->size);
    memcpy(newChunk->data, oldChunk->data, oldChunk->size);
    return newChunk;
}

int Compressed_DefragChunk(RedisModuleDefragCtx *ctx,
                           void *data,
                           __unused unsigned char *key,
                           __unused size_t keylen,
                           void **newptr) {
    CompressedChunk *chunk = data;
    chunk = defragPtr(ctx, chunk);
    chunk->data = defragPtr(ctx, chunk->data);
    *newptr = (void *)chunk;
    return DefragStatus_Finished;
}

static void swapChunks(CompressedChunk *a, CompressedChunk *b) {
    CompressedChunk tmp = *a;
    *a = *b;
    *b = tmp;
}

static void ensureAddSample(CompressedChunk *chunk, Sample *sample) {
    ChunkResult res = Compressed_AddSample(chunk, sample);
    if (res != CR_OK) {
        int oldsize = chunk->size;
        chunk->size += CHUNK_RESIZE_STEP;
        chunk->data = (uint64_t *)realloc(chunk->data, chunk->size * sizeof(char));
        memset((char *)chunk->data + oldsize, 0, CHUNK_RESIZE_STEP);
        // printf("Chunk extended to %lu \n", chunk->size);
        res = Compressed_AddSample(chunk, sample);
        assert(res == CR_OK);
    }
}

static void trimChunk(CompressedChunk *chunk) {
    int excess = (chunk->size * BIT - chunk->idx) / BIT;

    if (unlikely(chunk->size * BIT < chunk->idx)) {
        _log_if(
            true,
            "Invalid chunk index, we have written beyond allocated memorye"); // else we have
                                                                              // written beyond
                                                                              // allocated memory
        return;
    }

    if (excess > 1) {
        size_t newSize = chunk->size - excess + 1;
        // align to 8 bytes (uint64_t) otherwise we will have an heap overflow in gorilla.c because
        // each write happens in 8 bytes blocks.
        newSize += sizeof(binary_t) - (newSize % sizeof(binary_t));
        chunk->data = realloc(chunk->data, newSize);
        chunk->size = newSize;
    }
}

Chunk_t *Compressed_SplitChunk(Chunk_t *chunk) {
    CompressedChunk *curChunk = chunk;
    size_t split = curChunk->count / 2;
    size_t curNumSamples = curChunk->count - split;

    // add samples in new chunks
    size_t i = 0;
    Sample sample;
    ChunkIter_t *iter = Compressed_NewChunkIterator(curChunk);
    CompressedChunk *newChunk1 = Compressed_NewChunk(curChunk->size);
    CompressedChunk *newChunk2 = Compressed_NewChunk(curChunk->size);
    for (; i < curNumSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        ensureAddSample(newChunk1, &sample);
    }
    for (; i < curChunk->count; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
        ensureAddSample(newChunk2, &sample);
    }

    trimChunk(newChunk1);
    trimChunk(newChunk2);
    swapChunks(curChunk, newChunk1);

    Compressed_FreeChunkIterator(iter);
    Compressed_FreeChunk(newChunk1);

    return newChunk2;
}

ChunkResult Compressed_UpsertSample(UpsertCtx *uCtx, int *size, DuplicatePolicy duplicatePolicy) {
    *size = 0;
    ChunkResult rv = CR_OK;
    ChunkResult nextRes = CR_OK;
    CompressedChunk *oldChunk = (CompressedChunk *)uCtx->inChunk;

    size_t newSize = oldChunk->size;

    CompressedChunk *newChunk = Compressed_NewChunk(newSize);
    Compressed_Iterator *iter = Compressed_NewChunkIterator(oldChunk);
    timestamp_t ts = uCtx->sample.timestamp;
    int numSamples = oldChunk->count;

    size_t i = 0;
    Sample iterSample;
    for (; i < numSamples; ++i) {
        nextRes = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        if (iterSample.timestamp >= ts) {
            break;
        }
        ensureAddSample(newChunk, &iterSample);
    }

    if (ts == iterSample.timestamp) {
        ChunkResult cr = handleDuplicateSample(duplicatePolicy, iterSample, &uCtx->sample);
        if (cr != CR_OK) {
            Compressed_FreeChunkIterator(iter);
            Compressed_FreeChunk(newChunk);
            return CR_ERR;
        }
        nextRes = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        *size = -1; // we skipped a sample
    }
    // upsert the sample
    ensureAddSample(newChunk, &uCtx->sample);
    *size += 1;

    if (i < numSamples) {
        while (nextRes == CR_OK) {
            ensureAddSample(newChunk, &iterSample);
            nextRes = Compressed_ChunkIteratorGetNext(iter, &iterSample);
        }
    }

    swapChunks(newChunk, oldChunk);

    Compressed_FreeChunkIterator(iter);
    Compressed_FreeChunk(newChunk);
    return rv;
}

ChunkResult Compressed_AddSample(Chunk_t *chunk, Sample *sample) {
    return Compressed_Append((CompressedChunk *)chunk, sample->timestamp, sample->value);
}

uint64_t Compressed_ChunkNumOfSample(Chunk_t *chunk) {
    return ((CompressedChunk *)chunk)->count;
}

timestamp_t Compressed_GetFirstTimestamp(Chunk_t *chunk) {
    if (((CompressedChunk *)chunk)->count ==
        0) { // When the chunk is empty it first TS is used for the chunk dict key
        return 0;
    }
    return ((CompressedChunk *)chunk)->baseTimestamp;
}

timestamp_t Compressed_GetLastTimestamp(Chunk_t *chunk) {
    if (unlikely(((CompressedChunk *)chunk)->count == 0)) { // empty chunks are being removed
        RedisModule_Log(mr_staticCtx, "error", "Trying to get the last timestamp of empty chunk");
    }
    return ((CompressedChunk *)chunk)->prevTimestamp;
}

double Compressed_GetLastValue(Chunk_t *chunk) {
    if (unlikely(((CompressedChunk *)chunk)->count == 0)) { // empty chunks are being removed
        RedisModule_Log(mr_staticCtx, "error", "Trying to get the last value of empty chunk");
    }
    return ((CompressedChunk *)chunk)->prevValue.d;
}

size_t Compressed_GetChunkSize(const Chunk_t *chunk, bool includeStruct) {
    const CompressedChunk *cmpChunk = chunk;
    size_t size = includeStruct ? RedisModule_MallocSize((void *)cmpChunk) +
                                      RedisModule_MallocSize(cmpChunk->data)
                                : cmpChunk->size;
    return size;
}

size_t Compressed_DelRange(Chunk_t *chunk, timestamp_t startTs, timestamp_t endTs) {
    CompressedChunk *oldChunk = (CompressedChunk *)chunk;
    size_t newSize = oldChunk->size; // mem size
    CompressedChunk *newChunk = Compressed_NewChunk(newSize);
    Compressed_Iterator *iter = Compressed_NewChunkIterator(oldChunk);
    size_t i = 0;
    size_t deleted_count = 0;
    Sample iterSample;
    int numSamples = oldChunk->count; // sample size
    for (; i < numSamples; ++i) {
        Compressed_ChunkIteratorGetNext(iter, &iterSample);
        if (iterSample.timestamp >= startTs && iterSample.timestamp <= endTs) {
            // in delete range, skip adding to the new chunk
            deleted_count++;
            continue;
        }
        ensureAddSample(newChunk, &iterSample);
    }
    swapChunks(newChunk, oldChunk);
    Compressed_FreeChunkIterator(iter);
    Compressed_FreeChunk(newChunk);
    return deleted_count;
}

// TODO: convert to template and unify with decompressChunk when moving to RUST
// decompress chunk reverse
static inline void decompressChunkReverse(const CompressedChunk *compressedChunk,
                                          uint64_t start,
                                          uint64_t end,
                                          EnrichedChunk *enrichedChunk) {
    uint64_t numSamples = compressedChunk->count;
    uint64_t lastTS = compressedChunk->prevTimestamp;
    Sample sample;
    ResetEnrichedChunk(enrichedChunk);
    if (unlikely(numSamples == 0 || end < start || compressedChunk->baseTimestamp > end ||
                 lastTS < start)) {
        return;
    }

    Compressed_Iterator *iter = Compressed_NewChunkIterator(compressedChunk);
    timestamp_t *timestamps_ptr = enrichedChunk->samples.timestamps + numSamples - 1;
    double *values_ptr = enrichedChunk->samples.values + numSamples - 1;

    // find the first sample which is greater than start
    Compressed_ChunkIteratorGetNext(iter, &sample);
    while (sample.timestamp < start && iter->count < numSamples) {
        Compressed_ChunkIteratorGetNext(iter, &sample);
    }

    if (unlikely(sample.timestamp > end)) {
        // occurs when the are TS smaller than start and larger than end but nothing in the range.
        Compressed_FreeChunkIterator(iter);
        return;
    }
    *timestamps_ptr-- = sample.timestamp;
    *values_ptr-- = sample.value;

    if (lastTS > end) { // the range not include the whole chunk
        // 4 samples per iteration
        const size_t n = numSamples >= 4 ? numSamples - 4 : 0;
        while (iter->count < n) {
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr-- = sample.timestamp;
            *values_ptr-- = sample.value;
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr-- = sample.timestamp;
            *values_ptr-- = sample.value;
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr-- = sample.timestamp;
            *values_ptr-- = sample.value;
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr-- = sample.timestamp;
            *values_ptr-- = sample.value;
            if (unlikely(*(timestamps_ptr + 1) > end)) {
                while (*(timestamps_ptr + 1) > end) {
                    ++timestamps_ptr;
                    ++values_ptr;
                }
                goto _done;
            }
        }

        // left-overs
        while (iter->count < numSamples) {
            Compressed_ChunkIteratorGetNext(iter, &sample);
            if (sample.timestamp > end) {
                goto _done;
            }
            *timestamps_ptr-- = sample.timestamp;
            *values_ptr-- = sample.value;
        }
    } else {
        while (iter->count < numSamples) {
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr-- = sample.timestamp;
            *values_ptr-- = sample.value;
        }
    }

_done:
    enrichedChunk->samples.timestamps = timestamps_ptr + 1;
    enrichedChunk->samples.values = values_ptr + 1;
    enrichedChunk->samples.num_samples =
        enrichedChunk->samples.og_timestamps + numSamples - enrichedChunk->samples.timestamps;
    enrichedChunk->rev = true;

    Compressed_FreeChunkIterator(iter);

    return;
}

// decompress chunk
static inline void decompressChunk(const CompressedChunk *compressedChunk,
                                   uint64_t start,
                                   uint64_t end,
                                   EnrichedChunk *enrichedChunk) {
    uint64_t numSamples = compressedChunk->count;
    uint64_t lastTS = compressedChunk->prevTimestamp;
    Sample sample;
    ChunkResult res;
    ResetEnrichedChunk(enrichedChunk);
    if (unlikely(numSamples == 0 || end < start || compressedChunk->baseTimestamp > end ||
                 lastTS < start)) {
        return;
    }

    Compressed_Iterator *iter = Compressed_NewChunkIterator(compressedChunk);
    timestamp_t *timestamps_ptr = enrichedChunk->samples.timestamps;
    double *values_ptr = enrichedChunk->samples.values;

    // find the first sample which is greater than start
    res = Compressed_ChunkIteratorGetNext(iter, &sample);
    while (sample.timestamp < start && res == CR_OK) {
        res = Compressed_ChunkIteratorGetNext(iter, &sample);
    }

    if (unlikely(sample.timestamp > end)) {
        // occurs when the are TS smaller than start and larger than end but nothing in the range.
        Compressed_FreeChunkIterator(iter);
        return;
    }
    *timestamps_ptr++ = sample.timestamp;
    *values_ptr++ = sample.value;

    if (lastTS > end) { // the range not include the whole chunk
        // 4 samples per iteration
        const size_t n = numSamples >= 4 ? numSamples - 4 : 0;
        while (iter->count < n) {
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr++ = sample.timestamp;
            *values_ptr++ = sample.value;
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr++ = sample.timestamp;
            *values_ptr++ = sample.value;
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr++ = sample.timestamp;
            *values_ptr++ = sample.value;
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr++ = sample.timestamp;
            *values_ptr++ = sample.value;
            if (unlikely(*(timestamps_ptr - 1) > end)) {
                while (*(timestamps_ptr - 1) > end) {
                    --timestamps_ptr;
                    --values_ptr;
                }
                goto _done;
            }
        }

        // left-overs
        while (iter->count < numSamples) {
            Compressed_ChunkIteratorGetNext(iter, &sample);
            if (sample.timestamp > end) {
                goto _done;
            }
            *timestamps_ptr++ = sample.timestamp;
            *values_ptr++ = sample.value;
        }
    } else {
        while (iter->count < numSamples) {
            Compressed_ChunkIteratorGetNext(iter, &sample);
            *timestamps_ptr++ = sample.timestamp;
            *values_ptr++ = sample.value;
        }
    }

_done:
    enrichedChunk->samples.num_samples = timestamps_ptr - enrichedChunk->samples.timestamps;

    Compressed_FreeChunkIterator(iter);

    return;
}

/************************
 *  Iterator functions  *
 ************************/
// LCOV_EXCL_START - used for debug
uint64_t getIterIdx(ChunkIter_t *iter) {
    return ((Compressed_Iterator *)iter)->idx;
}
// LCOV_EXCL_STOP

void Compressed_ResetChunkIterator(ChunkIter_t *iterator, const Chunk_t *chunk) {
    const CompressedChunk *compressedChunk = chunk;
    Compressed_Iterator *iter = (Compressed_Iterator *)iterator;
    iter->chunk = (CompressedChunk *)compressedChunk;
    iter->idx = 0;
    iter->count = 0;

    iter->prevDelta = 0;
    iter->prevTS = compressedChunk->baseTimestamp;
    iter->prevValue.d = compressedChunk->baseValue.d;
    iter->leading = 32;
    iter->trailing = 32;
    iter->blocksize = 0;
    iterator = (ChunkIter_t *)iter;
}

ChunkIter_t *Compressed_NewChunkIterator(const Chunk_t *chunk) {
    const CompressedChunk *compressedChunk = chunk;
    Compressed_Iterator *iter = (Compressed_Iterator *)calloc(1, sizeof(Compressed_Iterator));
    Compressed_ResetChunkIterator(iter, compressedChunk);
    return (ChunkIter_t *)iter;
}

void Compressed_FreeChunkIterator(ChunkIter_t *iter) {
    free(iter);
}

void Compressed_ProcessChunk(const Chunk_t *chunk,
                             uint64_t start,
                             uint64_t end,
                             EnrichedChunk *enrichedChunk,
                             bool reverse) {
    if (unlikely(!chunk)) {
        return;
    }
    const CompressedChunk *compressedChunk = chunk;

    if (unlikely(reverse)) {
        decompressChunkReverse(compressedChunk, start, end, enrichedChunk);
    } else {
        decompressChunk(compressedChunk, start, end, enrichedChunk);
    }

    return;
}

typedef void (*SaveUnsignedFunc)(void *, uint64_t);
typedef void (*SaveStringBufferFunc)(void *, const char *str, size_t len);
typedef uint64_t (*ReadUnsignedFunc)(void *);
typedef char *(*ReadStringBufferFunc)(void *, size_t *);

static void Compressed_Serialize(Chunk_t *chunk,
                                 void *ctx,
                                 SaveUnsignedFunc saveUnsigned,
                                 SaveStringBufferFunc saveStringBuffer) {
    CompressedChunk *compchunk = chunk;

    saveUnsigned(ctx, compchunk->size);
    saveUnsigned(ctx, compchunk->count);
    saveUnsigned(ctx, compchunk->idx);
    saveUnsigned(ctx, compchunk->baseValue.u);
    saveUnsigned(ctx, compchunk->baseTimestamp);
    saveUnsigned(ctx, compchunk->prevTimestamp);
    saveUnsigned(ctx, compchunk->prevTimestampDelta);
    saveUnsigned(ctx, compchunk->prevValue.u);
    saveUnsigned(ctx, compchunk->prevLeading);
    saveUnsigned(ctx, compchunk->prevTrailing);
    saveStringBuffer(ctx, (char *)compchunk->data, compchunk->size);
}

void Compressed_SaveToRDB(Chunk_t *chunk, struct RedisModuleIO *io) {
    Compressed_Serialize(chunk,
                         io,
                         (SaveUnsignedFunc)RedisModule_SaveUnsigned,
                         (SaveStringBufferFunc)RedisModule_SaveStringBuffer);
}

int Compressed_LoadFromRDB(Chunk_t **chunk, struct RedisModuleIO *io) {
    bool err = false;
    errdefer(err, *chunk = NULL);

    CompressedChunk *compchunk = (CompressedChunk *)malloc(sizeof(*compchunk));
    errdefer(err, Compressed_FreeChunk(compchunk));

    compchunk->data = NULL;
    compchunk->size = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->count = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->idx = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->baseValue.u = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->baseTimestamp = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->prevTimestamp = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->prevTimestampDelta = (int64_t)LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->prevValue.u = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->prevLeading = LoadUnsigned_IOError(io, err, TSDB_ERROR);
    compchunk->prevTrailing = LoadUnsigned_IOError(io, err, TSDB_ERROR);

    size_t len;
    compchunk->data = (uint64_t *)LoadStringBuffer_IOError(io, &len, err, TSDB_ERROR);
    *chunk = (Chunk_t *)compchunk;

    return TSDB_OK;
}

void Compressed_MRSerialize(Chunk_t *chunk, WriteSerializationCtx *sctx) {
    Compressed_Serialize(chunk,
                         sctx,
                         (SaveUnsignedFunc)MR_SerializationCtxWriteLongLongWrapper,
                         (SaveStringBufferFunc)MR_SerializationCtxWriteBufferWrapper);
}

int Compressed_MRDeserialize(Chunk_t **chunk, ReaderSerializationCtx *sctx) {
    CompressedChunk *compchunk = (CompressedChunk *)malloc(sizeof(*compchunk));

    compchunk->data = NULL;
    compchunk->size = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->count = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->idx = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->baseValue.u = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->baseTimestamp = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->prevTimestamp = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->prevTimestampDelta = (int64_t)MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->prevValue.u = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->prevLeading = MR_SerializationCtxReadLongLongWrapper(sctx);
    compchunk->prevTrailing = MR_SerializationCtxReadLongLongWrapper(sctx);

    size_t len;
    compchunk->data = (uint64_t *)MR_ownedBufferFrom(sctx, &len);
    *chunk = (Chunk_t *)compchunk;
    return TSDB_OK;
}
