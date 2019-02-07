#include "training/graph_group_sync.h"

namespace marian {

SyncGraphGroup::SyncGraphGroup(Ptr<Options> config)
    : GraphGroup(config),
      delay_{options_->get<double>("optimizer-delay")} { // @TODO: rename to something else; delay means delayed updated, not accumulation

  mpi_ = initMPI(/*multiThreaded=*/false); // when not running under MPI, this will be a fake object that represents a one-MPI-process setup

  devices_ = Config::getDevices(options_, mpi_->myMPIRank(), mpi_->numMPIProcesses());
  for(auto device : devices_) {
    auto graph = New<ExpressionGraph>();
    graph->setDevice(device);

    auto precisions = options_->get<std::vector<std::string>>("precision");
    graph->setParameterType(typeFromString(precisions[0]));

    if(options_->get<bool>("check-nan")) // @TODO: add to other places
      graph->setThrowNan(true);

    graph->reserveWorkspaceMB(options_->get<size_t>("workspace"));
    graph->getBackend()->setClip(options_->get<float>("clip-gemm"));

    graphs_.push_back(graph);
    shardOpt_.push_back(Optimizer(options_, graph->allocator()));
    builders_.push_back(models::from_options(options_, models::usage::training));
  }

  // Note: We may well end up with only one MPI process or only one graph per worker.
  // This part of the code will not special-case any of this here.
  // Rather, it is assumed that the communicator knows to reduce unnecessary transfers to no-ops.
  comm_ = createCommunicator(graphs_, /*noNccl=*/options_->get<bool>("no-nccl", false), /*mpi=*/mpi_);

  auto type = utils::toUpper(devices_.front().typeAsString()) + "s";
  if (mpi_->numMPIProcesses() > 1)
    LOG(info, "[training] Using {} {}, distributed over {} MPI processes", mpi_->numMPIProcesses() * devices_.size(), type, mpi_->numMPIProcesses());
  else
    LOG(info, "[training] Using {} {}", devices_.size(), type);
}

void SyncGraphGroup::setScheduler(Ptr<Scheduler> scheduler) /*override*/ {
  validate();
  scheduler_ = scheduler;
  // optimizer has to be registered last to see changes of learning rate
  // @TODO: ^^Fix this comment. Either it refers to the scheduler, or it should be moved. Which one?
  scheduler_->registerTrainingObserver(scheduler_);

  for(auto opt : shardOpt_)
    scheduler_->registerTrainingObserver(opt);
}

void SyncGraphGroup::initialize(const Ptr<data::Batch>& exampleBatch) {
  // Initialize graphs with random weights in one forward step
  // Also allocate and clear the gradients
  comm_->foreach([&](size_t i, size_t /*begin*/, size_t /*end*/) {
    builders_[i]->build(graphs_[i], exampleBatch);
    graphs_[i]->forward();
    graphs_[i]->params()->allocateBackward();
    graphs_[i]->params()->set_zero_adjoint();
    return true; // dummy success
  });

  // Copy weights from 0-th graph to all other graphs
  // to have equal weights across devices
  comm_->foreach([&](size_t i, size_t /*begin*/, size_t /*end*/) {
    if (i > 0)
      graphs_[i]->params()->vals()->copyFrom(graphs_[0]->params()->vals());
    return true; // dummy success
  });
  //ThreadPool pool(graphs_.size() - 1, graphs_.size() - 1);
  //for(size_t i = 1; i < graphs_.size(); ++i) {
  //  auto init = [&](size_t i) {
  //    // initialize i-th graph and weights
  //    builders_[i]->build(graphs_[i], exampleBatch);
  //    graphs_[i]->forward();
  //    // overwrite weights of i-th graph with weights from 0-th graph
  //    graphs_[i]->params()->vals()->copyFrom(graphs_[0]->params()->vals());
  //  };
  //  pool.enqueue(init, i);
  //}
  //// ThreadPool destructor waits until completion of all tasks.
  //// @TODO: can we use comm_->foreach()?
}

void SyncGraphGroup::initializeAvg() {
  Ptr<ExpressionGraph> graphAvg; // CPU-side temp
  std::string name = options_->get<std::string>("model");
  // if(filesystem::exists(name + ".orig.npz")) {
  //   // Load the averaged parameters into a temporary graph
  //   graphAvg = New<ExpressionGraph>();
  //   graphAvg->setDevice({0, DeviceType::cpu});
  //   graphAvg->setParameterType(graphs_[0]->getParameterType());

  //   graphAvg->load(name, false);
  //   graphAvg->forward(); // initialize parameters if needed
  // }

  auto init = [&](size_t localDeviceIndex, size_t begin, size_t end) {
    size_t size = end-begin;

    // get the device-specific allocator
    auto paramsAllocator = New<TensorAllocator>(graphs_[localDeviceIndex]->getBackend());
    paramsAllocs_[localDeviceIndex] = paramsAllocator;

    paramsAllocator->reserveExact(size * sizeOf(graphs_[0]->getParameterType()));

    // note: for multi-node, graphAvg and graphs_[0] contain a complete copy, from which
    // each MPI process copies only part into its respective shard(s)
    return true; // dummy success
  };

  paramsAllocs_.resize(graphs_.size()); // allocators
  comm_->foreach(init, /*parallel=*/false); // @TODO: is sequential operation necessary here? (is the allocation stuff sufficiently reentrant or thread-separated?)
}

Ptr<data::BatchStats> SyncGraphGroup::collectStats() {
  // This function determines the granularity in which the reader provides data.
  // If no mini-batch-fit, then user provides a constant number. It reads that much. We won't get into this function.
  // If mini-batch-fit, then we get here and set miniBatchFitMultiplier_. Then...
  // If dynamic MB scaling, then we want fine-grained minibatches of the size of one GPU.
  // If not, we prefer a single large batch that can be split into equal-size parts over GPUs,
  // so that we have perfect load balancing and read precisely as much as we need (no waste).
  double multiplier = devices_.size() * mpi_->numMPIProcesses() * delay_;
  bool isDynamic = scheduler_->isDynamicMBSizeScaling();
  double readerMultiplier = isDynamic ? 1. : multiplier; // multiplier applied already by reader
  updateMultiplier_ = isDynamic ? multiplier : 1.;       // multiplier applied later in update()
  return GraphGroup::collectStats(graphs_[0], builders_[0], readerMultiplier);
}

// helper for MB scaling: quantize the ratio with a given error margin
static double roundUpRatio(double ratio) {
  if (ratio == 0)
    return ratio;
  // find largest power of two that fits into ratio
  double p = 1;
  while (p*2 < ratio)
    p *= 2;
  // round up to nearest multiple of a largest power of 2 where relative error is within margin
  // 25% error margin seems acceptable:
  //  - using a 25% larger MB size should not break convergence
  //  - @TODO: not using the first 25% of the next block is OK since those are dominated by data exchange
  double maxError = 0.25;
  while (p >= 1) {
    double proposedRatio = ceil(ratio / p) * p;
    double error = (proposedRatio - ratio) / ratio;
    if (fabs(error) <= maxError)
      return proposedRatio;
    p /= 2;
  }
  return ratio;
}

// helper routine that handles accumulation and load-balancing of sub-batches to fill all devices
// It adds 'newBatch' to 'pendingBatches_', and if sufficient batches have been queued, then
// returns 'pendingBatches_' in 'subBatches' and resets it. If not, it returns false.
bool SyncGraphGroup::tryGetSubBatches(Ptr<data::Batch> newBatch, size_t overstuff,
    std::vector<Ptr<data::Batch>>& subBatches, size_t& numReadBatches) {
  // The reader delivers in chunks of these sizes, according to case:
  //  - no dynamic MB-size scaling:
  //     - reader batch size = update batch size, with...
  //     - mini-batch-fit:
  //        - update batch size = what fits into all GPUs, times decay_ to allow experimenting with fractional sizes
  //     - no mini-batch-fit:
  //        - update batch size = user-specified size (user guarantees that it fits if distributed over delay_ GPUs)
  //  - dynamic MB-size scaling:
  //     - update batch size = aggregate reader batch size * (dynamic progress-based ratio * reference adjustment), with...
  //     - mini-batch-fit:
  //        - aggregate reader batch size = equal to what fits into one GPU * warpSize * delay_
  //     - no mini-batch-fit:
  //        - aggregate reader batch size = user-specified size (user guarantees that it fits if distributed over delay_ GPUs)
  //     - reference adjustment =
  //        - reference batch size specified: (reference batch size / typical aggregate reader batch size)
  //        - no ref size specified: 1

  size_t warpSize = devices_.size() * mpi_->numMPIProcesses(); // warp := set of batches processed concurrently across GPus and workers

  // if not dynamic then return the big batch, but first split it over GPUs as it may be too large
  if (!scheduler_->isDynamicMBSizeScaling()) {
    // If mini-batch-fit, then the read batch is (devices_.size() * mpi_->numMPIProcesses() * delay_)
    // times what fits one GPU. If not mini-batch-fit, it is whatever the user has specified, which
    // is the user's responsibility to guarantee that it fits into 'delay_' warps.
    // Distribute evenly over all GPUs we have, using multiple warps if needed.
    size_t numWarps = (size_t)ceil(delay_);
    subBatches = newBatch->split(numWarps * warpSize);
    numReadBatches = 1;
    return true;
  }
  LOG_ONCE(info, "[training] Dynamic mini-batch scaling enabled");

  // if dynamic and mini-batch-fit, then we get batches in the size of what fits into one GPU
  pendingBatches_.push_back(newBatch);

  // what ratio (how many batches in reader's batch size) do we want, based on current training progress schedule?
  double ratio = scheduler_->getDynamicMBSizeMultiplier();

  // relative to what base? (what does ratio == 1 mean)
  ratio *= updateMultiplier_; // if mini-batch-fit, this is = warpSize * delay_, otherwise 1

  // If a reference is given, then at progress == mbWarmup.n (ratio=1), we would like to have refBatchLabels instead of whichever
  // the actual batch size is. Since we cannot know the future actual batch sizes that will be delivered
  // by the reader, we approximate them with (typicalTrgBatchWords * updateMultiplier), and scale ratio accordingly.
  auto refBatchLabels = options_->get<size_t>("mini-batch-words-ref");
  if (refBatchLabels != 0) {
    LOG_ONCE(info, "[scheduler] Scaling to {} reference labels, using actual-batch-word estimate of {}", refBatchLabels, typicalTrgBatchWords_);
    ABORT_IF(typicalTrgBatchWords_ == 0, "Dynamic scaling with words target requires MB size to be known in words"); // happens if MB size is specified in sentences
    ratio *= (double)refBatchLabels / (double)(typicalTrgBatchWords_ * updateMultiplier_);
  }

  // overstuff: blow up ratio by a factor, which we later factor into the learning rate
  ratio *= (double)overstuff;

  // round up to full batches if within a certain error margin  --@BUGBUG: Not invariant w.r.t. GPU size, as ratio is relative to what fits into 1 GPU
  ratio = roundUpRatio(ratio);

  if (pendingBatches_.size() < ratio)
    return false; // not enough data yet

  // now we have enough to fill at least 'ratio' batches
  // @BUGBUG: We do not handle the case that fixed MB size * ratio exceeds GPU memory (we'd need to split that).

  numReadBatches = pendingBatches_.size(); // remember original batch-counter increment from reader (which is not always the same as subBatches.size() in the end)

  // in fact, we got too much, so make up for it by shortening all batches to accurately reflect desired ratio
  // e.g. ratio = 3.3 for 4 batches -> Reduce each by 3.3/4
  // Alternatively, we could just shorten the last 'warp', but that would not be invariant to warp size.
  for (auto& batch : pendingBatches_) {
    auto reducedBatchSize = (size_t)ceil((double)batch->size() * ratio / (double)pendingBatches_.size());
    size_t minSize = 1;
    if (pendingBatches_.size() == 1) { // enforce a minimum (only needed/correct if still in first batch)
      size_t minTrgWords = 256;        // don't go below this number of target words, as it seems excessive  --@TODO: parameterize?
      minSize = 1 + (minTrgWords * batch->size() - 1) / batch->wordsTrg(); // approximately convert minTrgWords into a #sentences
    }
    reducedBatchSize = std::max(reducedBatchSize, minSize);
    if (reducedBatchSize < batch->size())
      batch = batch->split(/*numSubBatches=*/1, reducedBatchSize).front();
  }

  // load-balance: distribute the last numWarps-group's batches over GPUs
  // This is tricky since batches do not have the same length, therefore we can only split, but not merge.
  auto numWarps = (pendingBatches_.size() - 1) / warpSize + 1; // = ceil(#buffers / (#GPUs * #workers))
  auto availableDevices = numWarps * warpSize; // we will run this many GPUs: better use them all
  if (pendingBatches_.size() < availableDevices) {
    // last warp does not use all available GPUs: try to re-balance
    auto fullWarpsBatches = (numWarps - 1) * warpSize; // number of batches in all but the last warp. Those warps that are fully used.
    auto lastWarpSize = pendingBatches_.size() - fullWarpsBatches; // the last warp is possibly not fully used
    //LOG(info, "attempting to redistribute last {} batches over {} devices", lastWarpSize, warpSize);
    auto splitInto = warpSize / lastWarpSize;
    if (splitInto > 1) { // unfortunately we can only split in integer ratios
      // split each of last numWarps's batches into 'splitInto' batches
      // pop them first
      std::vector<Ptr<data::Batch>> batchesToSplit;
      while (pendingBatches_.size() > fullWarpsBatches) {
        batchesToSplit.push_back(pendingBatches_.back());
        pendingBatches_.pop_back();
      }
      // now split them and push them back
      for (auto& batchToSplit : batchesToSplit) {
        //LOG(info, "{}-way splitting batchToSplit with size {}", splitInto, batchToSplit->size());
        auto splitBatches = batchToSplit->split(splitInto);
        for (auto& splitBatch : splitBatches) {
          //LOG(info, " -> getting batchToSplit with size {}", splitBatch->size());
          pendingBatches_.push_back(splitBatch);
        }
      }
    }
    ABORT_IF(pendingBatches_.size() > availableDevices, "somehow split into too many batches??");
  }
  subBatches = std::move(pendingBatches_);

  // @TODO: sort by width, so that in case of delay > 1, each GPU gets about the same size
  return true;
}

void SyncGraphGroup::update(Ptr<data::Batch> newBatch) /*override*/ {
  validate();

  size_t overstuff = options_->get<size_t>("mini-batch-overstuff");
  if (overstuff != 1)
    LOG_ONCE(info, "Overstuffing minibatches by a factor of {}", overstuff);
  std::vector<Ptr<data::Batch>> subBatches;
  size_t numReadBatches; // actual #batches delivered by reader, for restoring from checkpoint   --@TODO: reader should checkpoint itself; should not go via the scheduler
  bool gotSubBatches = tryGetSubBatches(newBatch, overstuff, subBatches, numReadBatches);

  // not enough data yet: return right away
  if (!gotSubBatches)
    return;

  // for testing the hypothesis that one can always go smaller. This is independent of overstuff.
  size_t understuff = options_->get<size_t>("mini-batch-understuff");
  if (understuff != 1)
    LOG_ONCE(info, "Understuffing minibatches by a factor of {}", understuff);
  if (understuff == 1)
    update(subBatches, numReadBatches);
  else {
    std::vector<Ptr<data::Batch>> subBatches1;
    for (auto& b : subBatches) {
      auto bbs = b->split(understuff);
      for (auto& bb : bbs)
        subBatches1.push_back(bb);
    }
    for (size_t i = 0; i < understuff; i++) {
      std::vector<Ptr<data::Batch>> subBatchRange(subBatches1.begin() + i * subBatches1.size() / understuff, subBatches1.begin() + (i+1) * subBatches1.size() / understuff);
      if (!subBatchRange.empty())
        update(subBatchRange, numReadBatches * (i+1) / understuff - numReadBatches * i / understuff);
    }
  }
}

void SyncGraphGroup::update(std::vector<Ptr<data::Batch>> subBatches, size_t numReadBatches) {
  size_t overstuff  = options_->get<size_t>("mini-batch-overstuff");
  //size_t understuff = options_->get<size_t>("mini-batch-understuff");
  // determine num words for dynamic hyper-parameter adjustment
  // @TODO: We can return these directly from tryGetSubBatches()
  size_t batchSize = 0;
  size_t batchTrgWords = 0;
  for (const auto& batch : subBatches) {
    batchSize     += batch->size();
    batchTrgWords += batch->wordsTrg();
  }
  // effective batch size: batch should be weighted like this. This will weight down the learning rate.
  size_t effectiveBatchTrgWords = (size_t)ceil(batchTrgWords / (double)overstuff);
  size_t effectiveBatchSize     = (size_t)ceil(batchSize     / (double)overstuff);

  // Helper to access the subBatches array
  auto getSubBatch = [&](size_t warp, size_t localDeviceIndex, size_t rank) -> Ptr<data::Batch> {
    // Warp should be slowest changing dimension. If subBatches are sorted by
    // length, then grouping sentences of similar length into the same delay step can
    // reduce unnecessary time spent in padding.
    auto index = (warp * mpi_->numMPIProcesses() + rank) * devices_.size() + localDeviceIndex;
    if (index < subBatches.size())
      return subBatches[index];
    else
      return nullptr; // null if we reached beyond the end
  };

  // Upon very first execution, reset everything
  if(first_) {
    LOG(info, "[training] Batches are processed as {} process(es) x {} devices/process",
        mpi_->numMPIProcesses(), devices_.size());
    initialize(subBatches.front());
    first_ = false;
  }

  // Compute gradients
  // This happens in multiple steps in case of delay > 1.
  std::vector<float> localDeviceCosts(devices_.size(), 0.f); // [local device index] aggregate cost for each local device

  comm_->foreach([&](size_t localDeviceIndex, size_t /*begin*/, size_t /*end*/) { // parallel across devices. Aggregate for warp > 1.
    auto graph = graphs_[localDeviceIndex];
    // reset gradient  --presently done outside
    //graph->params()->allocateBackward();
    //if (warp == 0) // these have already been sized
    //  graph->params()->set_zero_adjoint();
    for (size_t warp = 0; ; warp++) {
      // Execute single forward/backward step
      auto subBatch = getSubBatch(warp, localDeviceIndex, mpi_->myMPIRank());
      if (!subBatch)
        break;

      auto costNode = builders_[localDeviceIndex]->build(graph, subBatch);
      if(costScaleFactor_ != 1.f)
        costNode = costNode * costScaleFactor_;

      graph->forward();
      localDeviceCosts[localDeviceIndex] += costNode->scalar() / (costScaleFactor_ * (float)overstuff);

      //float clipValue = options_->get<float>("clip-norm") * costScaleFactor_;
      graph->backward(/*zero=*/false, 0); // (gradients are reset before we get here)

      // @TODO: remove again, only for debugging.
      /*if(localDeviceIndex == 0 && mpi_->myMPIRank() == 0) {
        Logger logger = spdlog::get("norms");
        if(!logger)
          logger = createStderrLogger("norms", "%v", { options_->get<std::string>("log") + ".norms"}, true);

        if(scheduler_->numberOfBatches() % 100 == 0) {
          for(auto p : *graph->params()) {
            float norm = L2Norm(p->grad(), graph->allocator());
            logger->info("{}\t{}\t{}", scheduler_->numberOfBatches(), p->name(), norm);
          }
        }
      }*/
    }

    // Handle local gradient explosion but only clip to largest possible value
    // given number of GPUs and type. Should clip rarely. Also clips inf
    // We do another clipping/rescaling after summation.
    auto gradType = graph->params()->grads()->type();
    if(sizeOf(gradType) < sizeOf(Type::float32)) {
       using namespace functional;
       float numGpus = mpi_->numMPIProcesses() * devices_.size();
       float clipValue = NumericLimits<float>(gradType).max / numGpus;
       Element(_1 = clip(_1, clipValue), graph->params()->grads());
    }

    return true; // dummy success
  });

  // At this point, each device on each MPI process has a gradient aggregated over a subset of the sub-batches.

  // If individual gradients were averages, then need to average again over all subBatches
  float div = (float)subBatches.size();
  if (options_->get<std::string>("cost-type") == "ce-sum")
    div = (float)overstuff; // (note: with Adam, a constant here makes no difference)

  // check for Nan or Inf in all summed up shards
  auto checkNan = [&](size_t i, size_t begin, size_t end) {
    auto curGrad = graphs_[i]->params()->grads()->subtensor(begin, end-begin);
    bool hasNan = false, hasInf = false;
    IsNan(curGrad, graphs_[i]->allocator(), hasNan, hasInf);

    /*
    if(hasNan)
      LOG(warn, "Seen NaN");
    if(hasInf)
      LOG(warn, "Seen Inf");
    */

    return !(hasNan || hasInf);
  };

  // Update parameter shard with gradient shard
  auto update = [&](size_t i, size_t begin, size_t end) -> float {
    auto curGrad = graphs_[i]->params()->grads()->subtensor(begin, end-begin);
    auto curParam = graphs_[i]->params()->vals()->subtensor(begin, end-begin);

    if(div != 1) {
      using namespace functional;
      Element(_1 = _1 / div, curGrad);   // average once again for ce-mean*, or scale down if overstuffed for ce-sum
    }

    // actual model update
    auto updateTrgWords =
        /*if*/(options_->get<std::string>("cost-type") == "ce-sum") ?
          effectiveBatchTrgWords // if overstuffing then bring the count back to the original value
        /*else*/:
          OptimizerBase::mbSizeNotProvided;

    float l2norm = shardOpt_[i]->update(curParam, curGrad, updateTrgWords, costScaleFactor_ / (div / updateTrgWords));
    curGrad->set(0.f); // @TODO: all the different places where gradients get reset are confusing

    return l2norm; // return partial norm
  };

  // Reset gradient shard when no update was done
  auto reset = [&](size_t i, size_t begin, size_t end) {
    auto curGrad = graphs_[i]->params()->grads()->subtensor(begin, end-begin);
    curGrad->set(0.f); // @TODO: all the different places where gradients get reset are confusing
    return true; // dummy success
  };

  comm_->scatterReduceAndResetGrads();      // reduce gradients across all devices (globally) into shards
  bool noNanOrInf = costScale_ ? comm_->foreach(checkNan) : true; // @TODO: does this work with MPI?
  float gradNorm = 0.f;
  if(noNanOrInf) {
    auto accNorms = [](float& lhs, float rhs) {
      lhs = sqrtf(lhs * lhs + rhs * rhs); // to accumulate gradients norms, first undo sqrt, sum, apply sqrt.
    };

    gradNorm = comm_->foreach(update, accNorms, 0.f); // per-shard model-update
    comm_->allGatherParams(); // distribute param value shards back
  } else {
    comm_->foreach(reset);   // per-shard model-update
    GraphGroup::decreaseCostScaleFactor();
  }

  // cost across all local devices (scheduler will aggregate cross-process)
  float localCost = 0;
  for(auto& c : localDeviceCosts) // localDeviceCosts is already summed up over delay steps
    localCost += c;

  // if localCost is average-based, we need to turn the sum over devices into an average as well
  if(options_->get<std::string>("cost-type") != "ce-sum")
    localCost /= subBatches.size();

  if(scheduler_) {
    // track and log localCost
    scheduler_->update(localCost, gradNorm, numReadBatches, effectiveBatchSize, effectiveBatchTrgWords, mpi_);

    // save intermediate model (and optimizer state) to file
    if(scheduler_->saving())
      save();

    // process valid data set
    // This may save a model as well.
    if(scheduler_->validating()) {
      if(isMainProcess()) {
        std::vector<Ptr<ExpressionGraph>> tempGraphs;
        for(auto graph : graphs_)
          tempGraphs.push_back(graphFromOptimizer(graph, shardOpt_));
        scheduler_->validate(tempGraphs);
      }
    }
  }

  if(noNanOrInf)
    GraphGroup::increaseCostScaleFactor();
}

void SyncGraphGroup::load() /*override*/ {
  validate();

  // This function loads the main parameters in the graphs.

  if(!options_->get<bool>("no-reload")) {
    std::string name = options_->get<std::string>("model");

    if(filesystem::exists(name)) {
      if(scheduler_)
        scheduler_->load(name);

      std::string nameGraph = name;

      size_t i = 0;
      for(auto graph : graphs_)
        builders_[i++]->load(graph, nameGraph); // we just load it N times from disk (it'll be in disk cache after the first)

      // @TODO: probably we want to have the list of DeviceIds as an attribute
      std::vector<Ptr<Backend>> backends;
      for(auto graph : graphs_)
        backends.push_back(graph->getBackend());
      shardOpt_[0]->load(name + ".optimizer.npz", shardOpt_, backends,
        [&](const io::Item& optimizerState, const OptimizerBase::ScatterStateSetFunc& setShardFn) {
          comm_->scatterState(optimizerState, setShardFn);
        });
      LOG(info, "[training] Model reloaded from {}", name);
    } else if(options_->has("pretrained-model")) {
      std::string nameInit = options_->get<std::string>("pretrained-model");
      LOG(info,
          "[training] Initializing model weights with the pre-trained model {}",
          nameInit);

      size_t i = 0;
      for(auto graph : graphs_)
        builders_[i++]->load(graph, nameInit, false);
    }
  }
}

void SyncGraphGroup::save(bool final) /*override*/ {
  // validate(); @TODO: get rid of this everywhere (SyncGraphGroup)
  barrier(); // (for better grouping of log messages)
  // do final validation
  if(final && scheduler_) {
    // bring the smoothed model in
    // Note that it is sharded. For multi-node, it is sharded over multiple machines, so this is a network access.
    // Also note that the swap must run on all MPI processes concurrently, although only one actually validates.

    if (isMainProcess()) { // in multi-node, only first MPI process saves the model (they are all identical)
      std::vector<Ptr<ExpressionGraph>> tempGraphs;
      for(auto graph : graphs_)
        tempGraphs.push_back(graphFromOptimizer(graph, shardOpt_));
      scheduler_->validate(tempGraphs, true);
    }

  }

  std::string name = options_->get<std::string>("model");

  barrier(); // (for better grouping of log messages)

  // save main model file
  if (isMainProcess()) { // only save from one MPI process
    // if not overwrite then save a copy with number of updates in the model pathname
    auto tempGraph = graphFromOptimizer(graphs_[0], shardOpt_);

    if(!options_->get<bool>("overwrite") && !final) {
      std::string numberOfBatches
          = scheduler_ ? std::to_string(scheduler_->numberOfBatches())
                       : "unknown";
      std::string nameOverwrite = name;
      nameOverwrite.replace(name.size() - 4, 4, ".iter" + numberOfBatches + ".npz"); // @TODO: use insert?
      builders_[0]->save(tempGraph, nameOverwrite);
    }
    // save main model file
    builders_[0]->save(tempGraph, name, true);
    // save scheduler-related state
    if (scheduler_)
      scheduler_->save(name);
  }

  barrier(); // (for better grouping of log messages)

  // persist optimizer state
  shardOpt_[0]->save(name + ".optimizer.npz", shardOpt_,
    [&](const OptimizerBase::GatherStateGetFunc& getShardFn) {
      return comm_->gatherState(getShardFn);
    },
    isMainProcess());

  barrier(); // (for better grouping of log messages)
}

void SyncGraphGroup::finalize() /*override*/ {
  validate();
  Base::finalize();
}

SyncGraphGroup::~SyncGraphGroup() /*override*/ {
  comm_ = nullptr;
  finalizeMPI(std::move(mpi_));
}

}  // namespace marian
