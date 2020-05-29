//
// Created by Borelset on 2020/5/27.
//
//  Copyright (C) 2020-present, Xiangyu Zou. All rights reserved.
//  This source code is licensed under the GPLv2

#ifndef MFDEDUP_RESTOREPARSERPIPELINE_H
#define MFDEDUP_RESTOREPARSERPIPELINE_H

#include "RestoreWritePipeline.h"
#include "../Utility/StorageTask.h"
#include "../Utility/FileOperator.h"
#include <thread>
#include <assert.h>

DEFINE_uint64(RestoreReadBufferLength,
              67108864, "WriteBufferLength");

class RestoreParserPipeline {
public:
    RestoreParserPipeline(uint64_t target, const std::string &path) : taskAmount(0), runningFlag(true), mutexLock(),
                                                                      condition(mutexLock) {
        worker = new std::thread(std::bind(&RestoreParserPipeline::restoreParserCallback, this, path));
    }

    int addTask(RestoreParseTask *restoreParseTask) {
        MutexLockGuard mutexLockGuard(mutexLock);
        taskList.push_back(restoreParseTask);
        taskAmount++;
        condition.notify();
    }

    ~RestoreParserPipeline() {
        runningFlag = false;
        condition.notifyAll();
        worker->join();
    }

private:
    void restoreParserCallback(const std::string &path) {

        FileOperator recipeFD((char *) path.data(), FileOpenType::Read);
        uint64_t size = FileOperator::size(path);
        uint8_t *recipeBuffer = (uint8_t *) malloc(size);
        recipeFD.read(recipeBuffer, size);
        uint64_t count = size / sizeof(BlockHeader);
        assert(count * sizeof(BlockHeader) == size);
        BlockHeader *blockHeader;

        uint64_t pos = 0;
        for (uint64_t i = 0; i < count; i++) {
            blockHeader = (BlockHeader *) (recipeBuffer + i * sizeof(BlockHeader));
            restoreMap[blockHeader->fp].push_back({pos});
            pos += blockHeader->length;
        }
        GlobalRestoreWritePipelinePtr->setSize(pos);

        RestoreParseTask *restoreParseTask;
        uint64_t leftLength = 0;
        uint64_t copyLength = 0;
        uint8_t *chunkPtr;

        uint8_t *temp = (uint8_t *) malloc(FLAGS_RestoreReadBufferLength);

        while (likely(runningFlag)) {
            {
                MutexLockGuard mutexLockGuard(mutexLock);
                while (!taskAmount) {
                    condition.wait();
                    if (unlikely(!runningFlag)) break;
                }
                if (unlikely(!runningFlag)) continue;
                taskAmount--;
                restoreParseTask = taskList.front();
                taskList.pop_front();
            }

            if (unlikely(restoreParseTask->endFlag)) {
                delete restoreParseTask;
                RestoreWriteTask *restoreWriteTask = new RestoreWriteTask(true);
                GlobalRestoreWritePipelinePtr->addTask(restoreWriteTask);
                break;
            }

            uint64_t taskRemain = 0;
            if (leftLength + restoreParseTask->length > FLAGS_RestoreReadBufferLength) {
                copyLength = FLAGS_RestoreReadBufferLength - leftLength;
                taskRemain = leftLength;
            } else {
                copyLength = restoreParseTask->length;
                taskRemain = 0;
            }
            uint64_t parseLeft = copyLength + leftLength;
            memcpy(temp + leftLength, restoreParseTask->buffer, copyLength);

            uint64_t readoffset = 0;

            while (1) {
                blockHeader = (BlockHeader *) (temp + readoffset);
                if (parseLeft < sizeof(BlockHeader) || parseLeft < sizeof(BlockHeader) + blockHeader->length) {
                    memcpy(temp, temp + readoffset, parseLeft);
                    memcpy(temp + parseLeft, restoreParseTask->buffer + copyLength, taskRemain);
                    readoffset = 0;
                    parseLeft += taskRemain;

                    taskRemain = 0;
                    copyLength += taskRemain;
                    blockHeader = (BlockHeader *) (temp + readoffset);

                    if (parseLeft < sizeof(BlockHeader) || parseLeft < sizeof(BlockHeader) + blockHeader->length) {
                        leftLength = parseLeft;
                        break;
                    }
                }
                chunkPtr = temp + readoffset + sizeof(BlockHeader);
                auto iter = restoreMap.find(blockHeader->fp);
                assert(iter->second.size() > 0);
                for (auto item : iter->second) {
                    totalLength += blockHeader->length;
                    RestoreWriteTask *restoreWriteTask = new RestoreWriteTask(chunkPtr, item, blockHeader->length);
                    GlobalRestoreWritePipelinePtr->addTask(restoreWriteTask);
                }
                readoffset += sizeof(BlockHeader) + blockHeader->length;
                parseLeft -= sizeof(BlockHeader) + blockHeader->length;
            }

            delete restoreParseTask;
        }
    }


    char filePath[256];
    bool runningFlag;
    std::thread *worker;
    uint64_t taskAmount;
    std::list<RestoreParseTask *> taskList;
    MutexLock mutexLock;
    Condition condition;

    uint64_t totalLength = 0;

    std::unordered_map<SHA1FP, std::list<uint64_t>, TupleHasher, TupleEqualer> restoreMap;
};

static RestoreParserPipeline *GlobalRestoreParserPipelinePtr;

#endif //MFDEDUP_RESTOREPARSERPIPELINE_H
