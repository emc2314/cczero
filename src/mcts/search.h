/*
  This file is part of Chinese Chess Zero.
  Copyright (C) 2018 The CCZero Authors

  Chinese Chess Zero is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Chinese Chess Zero is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Chinese Chess Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <functional>
#include <shared_mutex>
#include <thread>

#include "chess/callbacks.h"
#include "chess/uciloop.h"
#include "mcts/node.h"
#include "neural/cache.h"
#include "neural/network.h"
#include "utils/mutex.h"
#include "utils/optional.h"
#include "utils/optionsdict.h"
#include "utils/optionsparser.h"

namespace cczero {

struct SearchLimits {
    std::int64_t visits = -1;
    std::int64_t playouts = -1;
    std::int64_t time_ms = -1;
    bool infinite = false;
    MoveList searchmoves;
};

class Search {
   public:
    Search(const NodeTree& tree, Network* network,
           BestMoveInfo::Callback best_move_callback,
           ThinkingInfo::Callback info_callback, const SearchLimits& limits,
           const OptionsDict& options, NNCache* cache);

    ~Search();

    // Populates UciOptions with search parameters.
    static void PopulateUciParams(OptionsParser* options);

    // Starts worker threads and returns immediately.
    void StartThreads(size_t how_many);

    // Starts search with k threads and wait until it finishes.
    void RunBlocking(size_t threads);

    // Runs search single-threaded, blocking.
    void RunSingleThreaded();

    // Stops search. At the end bestmove will be returned. The function is not
    // blocking, so it returns before search is actually done.
    void Stop();
    // Stops search, but does not return bestmove. The function is not blocking.
    void Abort();
    // Blocks until all worker thread finish.
    void Wait();

    // Returns best move, from the point of view of white player. And also
    // ponder. May or may not use temperature, according to the settings.
    std::pair<Move, Move> GetBestMove() const;
    // Returns the evaluation of the best move, WITHOUT temperature. This
    // differs from the above function; with temperature enabled, these two
    // functions may return results from different possible moves.
    float GetBestEval() const;

    // Strings for UCI params. So that others can override defaults.
    // TODO(mooskagh) There are too many options for now. Factor out that into a
    // separate class.
    static const char* kMiniBatchSizeStr;
    static const char* kMaxPrefetchBatchStr;
    static const char* kCpuctStr;
    static const char* kTemperatureStr;
    static const char* kTempDecayMovesStr;
    static const char* kNoiseStr;
    static const char* kVerboseStatsStr;
    static const char* kSmartPruningStr;
    static const char* kFpuReductionStr;
    static const char* kCacheHistoryLengthStr;
    static const char* kPolicySoftmaxTempStr;
    static const char* kAllowedNodeCollisionsStr;

   private:
    // Returns the best move, maybe with temperature (according to the
    // settings).
    std::pair<Move, Move> GetBestMoveInternal() const;

    // Returns a child with most visits, with or without temperature.
    // NoTemperature is safe to use on non-extended nodes, while WithTemperature
    // accepts only nodes with at least 1 visited child.
    EdgeAndNode GetBestChildNoTemperature(Node* parent) const;
    EdgeAndNode GetBestChildWithTemperature(Node* parent,
                                            float temperature) const;

    int64_t GetTimeSinceStart() const;
    void UpdateRemainingMoves();
    void MaybeTriggerStop();
    void MaybeOutputInfo();
    void SendUciInfo();  // Requires nodes_mutex_ to be held.

    void SendMovesStats() const;

    // We only need first ply for debug output, but could be easily generalized.
    NNCacheLock GetCachedFirstPlyResult(EdgeAndNode) const;

    mutable Mutex counters_mutex_ ACQUIRED_AFTER(nodes_mutex_);
    // Tells all threads to stop.
    bool stop_ GUARDED_BY(counters_mutex_) = false;
    // There is already one thread that responded bestmove, other threads
    // should not do that.
    bool responded_bestmove_ GUARDED_BY(counters_mutex_) = false;
    // Becomes true when smart pruning decides
    bool found_best_move_ GUARDED_BY(counters_mutex_) = false;
    // Stored so that in the case of non-zero temperature GetBestMove() returns
    // consistent results.
    std::pair<Move, Move> best_move_ GUARDED_BY(counters_mutex_);

    Mutex threads_mutex_;
    std::vector<std::thread> threads_ GUARDED_BY(threads_mutex_);

    Node* root_node_;
    NNCache* cache_;
    // Fixed positions which happened before the search.
    const PositionHistory& played_history_;

    Network* const network_;
    const SearchLimits limits_;
    const std::chrono::steady_clock::time_point start_time_;
    const int64_t initial_visits_;

    mutable SharedMutex nodes_mutex_;
    EdgeAndNode best_move_edge_ GUARDED_BY(nodes_mutex_);
    Edge* last_outputted_best_move_edge_ GUARDED_BY(nodes_mutex_) = nullptr;
    ThinkingInfo uci_info_ GUARDED_BY(nodes_mutex_);
    int64_t total_playouts_ GUARDED_BY(nodes_mutex_) = 0;
    int remaining_playouts_ GUARDED_BY(nodes_mutex_) =
        std::numeric_limits<int>::max();

    BestMoveInfo::Callback best_move_callback_;
    ThinkingInfo::Callback info_callback_;

    // External parameters.
    const int kMiniBatchSize;
    const int kMaxPrefetchBatch;
    const float kCpuct;
    const float kTemperature;
    const int kTempDecayMoves;
    const bool kNoise;
    const bool kVerboseStats;
    const bool kSmartPruning;
    const float kFpuReduction;
    const bool kCacheHistoryLength;
    const float kPolicySoftmaxTemp;
    const int kAllowedNodeCollisions;

    friend class SearchWorker;
};

// Single thread worker of the search engine.
// That used to be just a function Search::Worker(), but to parallelize it
// within one thread, have to split into stages.
class SearchWorker {
   public:
    SearchWorker(Search* search)
        : search_(search), history_(search_->played_history_) {}

    // Runs iterations while needed.
    void RunBlocking() {
        while (IsSearchActive()) {
            ExecuteOneIteration();
        }
    }

    // Does one full iteration of MCTS search:
    // 1. Initialize internal structures.
    // 2. Gather minibatch.
    // 3. Prefetch into cache.
    // 4. Run NN computation.
    // 5. Retrieve NN computations (and terminal values) into nodes.
    // 6. Propagate the new nodes' information to all their parents in the tree.
    // 7. Update the Search's status and progress information.
    void ExecuteOneIteration();

    // Returns whether another search iteration is needed (false means exit).
    bool IsSearchActive() const;

    // The same operations one by one:
    // 1. Initialize internal structures.
    // @computation is the computation to use on this iteration.
    void InitializeIteration(std::unique_ptr<NetworkComputation> computation);

    // 2. Gather minibatch.
    void GatherMinibatch();

    // 3. Prefetch into cache.
    void MaybePrefetchIntoCache();

    // 4. Run NN computation.
    void RunNNComputation();

    // 5. Retrieve NN computations (and terminal values) into nodes.
    void FetchMinibatchResults();

    // 6. Propagate the new nodes' information to all their parents in the tree.
    void DoBackupUpdate();

    // 7. Update the Search's status and progress information.
    void UpdateCounters();

   private:
    struct NodeToProcess {
        NodeToProcess(Node* node, bool is_collision)
            : node(node), is_collision(is_collision) {}
        Node* node;
        bool is_collision = false;
        bool nn_queried = false;
        // Value from NN's value head, or -1/0/1 for terminal nodes.
        float v;
    };

    NodeToProcess PickNodeToExtend();
    void ExtendNode(Node* node);
    bool AddNodeToComputation(Node* node, bool add_if_cached = true);
    int PrefetchIntoCache(Node* node, int budget);

    Search* const search_;
    std::vector<NodeToProcess> nodes_to_process_;
    std::unique_ptr<CachingComputation> computation_;
    // History is reset and extended by PickNodeToExtend().
    PositionHistory history_;
};

}  // namespace cczero
