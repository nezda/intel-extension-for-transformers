//  Copyright (c) 2023 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
#pragma once
#include <functional>
#include <thread>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "jit_blas_utils.h"
#include "jit_blas_device.h"

namespace jblas {
namespace parallel {
struct Config2D {
  int threads;
  int size[2];
  int step[2];
};
struct ThreadProblem2D {
  int tid;
  int tidx[2];
  int loc[2];
  int size[2];
  bool valid;
  void print() {
    printf("Thread %d indice:(%d,%d)\n", tid, tidx[0], tidx[1]);
    printf("Thread location:(%d,%d)\n", loc[0], loc[1]);
    printf("Thread problem size:(%d,%d)\n", size[0], size[1]);
  }
};
class Scheduler2D {
 public:
  Scheduler2D() = default;
  Scheduler2D(const Config2D& config) { update(config); }
  using ThreadProblem = ThreadProblem2D;

  virtual void getIndex(ThreadProblem& problem) const {
    if (problem.tid >= mThdValid) {
      problem.size[0] = 0;
      problem.size[1] = 0;
      problem.valid = false;
      return;
    }
    auto& tid = problem.tid;
    problem.tidx[1] = tid % mThdPerRow;
    problem.tidx[0] = tid / mThdPerRow;
    problem.loc[0] = problem.tidx[0] * mThdSize[0];
    problem.loc[1] = problem.tidx[1] * mThdSize[1];
    problem.size[0] = utils::remainsize(problem.loc[0], mSize[0], mThdSize[0]);
    problem.size[1] = utils::remainsize(problem.loc[1], mSize[1], mThdSize[1]);
    problem.valid = true;
  }

  virtual void update(const Config2D& config) {
    mThdCount = config.threads;
    for (size_t i = 0; i < 2; i++) {
      mSize[i] = config.size[i];
      mStep[i] = config.step[i];
    }
    schedule();
  }

  void print() {
    printf("Thread Block:(%d,%d)\n", mThdSize[0], mThdSize[1]);
    printf("Thread in use:%d of %d, Nx%d\n", mThdValid, mThdCount, mThdPerRow);
  }

  constexpr int* thread_size() { return mThdSize; }

 protected:
  void set(const int* thdsize, const int* size, const int* step) {
    for (size_t i = 0; i < 2; i++) {
      mThdSize[i] = thdsize[i];
      mSize[i] = size[i];
      mStep[i] = step[i];
    }
  }
  void schedule() {
    int rownum = utils::updiv(mSize[0], mStep[0]);
    int colnum = utils::updiv(mSize[1], mStep[1]);
    float ratio = colnum * rownum / static_cast<float>(mThdCount);
    if (ratio <= 1) {
      mThdSize[0] = mStep[0];
      mThdSize[1] = mStep[1];
      mThdPerRow = colnum;
      calc_valid_threads();
      return;
    }
    float colratio = ratio > colnum ? colnum : ceil(ratio);
    mThdSize[1] = static_cast<int>(colratio * mStep[1]);
    mThdPerRow = static_cast<int>(ceil(static_cast<float>(colnum) / colratio));
    mThdSize[0] = static_cast<int>(ceil(rownum / (static_cast<float>(mThdCount) / mThdPerRow)) * mStep[0]);
    calc_valid_threads();
  }
  void calc_valid_threads() {
    mThdValid = mThdPerRow * static_cast<int>(std::ceil(static_cast<float>(mSize[0]) / mThdSize[0]));
  }

  int mThdPerRow = 0;
  int mThdValid = 0;
  int mThdCount = 0;

 private:
  int mThdSize[2] = {0, 0};
  int mSize[2] = {0, 0};
  int mStep[2] = {0, 0};
};

namespace gemm {

struct Config {
  const int threads;
  const utils::GemmProblem problem;
  const size_t l2cache = 1024ULL * 1024;
  const size_t l1cache = 32ULL * 1024;
};

struct ThreadProblemBase : ThreadProblem2D {
  int block[3];
  size_t stacksize;
  size_t tmpcachesize;
};

template <class _GemmCore_T>
class SchedulerBase : public Scheduler2D {
 public:
  using ThreadProblem = ThreadProblemBase;
  SchedulerBase() = default;
  SchedulerBase(const Config& config) { update(config); }
  virtual void getIndex(ThreadProblem& problem) {
    problem.tmpcachesize = mL2Size - mL2Use;
    problem.stacksize = mL2Size;
    problem.block[0] = mBlock[0];
    problem.block[1] = mBlock[1];
    problem.block[2] = mBlock[2];
    Scheduler2D::getIndex(problem);
  }

  virtual void update(const Config& config) {
    for (size_t i = 0; i < 3; i++) {
      mSize[i] = config.problem.dims[i + 1];  // skip 0th batch
      mSizePadded[i] = utils::padto(mSize[i], mStep[i]);
    }
    mThdCount = config.threads;
    mL2Size = config.l2cache;
    mL1Size = config.l1cache;
    if (mSize[0] <= 0 || mSize[1] <= 0 || mSize[2] <= 0) {
      return;
    }
    schedule();
    assert(this->mL2Use <= this->mL2Size - ReservedSize);
    assert(this->mBlock[0] > 0);
    assert(this->mBlock[1] > 0);
    assert(this->mBlock[2] > 0);
  }

  constexpr int valid_theads() { return mThdValid; }

  virtual void print() {
    printf("Thread Block:(%d,%d)\n", mThdSize[0], mThdSize[1]);
    printf("Thread in use:%d of %d, Nx%d\n", mThdValid, mThdCount, mThdPerRow);
    printf("GEMM MStep:%d NStep:%d KStep:%d\n", mBlock[0], mBlock[1], mBlock[2]);
    printf("Cache Size:%zu used:%zu\n", mL2Size, mL2Use);
  }

 protected:
  virtual void schedule() {
    int rownum = utils::updiv(mSize[0], mStep[0]);
    int colnum = utils::updiv(mSize[1], mStep[1]);
    mDensity = static_cast<float>(mSize[0]) * mSize[1] / (mSize[0] + mSize[1]);
    int maxN = 0;
    float maxScore = std::numeric_limits<float>::min();
    int core_enum = static_cast<int>(std::sqrt(mThdCount));
    for (int i = 1; i <= core_enum; i += 1) {
      generate_by_cores(i, mThdCount / i, rownum, colnum);
      auto thdscore = calculate_score();
      if (maxScore < thdscore) {
        maxScore = thdscore;
        maxN = i;
      }
      generate_by_cores(mThdCount / i, i, rownum, colnum);
      thdscore = calculate_score();
      if (maxScore < thdscore) {
        maxScore = thdscore;
        maxN = mThdCount / i;
      }
    }
    generate_by_cores(maxN, mThdCount / maxN, rownum, colnum);
    update_cache_blocking();
    Scheduler2D::set(mThdSize, mSize, mStep);
    mL2Use = static_cast<size_t>(mBlock[0]) * mBlock[1] * mEleSize[2];
    mL2Use += static_cast<size_t>(mBlock[1]) * mBlock[2] * mEleSize[1];
    mL2Use += static_cast<size_t>(mStep[0]) * mBlock[2] * mEleSize[0];
  }
  const float DensityThres = 16;
  static size_t constexpr ReservedSize = 32ULL * 1024ULL;

  virtual float calculate_score() {
    int tmpnstep = mThdSize[1] < _GemmCore_T::PREFERRED_N ? mThdSize[1] : _GemmCore_T::PREFERRED_N;
    float threadratio = static_cast<float>(mThdValid) / mThdCount;
    float density = static_cast<float>(tmpnstep) * mThdSize[0] / (tmpnstep + mThdSize[0]);
    if (mDensity < DensityThres) {
      return threadratio;
    }
    return (threadratio * 1.f + density * 0.0016f);
  }

  virtual void generate_by_cores(int ny, int nx, int rownum, int colnum) {
    mThdSize[0] = utils::updiv(rownum, ny) * mStep[0];
    mThdSize[1] = utils::updiv(colnum, nx) * mStep[1];
    mThdPerRow = utils::updiv(mSize[1], mThdSize[1]);
    mThdValid = utils::updiv(mSize[0], mThdSize[0]) * mThdPerRow;
  }

  // cache = mMStep * mNStep * CSize + mNStep * mKStep * BSize
  //       = mNStep * (mMStep*CSize + mKStep*BSize)
  // C Access = K/mKStep
  // B Access = M/mMStep
  // A Access = N/mNStep
  virtual void update_cache_blocking() {
    if (mDensity <= DensityThres) {
      return cache_blocking_memory();
    } else {
      return cache_blocking_compute();
    }
  }

  virtual void cache_blocking_compute() {
    int constexpr KRef = 256;
    size_t valid_total = mL2Size - ReservedSize;
    auto asize = mStep[0] * KRef * mEleSize[0];
    size_t csize_total = valid_total - _GemmCore_T::PREFERRED_N * KRef * mEleSize[1] - asize;
    int maxM = static_cast<int>(csize_total / _GemmCore_T::PREFERRED_N / mEleSize[2]);
    maxM = utils::downdiv(maxM, mStep[0]);
    int nthdm = mThdSize[0] / mStep[0];
    if (maxM < nthdm) {
      int niter = utils::updiv(nthdm, maxM);
      mBlock[0] = utils::updiv(nthdm, niter) * mStep[0];
    } else {
      mBlock[0] = mThdSize[0];
    }
    int maxN = static_cast<int>((valid_total - asize) / (mBlock[0] * mEleSize[2] + KRef * mEleSize[1]));
    maxN = utils::downdiv(maxN, mStep[1]);
    int nthdn = mThdSize[1] / mStep[1];
    if (maxN < nthdn) {
      int niter = utils::updiv(nthdn, maxN);
      mBlock[1] = utils::updiv(nthdn, niter) * mStep[1];
    } else {
      mBlock[1] = mThdSize[1];
    }
    auto rawk = static_cast<int>((valid_total - mBlock[0] * mBlock[1] * mEleSize[2]) /
                                 (mStep[0] * mEleSize[0] + mBlock[1] * mEleSize[1]));
    rawk = std::min(rawk, mSizePadded[2]);
    mBlock[2] = utils::padto_le(rawk, mStep[2]);
  }

  virtual void cache_blocking_memory() {
    mBlock[0] = mThdSize[0];
    mBlock[1] = mStep[1];
    size_t reservsize = static_cast<size_t>(mBlock[0]) * mBlock[1] * mEleSize[2];
    size_t maxK = (mL1Size - reservsize) / (mBlock[1] * mEleSize[1] + mBlock[0] * mEleSize[0]);
    size_t Bsize = maxK * mBlock[1] * mEleSize[1];
    size_t Bsize_1K = utils::padto_le(Bsize, 1024);
    mBlock[2] = static_cast<int>(Bsize_1K / mEleSize[1] / mBlock[1]);
    mBlock[2] = utils::padto_le(mBlock[2], mStep[2]);
  }

  size_t mL2Size = 0, mL1Size = 0, mL2Use = 0;
  float mDensity = 0.f;

 protected:
  int mSize[3] = {0, 0, 0};
  int mThdSize[3] = {0, 0, 0};
  static constexpr int mStep[3] = {_GemmCore_T::MTILE, _GemmCore_T::NTILE, _GemmCore_T::KTILE};
  static constexpr int mEleSize[3] = {sizeof(typename _GemmCore_T::AType), sizeof(typename _GemmCore_T::BType),
                                      sizeof(typename _GemmCore_T::CType)};
  int mSizePadded[3] = {0, 0, 0};
  int mBlock[3] = {0, 0, 0};
};

template <class _GemmCore_T>
class SchedulerKBlock : public Scheduler2D {
  // Block[2]: block size of K must be mutiplier of mKBlock
  //           or factor of mKBlock
 public:
  using ThreadProblem = ThreadProblemBase;
  SchedulerKBlock() = default;
  SchedulerKBlock(const Config& config) { update(config); }
  virtual void getIndex(ThreadProblem& problem) {
    problem.stacksize = mL2Size;
    problem.tmpcachesize = mL2Size - mL2Use;
    problem.block[0] = mBlock[0];
    problem.block[1] = mBlock[1];
    problem.block[2] = mBlock[2];
    Scheduler2D::getIndex(problem);
  }

  void update(const Config& config) {
    for (size_t i = 0; i < 3; i++) {
      mSize[i] = config.problem.dims[i + 1];
      mSizePadded[i] = utils::padto(mSize[i], mStep[i]);
    }
    mThdCount = config.threads;
    mL2Size = config.l2cache;
    mL1Size = config.l1cache;
    mKBlock = config.problem.dims[4];
    if (mSize[0] <= 0 || mSize[1] <= 0 || mSize[2] <= 0) {
      return;
    }
    schedule();
    assert(this->mL2Use <= this->mL2Size);
    assert(this->mBlock[0] > 0);
    assert(this->mBlock[1] > 0);
    assert(this->mBlock[2] > 0);
  }

  constexpr int valid_theads() { return mThdValid; }

  void print() {
    printf("Thread Block:(%d,%d)\n", mThdSize[0], mThdSize[1]);
    printf("Thread in use:%d of %d, Nx%d\n", mThdValid, mThdCount, mThdPerRow);
    printf("GEMM MStep:%d NStep:%d KStep:%d\n", mBlock[0], mBlock[1], mBlock[2]);
    printf("Cache Size:%zu used:%zu\n", mL2Size, mL2Use);
  }

 protected:
  void schedule() {
    int rownum = utils::updiv(mSize[0], mStep[0]);
    int colnum = utils::updiv(mSize[1], mStep[1]);
    mDensity = static_cast<float>(mSize[0]) * mSize[1] / (mSize[0] + mSize[1]);
    int maxN = 0;
    float maxScore = std::numeric_limits<float>::min();
    int core_enum = static_cast<int>(std::sqrt(mThdCount));
    for (int i = 1; i <= core_enum; i += 1) {
      generate_by_cores(i, mThdCount / i, rownum, colnum);
      auto thdscore = calculate_score();
      if (maxScore < thdscore) {
        maxScore = thdscore;
        maxN = i;
      }
      generate_by_cores(mThdCount / i, i, rownum, colnum);
      thdscore = calculate_score();
      if (maxScore < thdscore) {
        maxScore = thdscore;
        maxN = mThdCount / i;
      }
    }
    generate_by_cores(maxN, mThdCount / maxN, rownum, colnum);
    update_cache_blocking();
    Scheduler2D::set(mThdSize, mSize, mStep);
    mL2Use = static_cast<size_t>(mBlock[0]) * mBlock[1] * mEleSize[2] * 2;
    mL2Use += static_cast<size_t>(mBlock[1]) * mBlock[2] * mEleSize[1];
    mL2Use += static_cast<size_t>(mStep[0]) * mBlock[2] * mEleSize[0];
  }
  const float DensityThres = 16;

  float calculate_score() {
    int tmpnstep = mThdSize[1] < _GemmCore_T::PREFERRED_N ? mThdSize[1] : _GemmCore_T::PREFERRED_N;
    float threadratio = static_cast<float>(mThdValid) / mThdCount;
    float density = static_cast<float>(tmpnstep) * mThdSize[0] / (tmpnstep + mThdSize[0]);
    if (mDensity < DensityThres) {
      return threadratio * 1.f;
    }
    return (threadratio * 1.f + density * 0.0016f);
  }

  void generate_by_cores(int ny, int nx, int rownum, int colnum) {
    mThdSize[0] = utils::updiv(rownum, ny) * mStep[0];
    mThdSize[1] = utils::updiv(colnum, nx) * mStep[1];
    mThdPerRow = utils::updiv(mSize[1], mThdSize[1]);
    mThdValid = utils::updiv(mSize[0], mThdSize[0]) * mThdPerRow;
  }

  // C-KBlock Accumulator=MBlock*NBlock
  // C-K Accumulator=MBlock*NBlock
  // B=MBlock*KBlock
  // A=MTILE*KBlock
  void update_cache_blocking() {
    if (mDensity <= DensityThres) {
      return cache_blocking_memory();
    } else {
      return cache_blocking_compute();
    }
  }

  void cache_blocking_compute() {
    int constexpr KRef = 256;
    int constexpr NRef = _GemmCore_T::PREFERRED_N;
    int constexpr MTile = _GemmCore_T::MTILE;
    int constexpr KSplitStage = 16;
    int BlkNum = utils::updiv(mSize[2], mKBlock);
    int KSplitSize = utils::padto(utils::updiv(mSize[2], KSplitStage), mStep[2]);
    mBlock[1] = NRef < mThdSize[1] ? NRef : mThdSize[1];
    if (KSplitStage * mStep[2] >= mSize[2]) {
      mBlock[2] = mSize[2];
    } else if (KSplitSize >= mKBlock) {
      mBlock[2] = mKBlock;
    } else {
      int scale = utils::downdiv(KSplitStage, BlkNum);
      for (; scale >= 1; scale--) {
        if (mKBlock % scale == 0) {
          break;
        }
      }
      mBlock[2] = utils::downdiv(mKBlock, scale);
      mBlock[2] = utils::padto_le(mBlock[2], mStep[2]);
    }
    size_t size_remain = mL2Size - mBlock[1] * mBlock[2] * mEleSize[1];
    // MBlock*KBlock*ASize+MBlock*NBlock*CSize*2<=size_remain
    int maxMBlock = static_cast<int>(size_remain / (mBlock[1] * mEleSize[2] * 2 + mBlock[2] * mEleSize[0]));
    int maxM = utils::downdiv(maxMBlock, mStep[0]);
    int nthdm = mThdSize[0] / mStep[0];
    if (maxM < nthdm) {
      int niter = utils::updiv(nthdm, maxM);
      mBlock[0] = utils::updiv(nthdm, niter) * mStep[0];
    } else {
      mBlock[0] = mThdSize[0];
    }
  }

  void cache_blocking_memory() {
    mBlock[0] = _GemmCore_T::MTILE;
    size_t startK = std::max(16, _GemmCore_T::KTILE);
    auto getMaxN = [&](size_t refk) {
      size_t sizeA = refk * mEleSize[0] * mBlock[0];
      size_t maxN = (mL1Size - sizeA) / (mBlock[0] * mEleSize[2] * 2 + refk * mEleSize[1]);
      return maxN;
    };
    auto getMaxK = [&](size_t refN) {
      size_t sizeC = refN * mEleSize[2] * mBlock[0] * 2;
      size_t maxK = (mL1Size - sizeC) / (mBlock[0] * mEleSize[0] + refN * mEleSize[1]);
      return maxK;
    };
    auto maxN = getMaxN(startK);
    if (maxN <= mThdSize[1]) {
      mBlock[1] = static_cast<int>(maxN);
      mBlock[1] = utils::padto_le(mBlock[1], mStep[1]);
      mBlock[2] = static_cast<int>(startK);
    } else {
      mBlock[1] = mThdSize[1];
      mBlock[2] = static_cast<int>(getMaxK(mBlock[1]));
      mBlock[2] = utils::padto_le(mBlock[2], mStep[2]);
      mBlock[2] = std::min(mKBlock, mBlock[2]);
      auto tmp = utils::updiv(mKBlock, mBlock[2]);
      while (mKBlock % tmp != 0) tmp++;  // TODO(Yu) optimize
      mBlock[2] = utils::downdiv(mKBlock, tmp);
    }
  }
  size_t mL2Size = 0, mL1Size = 0, mL2Use = 0;
  float mDensity = 0.f;
  int mKBlock = 0;

 private:
  int mSize[3] = {0, 0, 0};
  int mThdSize[3] = {0, 0, 0};
  static constexpr int mStep[3] = {_GemmCore_T::MTILE, _GemmCore_T::NTILE, _GemmCore_T::KTILE};
  static constexpr int mEleSize[3] = {sizeof(typename _GemmCore_T::AType), sizeof(typename _GemmCore_T::BType),
                                      sizeof(typename _GemmCore_T::CType)};
  int mSizePadded[3] = {0, 0, 0};
  int mBlock[3] = {0, 0, 0};
};

template <class _GemmCore_T>
class SchedulerKBlockS : public SchedulerBase<_GemmCore_T> {
  // Block[2]: block size of K must be mutiplier of mKBlock
  //           or factor of mKBlock
 public:
  using ThreadProblem = ThreadProblemBase;
  using BaseScheduler = SchedulerBase<_GemmCore_T>;
  SchedulerKBlockS() = default;
  SchedulerKBlockS(const Config& config) { update(config); }

  void update(const Config& config) {
    mKBlock = config.problem.dims[4];
    BaseScheduler::update(config);
    auto blks = utils::updiv(this->mBlock[2], mKBlock);
    this->mL2Use += static_cast<size_t>(blks) * (this->mBlock[1] + this->mStep[0]) *
                    (sizeof(float) + sizeof(int8_t) + sizeof(float));  // scale+zp+reduce
    assert(this->mL2Use <= this->mL2Size - ReservedSize);
    assert(this->mBlock[0]>0);
    assert(this->mBlock[1]>0);
    assert(this->mBlock[2]>0);
    assert(this->mBlock[2] % _GemmCore_T::KTILE == 0);
  }

 protected:
  const float DensityThres = 16;
  static size_t constexpr ReservedSize = 32ULL * 1024ULL;

  void cache_blocking_compute() override {
    int constexpr KRef = 256;
    int constexpr CorSize = sizeof(float) + sizeof(int8_t) + sizeof(float);
    size_t valid_total = this->mL2Size - ReservedSize;
    auto blks = utils::updiv(KRef, this->mKBlock);
    auto asize = this->mStep[0] * KRef * this->mEleSize[0] + this->mStep[0] * blks * CorSize;
    auto bsize = _GemmCore_T::PREFERRED_N * KRef * this->mEleSize[1] + _GemmCore_T::PREFERRED_N * blks * CorSize;
    size_t csize_total = valid_total - asize - bsize;
    int maxM = static_cast<int>(csize_total / _GemmCore_T::PREFERRED_N / this->mEleSize[2]);
    maxM = utils::downdiv(maxM, this->mStep[0]);
    int nthdm = this->mThdSize[0] / this->mStep[0];
    if (maxM < nthdm) {
      int niter = utils::updiv(nthdm, maxM);
      this->mBlock[0] = utils::updiv(nthdm, niter) * this->mStep[0];
    } else {
      this->mBlock[0] = this->mThdSize[0];
    }
    int maxN = static_cast<int>((valid_total - asize) /
                                (this->mBlock[0] * this->mEleSize[2] + KRef * this->mEleSize[1] + blks * CorSize));
    maxN = utils::downdiv(maxN, this->mStep[1]);
    int nthdn = this->mThdSize[1] / this->mStep[1];
    if (maxN < nthdn) {
      int niter = utils::updiv(nthdn, maxN);
      this->mBlock[1] = utils::updiv(nthdn, niter) * this->mStep[1];
    } else {
      this->mBlock[1] = this->mThdSize[1];
    }
    auto rawk = static_cast<int>((valid_total - this->mBlock[0] * this->mBlock[1] * this->mEleSize[2]) /
                                 (this->mStep[0] * this->mEleSize[0] +
                                  float(CorSize * (this->mStep[0] + this->mBlock[1])) / this->mKBlock +
                                  this->mBlock[1] * this->mEleSize[1]));
    if (rawk < this->mKBlock) {
      rawk = static_cast<int>((valid_total - this->mBlock[0] * this->mBlock[1] * this->mEleSize[2] -
                               1 * CorSize * (this->mStep[0] + this->mBlock[1])) /
                              (this->mStep[0] * this->mEleSize[0] + this->mBlock[1] * this->mEleSize[1]));
    }
    rawk = std::min(rawk, this->mSizePadded[2]);
    this->mBlock[2] = utils::padto_le(rawk, this->mStep[2]);
    if (this->mBlock[2] > this->mKBlock) {
      this->mBlock[2] = utils::padto_le(this->mBlock[2], this->mKBlock);
    }
  }

  void cache_blocking_memory() override {
    this->mBlock[0] = _GemmCore_T::MTILE;
    size_t startK = std::max(16, _GemmCore_T::KTILE);
    auto getMaxN = [&](size_t refk) {
      size_t sizeA = refk * this->mEleSize[0] * this->mBlock[0];
      auto blks = utils::updiv(refk, mKBlock);
      sizeA += blks * this->mBlock[0] * (sizeof(float) + sizeof(uint8_t));
      size_t maxN = (this->mL1Size - sizeA) / (this->mBlock[0] * this->mEleSize[2] + refk * this->mEleSize[1]);
      return maxN;
    };
    auto getMaxK = [&](size_t refN) {
      size_t sizeC = refN * this->mEleSize[2] * this->mBlock[0];
      size_t maxK = (this->mL1Size - sizeC) / (this->mBlock[0] * this->mEleSize[0] + refN * this->mEleSize[1]);
      return maxK;
    };
    if (mKBlock <= 32) {
      this->mBlock[2] = mKBlock;
      auto maxN = getMaxN(startK);
      this->mBlock[1] = static_cast<int>(maxN);
      this->mBlock[1] = std::min(this->mBlock[1], this->mThdSize[1]);
      this->mBlock[1] = utils::padto_le(this->mBlock[1], this->mStep[1]);
      return;
    }
    auto maxN = getMaxN(startK);
    if (maxN <= this->mThdSize[1]) {
      this->mBlock[1] = static_cast<int>(maxN);
      this->mBlock[1] = utils::padto_le(this->mBlock[1], this->mStep[1]);
      this->mBlock[2] = static_cast<int>(startK);
    } else {
      this->mBlock[1] = this->mThdSize[1];
      this->mBlock[2] = static_cast<int>(getMaxK(this->mBlock[1]));
      this->mBlock[2] = utils::padto_le(this->mBlock[2], this->mStep[2]);
      this->mBlock[2] = std::min(mKBlock, this->mBlock[2]);
    }
  }

  int mKBlock{0};
};

}  // namespace gemm
using thread_func = std::function<void(int tid)>;

class IThreading {
 public:
  explicit IThreading(int nthreads) : mThreadNum(nthreads) {}
  virtual void parallel_for(const thread_func& func) const = 0;
  virtual inline void sync() const = 0;
  virtual int num_threads() const { return mThreadNum; };
  virtual void set_threads(int nthreads) = 0;

 protected:
  int mThreadNum;
};
#ifdef _OPENMP
class OMPThreading : public IThreading {
 public:
  explicit OMPThreading(int nthreads) : IThreading(nthreads) { omp_set_num_threads(nthreads); }
  void parallel_for(const thread_func& func) const override {
#pragma omp parallel
    {
      int tidx = omp_get_thread_num();
      func(tidx);
    }
  }
  virtual void set_threads(int nthreads) override {
    mThreadNum = nthreads;
    omp_set_num_threads(nthreads);
  }
  virtual inline void sync() const override {
#pragma omp barrier
    (void)(0);  // make msvc happy with c++20
  }
};
#endif

class StdThreading : public IThreading {
 public:
  explicit StdThreading(int nthreads) : IThreading(nthreads) {}
  void parallel_for(const thread_func& func) const override {
    if (mThreadNum > 1) {
      std::vector<std::thread> thdset(mThreadNum - 1);
      for (size_t i = 0; i < mThreadNum - 1; i++) {
        thdset[i] = std::thread([&](int tidx) { func(tidx); }, int(i + 1));
      }
      func(0);
      for (size_t i = 0; i < mThreadNum - 1; i++) {
        thdset[i].join();
      }
    } else {
      func(0);
    }
  }

  void set_threads(int nthreads) override { mThreadNum = nthreads; }

  inline void sync() const override { assert(0); }

 private:
};

class SingleThread : public StdThreading {
 public:
  SingleThread() : StdThreading(1) {}

  void set_threads(int nthreads) override { (void)(nthreads); }

  inline void sync() const override {}
};

template <class Parallel_T, class Launch_T>
void GemmRun(Launch_T& launcher, const typename Launch_T::Param& args, parallel::IThreading* th) {
  device::CpuBase cb;
  Parallel_T para({th->num_threads(), args.problem, cb.mL2Cache, cb.mL1Cache});
  static bool flag = false;
  if (flag) {
    printf("%s\n", __FUNCTION__);
    para.print();
    flag = false;
  }
  th->parallel_for([&](int tidx) {
    typename Parallel_T::ThreadProblem thdp{tidx};
    para.getIndex(thdp);
    if (thdp.valid) {
      launcher.run(args, thdp);
    }
  });
}

template <class Parallel_T, class Launch_T>
void GemmRunWithA(Launch_T& launcher, const typename Launch_T::Param& args, parallel::IThreading* th) {
  device::CpuBase cb;
  Parallel_T para({th->num_threads(), args.problem, cb.mL2Cache, cb.mL1Cache});
  using AParall = typename Launch_T::PrologueA::Parallel;
  auto apara = launcher.mProA.createParallel(th->num_threads(), args.problem);
  static bool flag = false;
  if (flag) {
    printf("%s\n", __FUNCTION__);
    para.print();
    flag = false;
  }
  th->parallel_for([&](int tidx) {
    typename AParall::ThreadProblem thdpA{tidx};
    apara.getIndex(thdpA);
    if (thdpA.valid) {
      launcher.mProA.run(args.paramA, thdpA);
    }
    th->sync();
    typename Parallel_T::ThreadProblem thdp{tidx};
    para.getIndex(thdp);
    if (thdp.valid) {
      launcher.run(args, thdp);
    }
  });
}

}  // namespace parallel
}  // namespace jblas
