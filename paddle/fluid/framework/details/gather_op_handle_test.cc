//   Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/details/gather_op_handle.h"
#include "gtest/gtest.h"

#include "paddle/fluid/platform/device_context.h"

namespace paddle {
namespace framework {
namespace details {
namespace f = paddle::framework;
namespace p = paddle::platform;

// test data amount
const f::DDim kDims = {20, 20};

struct TestGatherOpHandle {
  std::vector<std::unique_ptr<p::DeviceContext>> ctxs_;
  std::vector<Scope*> local_scopes_;
  Scope g_scope_;
  std::unique_ptr<OpHandleBase> op_handle_;
  std::vector<std::unique_ptr<VarHandleBase>> vars_;
  std::vector<p::Place> gpu_list_;

  void WaitAll() {
    for (size_t j = 0; j < ctxs_.size(); ++j) {
      ctxs_[j]->Wait();
    }
  }

  void InitCtxOnGpu(bool use_gpu) {
    if (use_gpu) {
#ifdef PADDLE_WITH_CUDA
      int count = p::GetCUDADeviceCount();
      if (count <= 1) {
        LOG(WARNING) << "Cannot test multi-gpu Broadcast, because the CUDA "
                        "device count is "
                     << count;
        exit(0);
      }
      for (int i = 0; i < count; ++i) {
        auto p = p::CUDAPlace(i);
        gpu_list_.push_back(p);
        ctxs_.emplace_back(new p::CUDADeviceContext(p));
      }
#else
      PADDLE_THROW("CUDA is not support.");
#endif
    } else {
      int count = 8;
      for (int i = 0; i < count; ++i) {
        auto p = p::CPUPlace();
        gpu_list_.push_back(p);
        ctxs_.emplace_back(new p::CPUDeviceContext(p));
      }
    }
  }

  void InitGatherOp(size_t input_scope_idx) {
    for (size_t j = 0; j < gpu_list_.size(); ++j) {
      local_scopes_.push_back(&(g_scope_.NewScope()));
      local_scopes_[j]->Var("out");
    }
    local_scopes_[input_scope_idx]->Var("input");

    op_handle_.reset(new GatherOpHandle(local_scopes_, gpu_list_));
    // add input
    for (size_t j = 0; j < gpu_list_.size(); ++j) {
      op_handle_->dev_ctxes_[gpu_list_[j]] = ctxs_[j].get();
      auto* in_var_handle = new VarHandle(1, j, "input", gpu_list_[j]);
      vars_.emplace_back(in_var_handle);
      op_handle_->AddInput(in_var_handle);
    }

    // add dummy var
    vars_.emplace_back(new DummyVarHandle());
    DummyVarHandle* in_dummy_var_handle =
        static_cast<DummyVarHandle*>(vars_.back().get());
    in_dummy_var_handle->generated_op_ = nullptr;
    op_handle_->AddInput(in_dummy_var_handle);

    // add output
    auto* out_var_handle =
        new VarHandle(2, input_scope_idx, "out", gpu_list_[input_scope_idx]);
    vars_.emplace_back(out_var_handle);
    op_handle_->AddOutput(out_var_handle);

    // add dummy var
    vars_.emplace_back(new DummyVarHandle());
    DummyVarHandle* dummy_var_handle =
        static_cast<DummyVarHandle*>(vars_.back().get());
    op_handle_->AddOutput(dummy_var_handle);
  }

  void TestGatherSelectedRows(size_t output_scope_idx) {
    int height = kDims[0] * 2;
    std::vector<int64_t> rows{0, 1, 2, 3, 3, 0, 14, 7, 3, 1,
                              2, 4, 6, 3, 1, 1, 1,  1, 3, 7};
    std::vector<float> send_vector(f::product(kDims));
    for (size_t k = 0; k < send_vector.size(); ++k) {
      send_vector[k] = k;
    }

    for (size_t input_scope_idx = 0; input_scope_idx < gpu_list_.size();
         ++input_scope_idx) {
      auto in_var = local_scopes_[input_scope_idx]->Var("input");
      auto in_selected_rows = in_var->GetMutable<f::SelectedRows>();
      auto value = in_selected_rows->mutable_value();
      value->mutable_data<float>(kDims, gpu_list_[input_scope_idx]);

      in_selected_rows->set_height(height);
      in_selected_rows->set_rows(rows);

      paddle::framework::TensorFromVector<float>(
          send_vector, *(ctxs_[input_scope_idx]), value);
      value->Resize(kDims);
    }

    auto out_var = local_scopes_[output_scope_idx]->Var("out");
    auto out_selected_rows = out_var->GetMutable<f::SelectedRows>();

    auto in_var = local_scopes_[output_scope_idx]->Var("input");
    auto in_selected_rows = in_var->GetMutable<f::SelectedRows>();

    out_selected_rows->mutable_value()->ShareDataWith(
        in_selected_rows->value());

    op_handle_->Run(false);

    WaitAll();

    p::CPUPlace cpu_place;

    auto& out_select_rows = out_var->Get<f::SelectedRows>();
    auto rt = out_select_rows.value();

    PADDLE_ENFORCE_EQ(out_select_rows.height(), height, "height is not equal.");
    for (size_t k = 0; k < out_select_rows.rows().size(); ++k) {
      PADDLE_ENFORCE_EQ(out_select_rows.rows()[k], rows[k % rows.size()]);
    }

    f::Tensor result_tensor;
    f::TensorCopy(rt, cpu_place, *(ctxs_[output_scope_idx]), &result_tensor);
    float* ct = result_tensor.data<float>();

    for (int64_t j = 0; j < f::product(kDims); ++j) {
      ASSERT_NEAR(ct[j], send_vector[j % send_vector.size()], 1e-5);
    }
  }
};

TEST(GatherTester, TestCPUGatherTestSelectedRows) {
  TestGatherOpHandle test_op;
  size_t input_scope_idx = 0;
  test_op.InitCtxOnGpu(false);
  test_op.InitGatherOp(input_scope_idx);
  test_op.TestGatherSelectedRows(input_scope_idx);
}

#ifdef PADDLE_WITH_CUDA

TEST(GatherTester, TestGPUGatherTestSelectedRows) {
  TestGatherOpHandle test_op;
  size_t input_scope_idx = 0;
  test_op.InitCtxOnGpu(false);
  test_op.InitGatherOp(input_scope_idx);
  test_op.TestGatherSelectedRows(input_scope_idx);
}
#endif
}  // namespace details
}  // namespace framework
}  // namespace paddle
