/*
 * Copyright (c) 2020, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "ProcessGroupCCL.hpp"

#include <map>

namespace c10d
{

#define CCL_CHECK(cmd)                                               \
  do {                                                               \
    try {                                                            \
        cmd;                                                         \
    }                                                                \
    catch (ccl::ccl_error& e) {                                      \
        throw std::runtime_error("CCL error in: " +                  \
            std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
            ", with error message: " + e.what());                    \
    }                                                                \
    catch (...) {                                                    \
        throw std::runtime_error("unknown error in: " +              \
            std::string(__FILE__) + ":" + std::to_string(__LINE__)); \
    }                                                                \
  } while (0)

namespace {

// Op mapping
std::map<ReduceOp, ccl::reduction> cclOps =
{
    {ReduceOp::MIN, ccl::reduction::min},
    {ReduceOp::MAX, ccl::reduction::max},
    {ReduceOp::SUM, ccl::reduction::sum},
    {ReduceOp::PRODUCT, ccl::reduction::prod},
};

// Type mapping
std::map<at::ScalarType, ccl::data_type> cclDatatypes =
{
    {at::kByte, ccl::data_type::dt_char},
    {at::kChar, ccl::data_type::dt_char},
    {at::kDouble, ccl::data_type::dt_double},
    {at::kBFloat16, ccl::data_type::dt_bfp16},
    {at::kFloat, ccl::data_type::dt_float},
    {at::kInt, ccl::data_type::dt_int},
    {at::kLong, ccl::data_type::dt_int64}
};

static std::once_flag cclInitOnceFlag;
static std::mutex globalMutex;
static ccl::communicator_t globalComm;
static ccl::coll_attr collAttr;
static ccl::coll_attr collAttrAg;

// Checking the input tensor's validity
void checkSingleTensorHelper(const at::Tensor& tensor)
{
    TORCH_CHECK(tensor.is_contiguous(), "input tensor has to be contiguous");
    TORCH_CHECK(!tensor.is_sparse(), "input tensor has to be dense");
    TORCH_CHECK(!tensor.is_cuda(), "CUDA tensor detected and CCL doesn't support CUDA buffers");
    TORCH_CHECK(tensor.numel() >= 0, "input tensor numel should be non-negative");
}

void checkRank(int rank, int size)
{
    TORCH_CHECK((rank >= 0) && (rank < size), "unexpected rank");
}

void checkSingleTensor(const std::vector<at::Tensor>& tensors)
{
    TORCH_CHECK(tensors.size() == 1,
        "CCL process group does not support tensors count " + std::to_string(tensors.size()));

    checkSingleTensorHelper(tensors[0]);
}

void checkSameSizeAndType(const at::Tensor& tensor,
                          const std::vector<at::Tensor>& tensors)
{
    for (size_t i = 0; i < tensors.size(); ++i)
    {
        TORCH_CHECK((tensors[i].numel() == tensor.numel()) &&
                    (tensors[i].scalar_type() == tensor.scalar_type()),
                    "tensors are not equal in size or data type");

        checkSingleTensorHelper(tensors[i]);
    }
}

void checkSameType(const at::Tensor& tensor,
                   const std::vector<at::Tensor>& tensors)
{
    for (size_t i = 0; i < tensors.size(); ++i)
    {
        TORCH_CHECK(tensors[i].scalar_type() == tensor.scalar_type(),
            "tensors are not equal in data type");

        checkSingleTensorHelper(tensors[i]);
    }
}

void checkSplitSizes(
    const std::vector<int64_t>& split_sizes,
    const at::Tensor& tensor,
    int groupSize)
{
    if (split_sizes.size() == 0)
    {
        TORCH_CHECK(tensor.size(0) % groupSize == 0,
            "Tensor's dim 0 does not divide equally across group size");
    }
    else
    {
        TORCH_CHECK(split_sizes.size() == (size_t)groupSize,
            "Number of tensor splits not equal to group size");

        int sum = std::accumulate(split_sizes.begin(), split_sizes.end(), 0);

        TORCH_CHECK(sum == tensor.size(0),
            "Split sizes doesn't match total dim 0 size");
    }
}

bool computeLengthsAndCheckAndGetFlat(
    const std::vector<at::Tensor>& tensors,
    std::vector<size_t>& lengths,
    at::Tensor& flatTensor)
{
    int64_t groupSize = lengths.size();
    auto firstTensor = tensors[0];
    int64_t offset = 0;
    auto firstLength = firstTensor.numel();
    auto storage = firstTensor.storage();
    auto firstStorageOffset = firstTensor.storage_offset();
    bool isFlat = true;

    for (int i = 0; i < groupSize; i++)
    {
        auto& curTensor = tensors[i];
        int64_t length = curTensor.numel();

        if (firstLength == 0 && length != 0)
        {
            firstLength = length;
            firstTensor = curTensor;
            storage = curTensor.storage();
            firstStorageOffset = curTensor.storage_offset();
        }

        lengths[i] = length;

        if (isFlat && length != 0 &&
            (!storage.is_alias_of(curTensor.storage()) ||
             curTensor.storage_offset() != firstStorageOffset + offset))
            isFlat = false;

        offset += length;
    }

    if (isFlat)
    {
        flatTensor = firstTensor;
    }
    else
    {
        flatTensor = at::empty({offset}, firstTensor.options());
    }

    return isFlat;
}

} // namespace

ProcessGroupCCL::WorkCCL::~WorkCCL()
{
    if (req)
    {
        std::cerr << "attempted destruction of WorkCCL before work has completed, "
                  << "terminating the program."
                  << std::endl;
        std::terminate();
    }
}

bool ProcessGroupCCL::WorkCCL::isCompleted()
{
    if (!req)
    {
        return true;
    }

    bool flag = false;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(flag = req->test());

    if (flag)
    {
        req.reset();
        tensors.clear();
    }

    return flag;
}

bool ProcessGroupCCL::WorkCCL::isSuccess() const
{
    if (req)
    {
        throw std::runtime_error(
            "invalid call to WorkCCL::isSuccess before work has completed");
    }
    return true;
}

bool ProcessGroupCCL::WorkCCL::wait()
{
    if (!req)
    {
        return true;
    }

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req->wait());
    req.reset();
    tensors.clear();

    // Always return true, because abort API is not implemented.
    return true;
}

void ProcessGroupCCL::WorkCCL::abort()
{
    TORCH_CHECK(false, "ProcessGroupCCL::WorkCCL::abort not implemented.");
}

#ifdef USE_VECTOR_ALLGATHERV
thread_local std::vector<void*> ProcessGroupCCL::agRecvBuffers;
#endif

void ProcessGroupCCL::cclFini()
{
    std::unique_lock<std::mutex> globalLock(globalMutex);
    globalComm.reset();
}

void ProcessGroupCCL::cclInitOnce()
{
    std::call_once(cclInitOnceFlag, []() {

#ifdef USE_CACHE
      /* to enable collective caching */
      collAttr.to_cache = 1;
      collAttrAg.to_cache = 1;
#endif

#ifdef USE_VECTOR_ALLGATHERV
      /* to enable allgatherv with recv buffers vector */
      collAttrAg.vector_buf = 1;
#endif

      CCL_CHECK(globalComm = ccl::environment::instance().create_communicator());

#ifdef USE_VECTOR_ALLGATHERV
      agRecvBuffers.reserve(globalComm->size());
#endif

      if (std::atexit(ProcessGroupCCL::cclFini))
      {
          throw std::runtime_error("failed to register the CCL exit handler");
      }
  });
}

std::shared_ptr<ProcessGroup> ProcessGroupCCL::createProcessGroupCCL(
    const std::shared_ptr<Store>& store,
    int rank,
    int size,
    const std::chrono::duration<float>& timeout)
{
    cclInitOnce();

    TORCH_CHECK(((rank == -1) || (size_t)rank == globalComm->rank()),
        "unexpected rank " + std::to_string(rank) +
        ", CCL rank " + std::to_string(globalComm->rank()));

    TORCH_CHECK(((size == -1) || (size_t)size == globalComm->size()),
        "unexpected size " + std::to_string(size) +
        ", CCL size " + std::to_string(globalComm->size()));

    return std::make_shared<ProcessGroupCCL>(rank, size);
}

ProcessGroupCCL::ProcessGroupCCL(int rank, int size)
    : ProcessGroup(globalComm->rank(),
                   globalComm->size())
{
    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(comm = ccl::environment::instance().create_communicator());
}

ProcessGroupCCL::~ProcessGroupCCL()
{
    std::unique_lock<std::mutex> globalLock(globalMutex);
    comm.reset();
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::broadcast(
    std::vector<at::Tensor>& tensors,
    const BroadcastOptions& opts)
{
    checkSingleTensor(tensors);
    checkRank(opts.rootRank, getSize());

#ifdef USE_CACHE
    collAttr.match_id = opts.tensorName.c_str();
#endif

    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->bcast(tensors[0].data_ptr(),
                                (size_t)tensors[0].numel(),
                                cclDatatypes.at(tensors[0].scalar_type()),
                                (size_t)opts.rootRank,
                                &collAttr));

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, tensors);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::allreduce(
    std::vector<at::Tensor>& tensors,
    const AllreduceOptions& opts)
{
    checkSingleTensor(tensors);

#ifdef USE_CACHE
    collAttr.match_id = opts.tensorName.c_str();
#endif

    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->allreduce(tensors[0].data_ptr(),
                                    tensors[0].data_ptr(),
                                    (size_t)tensors[0].numel(),
                                    cclDatatypes.at(tensors[0].scalar_type()),
                                    cclOps.at(opts.reduceOp),
                                    &collAttr));

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, tensors);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::allreduce_coalesced(
    std::vector<at::Tensor>& /* unused */,
    const AllreduceCoalescedOptions& /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support allreduce_coalesced");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::reduce(
    std::vector<at::Tensor>& tensors,
    const ReduceOptions& opts)
{
    checkSingleTensor(tensors);
    checkRank(opts.rootRank, getSize());

#ifdef USE_CACHE
    collAttr.match_id = opts.tensorName.c_str();
#endif

    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->reduce(tensors[0].data_ptr(),
                                 tensors[0].data_ptr(),
                                 (size_t)tensors[0].numel(),
                                 cclDatatypes.at(tensors[0].scalar_type()),
                                 cclOps.at(opts.reduceOp),
                                 (size_t)opts.rootRank,
                                 &collAttr));

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, tensors);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::allgather(
    std::vector<std::vector<at::Tensor>>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const AllgatherOptions& opts)
{
    checkSingleTensor(inputTensors);

    TORCH_CHECK(outputTensors.size() == 1,
        "ProcessGroupCCL/allgather supports a single tensor op only");

    TORCH_CHECK(comm->size() == outputTensors[0].size(),
        "ProcessGroupCCL/allgather: number of output tensors should equal "
        "to the world size");

    checkSameSizeAndType(inputTensors[0], outputTensors[0]);

#ifdef USE_CACHE
    collAttrAg.match_id = opts.tensorName.c_str();
#endif

#ifdef USE_VECTOR_ALLGATHERV
    agRecvBuffers.clear();
    std::transform(outputTensors[0].begin(), outputTensors[0].end(),
                   std::back_inserter(agRecvBuffers), [](const at::Tensor& t) { return t.data_ptr(); } );
#else
    auto flatOutputTensor = newLikeFlat(outputTensors[0]);
#endif
    std::vector<size_t> recvCounts(comm->size(), inputTensors[0].numel());

    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->allgatherv(inputTensors[0].data_ptr(),
                                     (size_t)inputTensors[0].numel(),
#ifdef USE_VECTOR_ALLGATHERV
                                     agRecvBuffers.data(),
#else
                                     flatOutputTensor.data_ptr(),
#endif
                                     (size_t*)recvCounts.data(),
                                     cclDatatypes.at(inputTensors[0].scalar_type()),
                                     &collAttrAg));

#ifdef USE_VECTOR_ALLGATHERV
    auto agTensors = std::vector<at::Tensor>(outputTensors[0].begin(), outputTensors[0].end());
    agTensors.emplace_back(inputTensors[0]);
#else
    req->wait();
    for (size_t i = 0; i < outputTensors[0].size(); ++i)
    {
        outputTensors[0][i].copy_(flatOutputTensor[i]);
    }
    std::vector<at::Tensor> agTensors;
#endif

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, std::move(agTensors));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::allgather_base(
      at::Tensor& outputBuffer,
      at::Tensor& inputBuffer,
      const AllgatherOptions& /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support allgather_base");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::allgather_coalesced(
    std::vector<std::vector<at::Tensor>>& /* unused */,
    std::vector<at::Tensor>& /* unused */,
    const AllgatherOptions& /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support allgather_coalesced");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::gather(
    std::vector<std::vector<at::Tensor>>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const GatherOptions& opts)
{
    checkSingleTensor(inputTensors);

    if (rank_ != opts.rootRank)
    {
        TORCH_CHECK(outputTensors.size() == 0,
            "Gather: number of output tensors should be 0 "
            "for non-root");
    }
    else
    {
        TORCH_CHECK(outputTensors.size() == 1,
            "Gather: multi-GPU collective is not supported");

        TORCH_CHECK(static_cast<size_t>(size_) == outputTensors[0].size(),
            "Gather: number of output tensors should equal "
            "to the world size");

        // We don't need to be of same size but checking to be compatible with MPI
        checkSameSizeAndType(inputTensors[0], outputTensors[0]);
    }

    std::vector<size_t> sendCounts(size_, 0);
    std::vector<size_t> recvCounts(size_, 0);
    sendCounts[opts.rootRank] = inputTensors[0].numel();

    at::Tensor flatOutput;
    bool isOutputFlat = false;

    if (rank_ == opts.rootRank)
    {
        isOutputFlat =
            computeLengthsAndCheckAndGetFlat(outputTensors[0],
                                             recvCounts, flatOutput);
        TORCH_CHECK(sendCounts[rank_] == recvCounts[rank_],
            "Gather: Send and recv count doesn't match");
    }
    else
    {
        flatOutput = at::empty({0}, inputTensors[0].options());
    }


    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->alltoallv(inputTensors[0].data_ptr(),
                                    sendCounts.data(),
                                    flatOutput.data_ptr(),
                                    recvCounts.data(),
                                    cclDatatypes.at(flatOutput.scalar_type()),
                                    &collAttr));

    std::vector<at::Tensor> gatherTensors;

    if (rank_ == opts.rootRank)
    {
        if (!isOutputFlat)
        {
            req->wait();

            auto flatOutputSplits =
                flatOutput.split_with_sizes(c10::IntArrayRef((int64_t*)recvCounts.data(),
                                            recvCounts.size()), 0);

            for (int i = 0; i < size_; i++)
            {
                outputTensors[0][i].view({-1}).copy_(flatOutputSplits[i]);
            }
        }
        else
        {
            gatherTensors.emplace_back(flatOutput);
            gatherTensors.emplace_back(inputTensors[0]);
        }
    }
    else
    {
        gatherTensors.emplace_back(inputTensors[0]);
    }

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, std::move(gatherTensors));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::scatter(
    std::vector<at::Tensor>& outputTensors,
    std::vector<std::vector<at::Tensor>>& inputTensors,
    const ScatterOptions& opts)
{
    checkSingleTensor(outputTensors);

    if (rank_ != opts.rootRank)
    {
        TORCH_CHECK(inputTensors.size() == 0,
            "Scatter: number of input tensors should be 0 "
            "for non-root");
    }
    else
    {
        TORCH_CHECK(inputTensors.size() == 1,
            "Scatter: multi-GPU collective is not supported");

        TORCH_CHECK(static_cast<size_t>(size_) == inputTensors[0].size(),
            "Scatter: number of input tensors should equal "
            "to the world size");

        // We don't need to be of same size but checking to be compatible with MPI
        checkSameSizeAndType(outputTensors[0], inputTensors[0]);
    }

    std::vector<size_t> sendCounts(size_, 0);
    std::vector<size_t> recvCounts(size_, 0);
    recvCounts[opts.rootRank] = outputTensors[0].numel();

    at::Tensor flatInput;

    if (rank_ == opts.rootRank)
    {
        bool isInputFlat =
            computeLengthsAndCheckAndGetFlat(inputTensors[0],
                                             sendCounts, flatInput);

        if (!isInputFlat)
        {
            auto flatInputSplits =
                flatInput.split_with_sizes(c10::IntArrayRef((int64_t*)sendCounts.data(),
                                           sendCounts.size()), 0);

            for (int i = 0; i < size_; i++)
            {
                flatInputSplits[i].copy_(inputTensors[0][i].view({-1}));
            }
        }
        TORCH_CHECK(recvCounts[rank_] == sendCounts[rank_],
            "Scatter: Send and recv count doesn't match");
    }
    else
    {
        flatInput = at::empty({0}, outputTensors[0].options());
    }

    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->alltoallv(flatInput.data_ptr(),
                                    sendCounts.data(),
                                    outputTensors[0].data_ptr(),
                                    recvCounts.data(),
                                    cclDatatypes.at(flatInput.scalar_type()),
                                    &collAttr));

    std::vector<at::Tensor> scatterTensors;
    scatterTensors.emplace_back(outputTensors[0]);
    if (rank_ == opts.rootRank)
        scatterTensors.emplace_back(flatInput);

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, std::move(scatterTensors));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::reduce_scatter(
    std::vector<at::Tensor>& /* unused */,
    std::vector<std::vector<at::Tensor>>& /* unused */,
    const ReduceScatterOptions& /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support reduce_scatter");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::alltoall_base(
    at::Tensor& outputTensor,
    at::Tensor& inputTensor,
    std::vector<int64_t>& outputSplitSizes,
    std::vector<int64_t>& inputSplitSizes,
    const AllToAllOptions& opts)
{
    checkSingleTensorHelper(inputTensor);
    checkSingleTensorHelper(outputTensor);

    std::shared_ptr<ccl::request> req;

    if (outputSplitSizes.size() == 0 && inputSplitSizes.size() == 0)
    {
        // We can use alltoall
        TORCH_CHECK(outputTensor.numel() == inputTensor.numel() &&
                    outputTensor.scalar_type() == inputTensor.scalar_type(),
                    "Tensors are not equal in size or data type");

        TORCH_CHECK(outputTensor.size(0) % size_ == 0,
            "Tensor's dim 0 does not divide equally across group size");

        std::unique_lock<std::mutex> globalLock(globalMutex);
        CCL_CHECK(req = comm->alltoall(inputTensor.data_ptr(),
                                       outputTensor.data_ptr(),
                                       (size_t)outputTensor.numel() / comm->size(),
                                       cclDatatypes.at(outputTensor.scalar_type()),
                                       &collAttr));
    }
    else
    {
        // Need alltoallv
        checkSplitSizes(inputSplitSizes, inputTensor, size_);
        checkSplitSizes(outputSplitSizes, outputTensor, size_);

        std::vector<size_t> sendCounts(size_);
        std::vector<size_t> recvCounts(size_);

        // inLen or outLen can be 0 so we need explicit flag
        bool inputSplitsEqual = inputSplitSizes.size() == 0;
        bool outputSplitsEqual = outputSplitSizes.size() == 0;

        size_t inLen = inputTensor.numel();
        size_t outLen = outputTensor.numel();
        if (inLen) inLen /= (inputSplitsEqual ? size_ : inputTensor.size(0));
        if (outLen) outLen /= (outputSplitsEqual ? size_ : outputTensor.size(0));

        for (int i = 0; i < size_; i++)
        {
            sendCounts[i] = (inputSplitsEqual ? inLen : inputSplitSizes[i] * inLen);
            recvCounts[i] = (outputSplitsEqual ? outLen : outputSplitSizes[i] * outLen);
        }

        std::unique_lock<std::mutex> globalLock(globalMutex);
        CCL_CHECK(req = comm->alltoallv(inputTensor.data_ptr(),
                                        sendCounts.data(),
                                        outputTensor.data_ptr(),
                                        recvCounts.data(),
                                        cclDatatypes.at(outputTensor.scalar_type()),
                                        &collAttr));
    }

    auto a2aTensors = std::vector<at::Tensor> { inputTensor, outputTensor };
    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, std::move(a2aTensors));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::alltoall(
    std::vector<at::Tensor>& outputTensors,
    std::vector<at::Tensor>& inputTensors,
    const AllToAllOptions& opts)
{
    TORCH_CHECK(inputTensors.size() == (size_t)size_,
        "Number of input tensors are not equal to group size");

    TORCH_CHECK(outputTensors.size() == (size_t)size_,
        "Number of output tensors are not equal to group size");

    checkSameType(outputTensors[0], inputTensors);
    checkSameType(inputTensors[0], outputTensors);

    std::vector<size_t> sendCounts(size_);
    std::vector<size_t> recvCounts(size_);

    at::Tensor flatInput;
    at::Tensor flatOutput;

    bool isInputFlat =
        computeLengthsAndCheckAndGetFlat(inputTensors, sendCounts, flatInput);

    bool isOutputFlat =
        computeLengthsAndCheckAndGetFlat(outputTensors, recvCounts, flatOutput);

    if (!isInputFlat)
    {
        auto flatInputSplits =
            flatInput.split_with_sizes(c10::IntArrayRef((int64_t*)sendCounts.data(),
                                       sendCounts.size()), 0);

        for (int i = 0; i < size_; i++)
        {
            flatInputSplits[i].copy_(inputTensors[i].view({-1}));
        }
    }

    std::shared_ptr<ccl::request> req;

    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(req = comm->alltoallv(flatInput.data_ptr(),
                                    sendCounts.data(),
                                    flatOutput.data_ptr(),
                                    recvCounts.data(),
                                    cclDatatypes.at(flatOutput.scalar_type()),
                                    &collAttr));

    std::vector<at::Tensor> a2aTensors;

    if (!isOutputFlat)
    {
        req->wait();

        auto flatOutputSplits =
            flatOutput.split_with_sizes(c10::IntArrayRef((int64_t*)recvCounts.data(),
                                        recvCounts.size()), 0);

        for (int i = 0; i < size_; i++)
        {
            outputTensors[i].view({-1}).copy_(flatOutputSplits[i]);
        }
    }
    else
    {
        a2aTensors.emplace_back(flatOutput);
        a2aTensors.emplace_back(flatInput);
    }

    return std::make_shared<ProcessGroupCCL::WorkCCL>(req, std::move(a2aTensors));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::send(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */,
    int /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support send");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::recv(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */,
    int /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support recv");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::recvAnysource(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */)
{
    throw std::runtime_error("ProcessGroupCCL does not support recvAnysource");
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupCCL::barrier(
    const BarrierOptions& opts)
{
    std::unique_lock<std::mutex> globalLock(globalMutex);
    CCL_CHECK(comm->barrier());
    return std::make_shared<ProcessGroupCCL::WorkCCL>();
}

#ifndef PROCESS_GROUP_CCL_TEST
PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("createProcessGroupCCL", &ProcessGroupCCL::createProcessGroupCCL);
}
#endif

} // namespace c10d