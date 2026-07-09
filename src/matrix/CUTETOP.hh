/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MATRIX_CUTE_TOP_HH__
#define __MATRIX_CUTE_TOP_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

#include "matrix/LocalMMUModel.hh"
#include "matrix/MRegFile.hh"
#include "matrix/MatrixTE.hh"
#include "matrix/MemoryLoader.hh"
#include "matrix/Scoreboard.hh"
#include "sim/stats.hh"

namespace gem5
{

namespace matrix
{

// Detailed CUTE active state: keep this header lean and direct.
class DetailedCuteBackend : public MatrixBackend
{
  public:
    enum class MicroTaskKind : uint8_t
    {
        AML = 0,
        BML,
        CML,
        Compute,
        Release,
        Count
    };

    enum class ComputeUnitKind : uint8_t
    {
        None = 0,
        ADC,
        BDC,
        MTE,
        CDC,
        Count
    };

    struct TimingConfig
    {
        unsigned localMmuLatencyCycles = 1;
        unsigned localMmuMaxOutstanding = 64;
        unsigned matrixL2FillTableEntries = 4;
        unsigned matrixL2FillBankFifoDepth = 2;
        unsigned matrixReduceWidthBytes = MatrixRegResource::EntryBytes;
        unsigned matrixOutsideDataWidthBytes = 64;
        std::optional<bool> matrixBmlBypassFillTable = std::nullopt;
    };

    explicit DetailedCuteBackend(
        size_t fifo_depth = 8,
        size_t ab_reg_count = MatrixRegFile::DefaultAbRegCount,
        size_t c_reg_count = MatrixRegFile::DefaultCRegCount,
        statistics::Group *stats_parent = nullptr);
    DetailedCuteBackend(size_t fifo_depth,
                        size_t ab_reg_count,
                        size_t c_reg_count,
                        TimingConfig timing_config,
                        statistics::Group *stats_parent = nullptr);

    bool canAccept(const CuteRequest &req) const override;
    void submit(const CuteRequest &req) override;
    bool hasWork() const override
    {
        return !fifo.empty() || !taskEvents.empty() || activeTaskCount() != 0;
    }
    void step() override;

    bool hasCompletion() const override { return !completions.empty(); }
    CuteCompletion popCompletion() override;
    bool hasArchitecturalState() const override
    {
        return regFile.hasAllocatedState();
    }

    void setTimingMemoryAdapter(MatrixTimingMemoryAdapter *adapter)
    {
        timingMemory = adapter;
    }
    void setIssueCallback(std::function<void(uint64_t)> callback)
    {
        issueCallback = callback;
    }
    bool completeTimingMemoryResponse(
        uint32_t source_id, const uint8_t *data = nullptr,
        uint32_t size = 0) override;

  private:
    enum class TaskStage : uint8_t
    {
        Accepted = 0,
        MemReq,
        FillPending,
        RegWrite,
        RegRead,
        StorePending,
        WaitPendingStoreClear,
        TerminalPending
    };

    enum class TaskEventKind : uint8_t
    {
        ReadFinish = 0,
        ComputeReadAFinish,
        ComputeReadBFinish,
        ComputeReadCFinish,
        WriteFinish,
        TerminalCompletion
    };

    enum class LocalMmuResponseResult : uint8_t
    {
        NoMatch,
        Blocked,
        Serviced
    };

    static constexpr size_t ComputeReadAIdx = 0;
    static constexpr size_t ComputeReadBIdx = 1;
    static constexpr size_t ComputeReadCIdx = 2;
    static constexpr size_t ComputeReadCount = 3;

    struct TaskSlot
    {
        DecodedFifoEntry entry = {};
        MicroTaskKind microTaskKind = MicroTaskKind::Release;
        TaskStage stage = TaskStage::Accepted;
        CuteCompletion bufferedCompletion = {};
        bool lsuBeatsEnqueued = false;
        bool lsuLoadFinalized = false;
        unsigned lsuTotalBeats = 0;
        unsigned lsuResponsesReceived = 0;
        unsigned lsuPendingMatrixRegWriteChunks = 0;
        TimingLoadPlan lsuLoadPlan;
        std::vector<uint8_t> lsuLoadBytes;
        TimingStorePlan lsuStorePlan;
        bool lsuStorePlanInitialized = false;
        MatrixTensor bufferedTensor = {};
        bool hasBufferedTensor = false;
        uint64_t issueStep = 0;
    };

    struct CutePhaseStats : public statistics::Group
    {
        CutePhaseStats(statistics::Group *parent);

        statistics::Scalar matrixCuteALoadIssue;
        statistics::Scalar matrixCuteALoadFinish;
        statistics::Scalar matrixCuteALoadIssueToFinishCycles;
        statistics::Scalar matrixCuteALoadIssueToFinishCyclesMax;
        statistics::Scalar matrixCuteBLoadIssue;
        statistics::Scalar matrixCuteBLoadFinish;
        statistics::Scalar matrixCuteBLoadIssueToFinishCycles;
        statistics::Scalar matrixCuteBLoadIssueToFinishCyclesMax;
        statistics::Scalar matrixCuteCLoadIssue;
        statistics::Scalar matrixCuteCLoadFinish;
        statistics::Scalar matrixCuteCLoadIssueToFinishCycles;
        statistics::Scalar matrixCuteCLoadIssueToFinishCyclesMax;
        statistics::Scalar matrixCuteStoreIssue;
        statistics::Scalar matrixCuteStoreFinish;
        statistics::Scalar matrixCuteStoreIssueToFinishCycles;
        statistics::Scalar matrixCuteStoreIssueToFinishCyclesMax;
        statistics::Scalar matrixCuteMmaIssue;
        statistics::Scalar matrixCuteMmaFinish;
        statistics::Scalar matrixCuteMmaIssueToFinishCycles;
        statistics::Scalar matrixCuteMmaIssueToFinishCyclesMax;
        statistics::Scalar matrixCuteReleaseIssue;
        statistics::Scalar matrixCuteReleaseFinish;
        statistics::Scalar matrixCuteReleaseIssueToFinishCycles;
        statistics::Scalar matrixCuteReleaseIssueToFinishCyclesMax;
    };

    struct ComputeTaskState
    {
        DecodedFifoEntry entry = {};
        CuteCompletion bufferedCompletion = {};
        MatrixTensor bufferedTensor = {};
        std::array<MatrixTensor, ComputeReadCount> readTensors = {};
        bool hasBufferedTensor = false;
        std::array<bool, ComputeReadCount> readTensorValid = {};
        std::array<bool, ComputeReadCount> readIssued = {};
        std::array<bool, ComputeReadCount> readComplete = {};
        ComputeUnitKind activeUnit = ComputeUnitKind::None;
        bool unitWorkDone = false;
        unsigned executeCyclesRemaining = 0;
        unsigned cdcWritebackBeatsTotal = 0;
        unsigned cdcWritebackBeatsRemaining = 0;
        uint64_t issueStep = 0;
        uint64_t unitIssueStep = 0;
        bool terminalIssued = false;
    };

    struct TaskEvent
    {
        DecodedFifoEntry entry = {};
        MicroTaskKind microTaskKind = MicroTaskKind::Release;
        TaskEventKind kind = TaskEventKind::TerminalCompletion;
        CuteCompletion completion = {};
        uint64_t issueStep = 0;
        uint64_t readyStep = 0;
    };

    struct PendingLocalMmuResponse
    {
        LocalMmuModel::Response response = {};
        unsigned bypassWriteChunksDone = 0;
        bool timingLoadRecorded = false;
    };

    bool issuePathReady(const DecodedFifoEntry &entry) const;
    bool computeUnitBusy(ComputeUnitKind kind) const;
    bool computeUnitAvailable(ComputeUnitKind kind) const;
    bool releaseReady() const
    {
        if (pendingStoreCount != 0) {
            return false;
        }
        return !amlTask.has_value() &&
               !bmlTask.has_value() &&
               !cmlTask.has_value() &&
               computeTasks.empty() &&
               taskEvents.empty();
    }
    bool headReady(const DecodedFifoEntry &entry,
                   DetailedCuteScoreboard::BlockReason &reason) const;
    void issueHead(const DecodedFifoEntry &entry);
    void processFifoHead();
    void advanceTaskSlots();
    void advanceTaskSlot(std::optional<TaskSlot> &slot);
    void advanceComputeTask();
    void serviceLocalMmuResponses();
    void issueLocalMmuTimingRequest();
    bool useTimingMemory() const;
    void serviceActiveComputeUnits();
    void dispatchReadyComputeUnits();
    void advanceLoadFill(TaskSlot &task);
    void advanceStoreRead(TaskSlot &task);
    size_t lsuPayloadBytes(const AmuLsuDesc &desc) const;
    void initializeTimingLoadBuffer(TaskSlot &task);
    void initializeTimingStoreBuffer(TaskSlot &task);
    bool recordTimingLoadResponse(
        TaskSlot &task, const LocalMmuModel::Response &response);
    bool buildTensorFromTimingLoadData(TaskSlot &task);
    LocalMmuModel::Client localMmuClient(const TaskSlot &task) const;
    void releaseLocalMmuSource(const LocalMmuModel::Response &response);
    unsigned matrixL2FillChunksForResponse(uint32_t byte_size) const;
    bool bmlBypassFillTableEnabled() const;
    bool useBmlBypassForResponse(
        const TaskSlot &task, const LocalMmuModel::Response &response) const;
    LocalMmuResponseResult serviceLocalMmuResponse(
        std::optional<TaskSlot> &slot, PendingLocalMmuResponse &pending);
    LocalMmuResponseResult serviceLocalMmuReadResponse(
        TaskSlot &task, PendingLocalMmuResponse &pending);
    LocalMmuResponseResult serviceBmlBypassResponse(
        TaskSlot &task, PendingLocalMmuResponse &pending,
        unsigned fill_chunks);
    LocalMmuResponseResult serviceFillTableResponse(
        TaskSlot &task, const LocalMmuModel::Response &response,
        unsigned fill_chunks);
    LocalMmuResponseResult finishLocalMmuStoreAck(
        TaskSlot &task, const LocalMmuModel::Response &response);
    void traceLocalMmuResponse(
        const TaskSlot &task, const LocalMmuModel::Response &response) const;
    bool enqueueLocalMmuBeats(TaskSlot &task);
    void serviceLsuMatrixRegWriteChunks();
    bool noteLsuMatrixRegWriteDrain(
        const MatrixL2FillTable::DrainCandidate &candidate);
    bool issueComputeReadFrontend(ComputeTaskState &task);
    void advanceComputeRead(ComputeTaskState &task, size_t read_idx);
    void advanceComputeExecute(ComputeTaskState &task);
    void advanceComputeWriteback(ComputeTaskState &task);
    void enqueueTaskEvent(const DecodedFifoEntry &entry,
                          MicroTaskKind micro_task_kind,
                          TaskEventKind event_kind,
                          uint64_t issue_step,
                          CuteCompletion completion = {});
    void enqueueTaskEvent(const TaskSlot &task, TaskEventKind kind,
                          CuteCompletion completion = {});
    void enqueueTaskEvent(const ComputeTaskState &task, TaskEventKind kind,
                          CuteCompletion completion = {});
    void traceTaskEvent(const TaskEvent &event) const;
    void applyWriteFinish(const DecodedFifoEntry &entry,
                          const CuteCompletion &completion,
                          MicroTaskKind kind);
    void retireTaskSlot(MicroTaskKind kind, uint64_t seq);
    void retireComputeTask(uint64_t seq);
    uint64_t finalizeCompletion(const CuteCompletion &completion,
                                uint64_t issueStep,
                                uint64_t activeCount);
    void processTaskEvents();
    void dispatchTask(const DecodedFifoEntry &entry);
    CuteCompletion executeTaskSlot(const TaskSlot &task);
    CuteCompletion executeLoadWrite(TaskSlot &task);
    CuteCompletion executeStoreWrite(const TaskSlot &task);
    CuteCompletion executeComputeWrite(ComputeTaskState &task);
    MteTiming computeMteTiming(const AmuMmaDesc &desc) const;
    unsigned computeExecuteLatency(const DecodedFifoEntry &entry) const;
    bool computeDatatypeSupported(const AmuMmaDesc &desc) const;
    void finishTaskSlot(std::optional<TaskSlot> &slot);
    void beginComputeUnit(ComputeTaskState &task, ComputeUnitKind kind);
    CuteCompletion executeArith(uint64_t seq, const AmuArithDesc &desc,
                                MatrixRegFile &state);
    void traceComputeUnitFinish(const ComputeTaskState &task,
                                ComputeUnitKind kind) const;
    void finishComputeUnit(ComputeTaskState &task, ComputeUnitKind kind,
                           bool mark_work_done = false);
    void enqueueComputeCompletion(ComputeTaskState &task,
                                  CuteCompletion completion);
    void finishComputeTerminal(ComputeTaskState &task,
                               CuteCompletion completion,
                               bool grant_all_cdc_beats = false,
                               bool record_cdc_finish = true);
    bool isALoad(const DecodedFifoEntry &entry) const;
    bool isBLoad(const DecodedFifoEntry &entry) const;
    bool isCLoad(const DecodedFifoEntry &entry) const;
    void recordCutePhaseIssue(const DecodedFifoEntry &entry);
    void recordCutePhaseCompletion(const TaskEvent &event, uint64_t latency);
    void recordCutePhaseLatency(statistics::Scalar &total,
                                statistics::Scalar &maximum,
                                uint64_t latency);
    MicroTaskKind microTaskKindForEntry(const DecodedFifoEntry &entry) const;
    bool useMemoryBudget();
    MatrixBankKind destBank(const DecodedFifoEntry &entry) const;
    size_t activeTaskCount() const;

  private:
    DecodedFifo fifo;
    MatrixRegFile regFile;
    DetailedCuteScoreboard scoreboard;
    std::optional<TaskSlot> amlTask;
    std::optional<TaskSlot> bmlTask;
    std::optional<TaskSlot> cmlTask;
    std::deque<ComputeTaskState> computeTasks;
    std::optional<TaskSlot> releaseTask;
    std::deque<TaskEvent> taskEvents;
    std::deque<CuteCompletion> completions;
    MatrixRegResource matrixRegResource;
    TimingConfig timingConfig;
    LocalMmuModel localMmu;
    MatrixL2FillTable matrixL2FillTable;
    MatrixTimingMemoryAdapter *timingMemory = nullptr;
    std::deque<PendingLocalMmuResponse> pendingLocalMmuResponses;
    unsigned pendingStoreCount = 0;
    unsigned memoryBudget = 1;
    uint64_t backendStep = 0;
    std::function<void(uint64_t)> issueCallback;
    CutePhaseStats cutePhaseStats;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_CUTE_TOP_HH__
