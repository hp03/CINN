// Copyright (c) 2021 CINN Authors. All Rights Reserved.
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

#include "cinn/hlir/pe/reduction.h"

#include <cinn/ir/ir_base.h>

#include <algorithm>

#include "cinn/common/ir_util.h"
#include "cinn/hlir/pe/broadcast.h"
#include "cinn/ir/ir_operators.h"
#include "cinn/ir/tensor.h"
#include "cinn/lang/builtin.h"
#include "cinn/lang/compute.h"

namespace cinn {
namespace hlir {
namespace pe {

using ir::Tensor;
using lang::Compute;

/**
 * @brief transform reduction axes which could be empty or have negative elements into real axes with valid dimension
 * indices.
 *
 * @param ndim Number of dimensions of the output tensor.
 * @param axes The axes parameter.
 * @param real_axes A non-empty sorted array of valid dimension indices, with no duplicates.
 *
 * @notes If the input axes are empty, the result will be axes including all dimensions. If any input element is
 * negative, it will be treated as an offset from the last dimension (same as python indexing rules).
 */
void GetRealAxes(int ndim, const std::vector<int>& axes, std::vector<int>* real_axes) {
  CHECK(real_axes);
  if (axes.empty()) {
    for (int i = 0; i < ndim; ++i) {
      real_axes->push_back(i);
    }
  } else {
    for (auto axis : axes) {
      if (axis < 0) {
        axis += ndim;
      }
      CHECK_LE(axis, ndim) << "exceeds the maximum dimension: " << ndim << std::endl;
      CHECK_GE(axis, 0);
      real_axes->push_back(axis);
    }
    real_axes->resize(std::unique(real_axes->begin(), real_axes->end()) - real_axes->begin());
    std::sort(real_axes->begin(), real_axes->end());
  }
}

/**
 * @brief Calculate the target reduced shape.
 *
 * @param real_axes A non-empty sorted array of valid dimension indices, with no duplicates.
 * @param output_shape The output Tensor shape.
 * @param tensor The input tensor.
 * @param keep_dims If this is set to true, the reduced axes are kept as dimensions with size one. This enables the
 * result to broadcast correctly against the input array.
 */
void GetOutputShape(const std::vector<int>& real_axes,
                    std::vector<Expr>* output_shape,
                    const Tensor& tensor,
                    bool keep_dims) {
  CHECK(output_shape);
  auto ndim = tensor->shape.size();
  if (keep_dims) {
    for (size_t i = 0; i < ndim; ++i) {
      if (std::find(real_axes.begin(), real_axes.end(), i) != real_axes.end()) {
        output_shape->push_back(common::make_one());
      } else {
        output_shape->push_back(tensor->shape[i]);
      }
    }
  } else {
    for (size_t i = 0; i < ndim; ++i) {
      if (std::find(real_axes.begin(), real_axes.end(), i) == real_axes.end()) {
        output_shape->push_back(tensor->shape[i]);
      }
    }
  }
  if (output_shape->empty()) {
    output_shape->push_back(common::make_one());
  }
}

/*!
 * @brief Create a reduction PE.
 *
 * @param tensor The input tensor.
 * @param fn The reduction function eg. ReduceSum
 * @param output_shape The output Tensor shape.
 * @param real_axes The real axes where the reduction is performed.
 * @param squeeze_axes The real axes to squeeze. If unsqueezed, reduced axes will have shape 1 in the output tensor.
 * @param initial Starting value for the sum.
 * @param output_name The name of the output Tensor.
 *
 * @return The result tensor.
 */
template <typename FuncOp>
Tensor DoReduce(const Tensor& tensor,
                const FuncOp& fn,
                const std::vector<Expr>& output_shape,
                const std::vector<int>& real_axes,
                const std::vector<int>& squeeze_axes,
                Expr initial,
                const std::string& output_name) {
  std::vector<Var> reduce_axes;
  for (auto& axis : real_axes) {
    std::string name = UniqName("kk");
    reduce_axes.push_back(Var(tensor->shape[axis], name));
  }
  auto compute = [&](const std::vector<Expr>& indices) -> Expr {
    std::vector<Expr> eval_indice;
    int indice_cnt = 0;
    int reduce_cnt = 0;

    for (size_t i = 0; i < tensor->shape.size(); ++i) {
      bool squeeze_i = std::find(squeeze_axes.begin(), squeeze_axes.end(), i) != squeeze_axes.end();
      if (std::find(real_axes.begin(), real_axes.end(), i) != real_axes.end()) {
        eval_indice.push_back(reduce_axes[reduce_cnt]);
        reduce_cnt++;
        indice_cnt += !squeeze_i;
        continue;
      }
      eval_indice.push_back(indices[indice_cnt]);
      indice_cnt++;
    }
    return fn(tensor(eval_indice), reduce_axes, initial);
  };

  Tensor C = Compute(output_shape, compute, output_name);
  return C;
}

/**
 * @brief reduction PE
 *
 * @param tensor The input tensor.
 * @param axes The axes along which the reduction are performed.
 * @param fn The reduction function eg. ReduceSum
 * @param keep_dims If it is set true, the axes which are reduced are left in the result as dimensions with size one.
 * @param initial Starting value for the sum.
 *
 * @return The result tensor.
 */
template <typename FuncOp>
Tensor Reduce(const Tensor& tensor,
              const std::vector<int>& axes,
              const FuncOp& fn,
              bool keep_dims,
              ir::Expr initial,
              const std::string& output_name) {
  auto ndim = tensor->shape.size();
  CHECK_GT(ndim, 0) << "Reduce tensor's dim must be more than 0";
  std::vector<int> real_axes;
  GetRealAxes(static_cast<int>(ndim), axes, &real_axes);
  std::vector<Expr> output_shapes;
  GetOutputShape(real_axes, &output_shapes, tensor, keep_dims);
  return DoReduce(
      tensor, fn, output_shapes, real_axes, keep_dims ? std::vector<int>() : real_axes, initial, output_name);
}

Tensor ReduceSum(
    const Tensor& A, const std::vector<int>& axes, bool keep_dims, ir::Expr initial, const std::string& output_name) {
  if (!initial.defined()) {
    initial = common::make_const(A->type(), 0);
  }
  return Reduce(A, axes, lang::ReduceSum, keep_dims, initial, output_name);
}

Tensor ReduceProd(
    const Tensor& A, const std::vector<int>& axes, bool keep_dims, ir::Expr initial, const std::string& output_name) {
  if (!initial.defined()) {
    initial = common::make_const(A->type(), 1);
  }
  return Reduce(A, axes, lang::ReduceMul, keep_dims, initial, output_name);
}

Tensor ReduceMax(
    const Tensor& A, const std::vector<int>& axes, bool keep_dims, Expr initial, const std::string& output_name) {
  return Reduce(A, axes, lang::ReduceMax, keep_dims, Expr(), output_name);
}

Tensor ReduceMin(
    const Tensor& A, const std::vector<int>& axes, bool keep_dims, Expr initial, const std::string& output_name) {
  return Reduce(A, axes, lang::ReduceMin, keep_dims, Expr(), output_name);
}

std::vector<Tensor> WarpReduce(const ir::Tensor& A,
                               int last_reduce_dim_num,
                               const std::string& reduce_type,
                               const std::string& output_name) {
  Expr lane(1);
  for (int idx = A->shape.size() - 1; idx >= (A->shape.size() - last_reduce_dim_num); --idx) {
    lane = lane * A->shape[idx].as_int32();
  }

  std::vector<Expr> tmp_shape(A->shape.begin(), A->shape.begin() + A->shape.size() - last_reduce_dim_num);
  tmp_shape.push_back(Expr(32));
  auto tmp_out = Compute(
      tmp_shape,
      [=](const std::vector<Expr>& indexs) -> Expr {
        std::vector<Expr> tmp_indexs(indexs.begin(), indexs.begin() + indexs.size() - 1);
        for (int idx = 0; idx < last_reduce_dim_num; ++idx) {
          tmp_indexs.push_back(Expr(0));
        }
        CHECK_EQ(A->shape.size(), tmp_indexs.size());
        Expr offset = common::IndiceToAbsOffset(A->shape, tmp_indexs);
        return lang::CallExtern(reduce_type, {A, offset, lane});
      },
      UniqName(output_name + "_" + reduce_type));

  std::vector<Expr> out_shape(A->shape.begin(), A->shape.begin() + A->shape.size() - last_reduce_dim_num);
  auto out = Compute(
      out_shape,
      [=](const std::vector<Expr>& indexs) -> Expr {
        std::vector<Expr> tmp_indexs(indexs);
        tmp_indexs.push_back(Expr(0));
        return tmp_out(tmp_indexs);
      },
      UniqName(output_name));

  return {out, tmp_out};
}

/**
 * @brief find the max of array elements over the last dimension
 *
 * @param A The input Tensor
 * @param output_name The name of the output Tensor
 */
std::vector<ir::Tensor> WarpReduceMax(const ir::Tensor& A, int last_reduce_dim_num, const std::string& output_name) {
  return WarpReduce(A, last_reduce_dim_num, "cinn_warp_reduce_max", output_name);
}

/**
 * @brief compute the sum of array elements over the last dimension
 *
 * @param A The input Tensor
 * @param output_name The name of the output Tensor
 */
std::vector<ir::Tensor> WarpReduceSum(const ir::Tensor& A, int last_reduce_dim_num, const std::string& output_name) {
  return WarpReduce(A, last_reduce_dim_num, "cinn_warp_reduce_sum", output_name);
}

/**
 * @brief compute the average of array elements over the last dimension
 *
 * @param A The input Tensor
 * @param output_name The name of the output Tensor
 */
std::vector<ir::Tensor> WarpReduceAvg(const ir::Tensor& A, int last_reduce_dim_num, const std::string& output_name) {
  return WarpReduce(A, last_reduce_dim_num, "cinn_warp_reduce_avg", output_name);
}

}  // namespace pe
}  // namespace hlir
}  // namespace cinn
