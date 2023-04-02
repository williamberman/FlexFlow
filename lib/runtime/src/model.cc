/* Copyright 2023 CMU, Facebook, LANL, MIT, NVIDIA, and Stanford (alphabetical)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "model.h"
/* #if defined(FF_USE_CUDA) || defined(FF_USE_HIP_CUDA) */
/* #include "flexflow/utils/cuda_helper.h" */
/* #else */
/* #include "utils/hip_helper.h" */
/* #endif */
#include "op-attrs/ffconst_utils.h"
#include "mapper.h"
#include "ops/aggregate.h"
#include "ops/aggregate_spec.h"
#include "ops/attention.h"
#include "ops/batch_matmul.h"
#include "ops/batch_norm.h"
#include "ops/cast.h"
#include "ops/concat.h"
#include "ops/conv_2d.h"
#include "ops/dropout.h"
#include "ops/element_binary.h"
#include "ops/element_unary.h"
#include "ops/embedding.h"
#include "ops/flat.h"
#include "ops/fused.h"
#include "ops/gather.h"
#include "ops/groupby.h"
#include "ops/layer_norm.h"
#include "ops/linear.h"
#include "ops/noop.h"
#include "ops/pool_2d.h"
#include "ops/reduce.h"
#include "ops/reshape.h"
#include "ops/reverse.h"
#include "ops/softmax.h"
#include "ops/split.h"
#include "ops/topk.h"
#include "ops/transpose.h"
#include "parallel_ops/combine.h"
#include "parallel_ops/fused_parallel_op.h"
#include "parallel_ops/partition.h"
#include "parallel_ops/reduction.h"
#include "parallel_ops/replicate.h"
#include "utils/random_utils.h"
#include "test_utils.h"
#include "legion/legion_utilities.h"
#include <dirent.h>
#include <queue>
#include <unordered_set>

namespace FlexFlow {

using namespace Legion;

LegionRuntime::Logger::Category log_model("Model");
LegionRuntime::Logger::Category log_measure("measure");
LegionRuntime::Logger::Category log_profile("profile");


/* std::unordered_map<int, int> output_to_input_mapping( */
/*     std::vector<ParallelDimMappingRecord> const &mapping) { */
/*   std::unordered_map<int, int> dim_mapping; */
/*   for (ParallelDimMappingRecord const &record : mapping) { */
/*     if (record.get_type() == MappingRecordType::INPUT_OUTPUT) { */
/*       dim_mapping[record.output_dim] = record.input_dim; */
/*     } */
/*   } */

/*   return dim_mapping; */
/* } */

/* std::unordered_map<int, int> input_to_output_mapping( */
/*     std::vector<ParallelDimMappingRecord> const &mapping) { */
/*   std::unordered_map<int, int> dim_mapping; */
/*   for (ParallelDimMappingRecord const &record : mapping) { */
/*     if (record.get_type() == MappingRecordType::INPUT_OUTPUT) { */
/*       dim_mapping[record.input_dim] = record.output_dim; */
/*     } */
/*   } */

/*   return dim_mapping; */
/* } */

FFModel::FFModel(FFConfig &_config)
    : op_global_guid(OP_GUID_FIRST_VALID),
      layer_global_guid(LAYER_GUID_FIRST_VALID),
      tensor_global_guid(TENSOR_GUID_FIRST_VALID),
      parallel_tensor_global_guid(PARALLEL_TENSOR_GUID_FIRST_VALID),
      node_global_guid(NODE_GUID_FIRST_VALID), config(_config), optimizer(NULL),
      loss_op(NULL), metrics_op(NULL), simulator(NULL) {
  Runtime *runtime = config.lg_hlr;
  Context ctx = config.lg_ctx;
  // Register machine views
  register_all_machine_views(config.numNodes,
                             config.workersPerNode,
                             config.cpusPerNode,
                             all_valid_views);
  metrics_input = -1;
  // Load strategy file
  // Create field space
  {
    FieldAllocator allocator =
        runtime->create_field_allocator(ctx, config.field_space);
    allocator.allocate_field(sizeof(float), FID_DATA);
  }
  // Build training dataset
  // if (config.datasetPath.length() == 0) {
  //  dataLoader = NULL;
  //} else {
  //  dataLoader = new DataLoader(config.datasetPath);
  //}

  ArgumentMap argmap;
  Rect<1> task_rect(Point<1>(0),
                    Point<1>(config.workersPerNode * config.numNodes - 1));
  IndexSpaceT<1> task_is = runtime->create_index_space(ctx, task_rect);

  // int rank = 0;
  for (PointInRectIterator<1> it(task_rect); it(); it++) {
    FFInitInfo info;
    // info.myRank = rank++;
    // info.allRanks = config.workersPerNode * config.numNodes;
    info.workSpaceSize = config.workSpaceSize;
    info.allowTensorOpMathConversion = config.allow_tensor_op_math_conversion;
    argmap.set_point(*it, TaskArgument(&info, sizeof(FFInitInfo)));
  }

  // Init CUDA library on each worker
  IndexLauncher initLauncher(FF_INIT_TASK_ID,
                             task_is,
                             TaskArgument(NULL, 0),
                             argmap,
                             Predicate::TRUE_PRED,
                             false /*must*/,
                             0 /*mapper_id*/,
                             FFConfig::DataParallelism_GPU);
  FutureMap fm = runtime->execute_index_space(ctx, initLauncher);
  fm.wait_all_results();
  int idx = 0;
  for (PointInRectIterator<1> it(task_rect); it(); it++) {
    handlers[idx++] = fm.get_result<FFHandler>(*it);
  }
}

Tensor FFModel::aggregate(
    Tensor const *inputs, /* gate_preds, gate_assign, gate assign TopK,
                             full_gate_pred, exp_pred_1, ... , exp_pred_n */
    int n,
    float lambda_bal,
    char const *name) {
  Layer *li = new Layer(this,
                        OP_AGGREGATE,
                        DT_FLOAT,
                        name,
                        n + 4 /*inputs*/,
                        0 /*weights*/,
                        1 /*outputs*/,
                        inputs);
  {
    int num_dim = inputs[4]->num_dims;
    // Set output shape
    int dims[MAX_TENSOR_DIM];
    for (int i = 0; i < num_dim - 1; i++) {
      dims[i] = inputs[4]->dims[i];
    }
    dims[num_dim - 1] = inputs[0]->dims[num_dim - 1];
    li->outputs[0] = create_tensor_legion_ordering(
        num_dim, dims, DT_FLOAT, li, 0, true /*create_grad*/);
  }
  li->add_int_property("n", n);
  li->add_float_property("lambda_bal", lambda_bal);
  layers.push_back(li);
  return li->outputs[0];
}


#ifdef FF_USE_NCCL
ncclComm_t *FFModel::find_nccl_comms(MachineView const &view) const {
  auto const &it = view_hash_to_nccl_comms.find(view.hash());
  if (it == view_hash_to_nccl_comms.end()) {
    assert(config.computationMode == COMP_MODE_INFERENCE);
    return NULL;
  } else {
    return it->second;
  }
}
#endif

template <int NDIM>
Tensor FFModel::create_constant(int const dims[],
                                float value,
                                DataType data_type) {
  // FIXME: currently create gradients for constants since the current auto grad
  // algorithm computes gradients for all operators
  Tensor tensor = create_tensor<NDIM>(
      dims, data_type, NULL /*owner_op*/, false /*create_grad*/);
  tensor->initializer = new ConstantInitializer(value);
  return tensor;
#ifdef DEADCODE
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  assert(false);
  ArgumentMap argmap;
  IndexLauncher launcher(CONSTANT_INIT_TASK_ID,
                         tensor->parallel_is,
                         TaskArgument(init, sizeof(ConstantInitializer)),
                         argmap,
                         Predicate::TRUE_PRED,
                         false,
                         0,
                         tensor->machine_view.hash());
  launcher.add_region_requirement(RegionRequirement(tensor->part,
                                                    0 /*projection id*/,
                                                    WRITE_ONLY,
                                                    EXCLUSIVE,
                                                    tensor->region));
  launcher.add_field(0, FID_DATA);
  FutureMap fm = runtime->execute_index_space(ctx, launcher);
  fm.wait_all_results();
  return tensor;
#endif
}

PCG::Node FFModel::new_node(Op *op) {
  PCG::Node ret;
  ret.guid = this->node_global_guid++;
  ret.ptr = op;

  return ret;
}

Tensor FFModel::create_tensor(int numdim,
                              int const dims[],
                              DataType data_type,
                              Layer const *layer,
                              int idx,
                              bool create_grad) {
  switch (numdim) {
#define DIMFUNC(DIM)                                                           \
  case DIM:                                                                    \
    return create_tensor<DIM>(dims, data_type, layer, idx, create_grad);
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default:
      assert(false && "Unsupported dim!");
  }
}

ParallelTensor FFModel::create_parallel_tensor(int numdim,
                                               const ParallelDim dims[],
                                               DataType data_type,
                                               Op const *op,
                                               int idx,
                                               bool create_grad,
                                               size_t input_tensor_guid) {
  switch (numdim) {
#define DIMFUNC(DIM)                                                           \
  case DIM:                                                                    \
    return create_parallel_tensor<DIM>(                                        \
        dims, data_type, op, idx, create_grad, input_tensor_guid);
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default:
      assert(false && "Unsupported dim!");
  }
}

Tensor FFModel::create_tensor_legion_ordering(int numdim,
                                              int const dims[],
                                              DataType data_type,
                                              Layer const *layer,
                                              int idx,
                                              bool create_grad) {
  int c_dims[MAX_TENSOR_DIM];
  for (int i = 0; i < numdim; i++) {
    c_dims[i] = dims[numdim - 1 - i];
  }
  return create_tensor(numdim, c_dims, data_type, layer, idx, create_grad);
}

ParallelTensor
    FFModel::create_parallel_tensor_legion_ordering(int numdim,
                                                    const ParallelDim dims[],
                                                    DataType data_type,
                                                    Op const *op,
                                                    int idx,
                                                    bool create_grad,
                                                    size_t input_tensor_guid) {
  ParallelDim c_dims[MAX_TENSOR_DIM];
  for (int i = 0; i < numdim; i++) {
    c_dims[i] = dims[numdim - 1 - i];
  }
  return create_parallel_tensor(
      numdim, c_dims, data_type, op, idx, create_grad, input_tensor_guid);
}

template <int NDIM>
Tensor FFModel::create_tensor(int const dims[],
                              DataType data_type,
                              Layer const *owner_layer,
                              int owner_idx,
                              bool create_grad) {
  Tensor tensor = new TensorBase();
  tensor->tensor_guid = tensor_global_guid++;
  tensor->data_type = data_type;
  if (owner_layer == NULL) {
    Layer *input_layer = new Layer(this,
                                   OP_INPUT,
                                   data_type,
                                   "input",
                                   0 /*inputs*/,
                                   0 /*weight*/,
                                   1 /*outputs*/,
                                   NULL,
                                   NULL);
    input_layer->outputs[0] = tensor;
    layers.push_back(input_layer);
    tensor->owner_layer = input_layer;
    tensor->owner_idx = 0;
  } else {
    tensor->owner_layer = owner_layer;
    tensor->owner_idx = owner_idx;
  }
  tensor->create_gradients = create_grad;
  tensor->num_dims = NDIM;
  for (int i = 0; i < NDIM; i++) {
    tensor->dims[i] = dims[NDIM - 1 - i];
  }
  return tensor;
}

template <int NDIM>
ParallelTensor FFModel::create_parallel_tensor(const ParallelDim dims[],
                                               DataType data_type,
                                               Op const *owner_op,
                                               int owner_idx,
                                               bool create_grad,
                                               size_t input_tensor_guid) {
  ParallelTensor tensor = new ParallelTensorBase();
  tensor->parallel_tensor_guid = parallel_tensor_global_guid++;
  tensor->data_type = data_type;
  if (owner_op == nullptr) {
    NoOp *input_op = new NoOp(*this, OP_INPUT, input_tensor_guid, tensor);
    operators.push_back(input_op);
    tensor->owner_op = input_op;
    tensor->owner_idx = 0;
  } else {
    tensor->owner_op = owner_op;
    tensor->owner_idx = owner_idx;
  }
  tensor->create_gradients = create_grad;
  tensor->num_dims = NDIM;
  for (int i = 0; i < NDIM; i++) {
    tensor->dims[i] = dims[NDIM - 1 - i];
  }
  assert(tensor->check_valid());
  return tensor;
}

Parameter FFModel::create_weight_legion_ordering(int numdim,
                                                 int const dims[],
                                                 DataType data_type,
                                                 Layer const *layer,
                                                 bool create_grad,
                                                 Initializer *initializer,
                                                 ParameterSyncType sync_type) {
  int c_dims[MAX_TENSOR_DIM];
  for (int i = 0; i < numdim; i++) {
    c_dims[i] = dims[numdim - 1 - i];
  }
  return create_weight(
      numdim, c_dims, data_type, layer, create_grad, initializer, sync_type);
}

Parameter FFModel::create_weight(int numdim,
                                 int const dims[],
                                 DataType data_type,
                                 Layer const *owner_layer,
                                 bool create_grad,
                                 Initializer *initializer,
                                 ParameterSyncType sync_type) {
  Parameter p = new TensorBase();
  p->data_type = data_type;
  assert(owner_layer != NULL);
  if (owner_layer == NULL) {
    Layer *weight_layer = new Layer(this,
                                    OP_WEIGHT,
                                    data_type,
                                    NULL,
                                    0 /*inputs*/,
                                    0 /*weights*/,
                                    1 /*outputs*/,
                                    NULL /*in1*/,
                                    NULL /*in2*/);
    layers.push_back(weight_layer);
    p->owner_layer = weight_layer;
    p->owner_idx = 0;
  } else {
    p->owner_layer = owner_layer;
    p->owner_idx = 0;
  }
  p->create_gradients = create_grad;
  p->initializer = initializer;
  p->sync_type = sync_type;
  p->num_dims = numdim;
  for (int i = 0; i < numdim; i++) {
    p->dims[i] = dims[numdim - 1 - i];
  }
  assert(p->get_volume() > 0);
  return p;
}

template <int NDIM>
ParallelParameter FFModel::create_parallel_weight(const ParallelDim dims[],
                                                  DataType data_type,
                                                  Op const *owner_op,
                                                  bool create_grad,
                                                  Initializer *initializer,
                                                  ParameterSyncType sync_type) {
  ParallelParameter p = new ParallelTensorBase();
  p->parallel_tensor_guid = parallel_tensor_global_guid++;
  p->data_type = data_type;
  if (owner_op == NULL) {
    NoOp *weight_op = new NoOp(*this, OP_WEIGHT, p);
    operators.push_back(weight_op);
    p->owner_op = weight_op;
    p->owner_idx = 0;
  } else {
    p->owner_op = owner_op;
  }
  p->create_gradients = create_grad;
  p->initializer = initializer;
  p->sync_type = sync_type;
  p->num_dims = NDIM;
  for (int i = 0; i < NDIM; i++) {
    p->dims[i] = dims[NDIM - 1 - i];
  }
  assert(p->get_volume() > 0);
  assert(p->check_valid());
  return p;
}

ParallelParameter FFModel::create_parallel_weight(int numdim,
                                                  const ParallelDim dims[],
                                                  DataType data_type,
                                                  Op const *owner_op,
                                                  bool create_grad,
                                                  Initializer *initializer,
                                                  ParameterSyncType sync_type) {
  switch (numdim) {
#define DIMFUNC(DIM)                                                           \
  case DIM:                                                                    \
    return create_parallel_weight<DIM>(                                        \
        dims, data_type, owner_op, create_grad, initializer, sync_type);
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default:
      assert(false && "Unsupported dim!");
  }
}

ParallelParameter FFModel::create_parallel_weight_legion_ordering(
    int numdim,
    const ParallelDim dims[],
    DataType data_type,
    Op const *owner_op,
    bool create_grad,
    Initializer *initializer,
    ParameterSyncType sync_type) {
  ParallelDim c_dims[MAX_TENSOR_DIM];
  std::reverse_copy(dims, dims + numdim, c_dims);

  return this->create_parallel_weight(
      numdim, c_dims, data_type, owner_op, create_grad, initializer, sync_type);
}

void FFModel::map_tensor(ParallelTensor tensor, Op const *op) {
  switch (tensor->num_dims) {
#define DIMFUNC(NDIM)                                                          \
  case NDIM: {                                                                 \
    map_tensor_with_dim<NDIM>(tensor, op);                                     \
    break;                                                                     \
  }
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default: {
      // Unsupported dim
      assert(false);
    }
  }
}

// Map tensor using parallelization strategies described in parallel_op
template <int NDIM>
void FFModel::map_tensor_with_dim(ParallelTensor tensor,
                                  Op const *parallel_op) {
  tensor->parallel_is = get_or_create_task_is(tensor);
  assert(tensor->owner_op != NULL);
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  Domain task_domain =
      runtime->get_index_space_domain(ctx, tensor->parallel_is);
  switch (task_domain.get_dim()) {
#define DIMFUNC(TDIM)                                                          \
  case TDIM: {                                                                 \
    map_tensor_with_dim2<NDIM, TDIM>(tensor, parallel_op);                     \
    break;                                                                     \
  }
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default: {
      assert(false && "Unsupported Task Dim");
    }
  }
}

template <int NDIM, int TDIM>
void FFModel::map_tensor_with_dim2(ParallelTensor tensor,
                                   Op const *parallel_op) {
  // Step 0: check we are the owner or the owner is NULL
  // in which case set the owner to us
  if (tensor->owner_op == NULL) {
    tensor->owner_op = parallel_op;
    tensor->owner_idx = -1; // meaning tensor is not an output of op
  } else {
    // assert tensor->owner_op == parallel_op or parallel_op == nullptr,
    // which indicates the tensor is not parallelized
    assert(tensor->owner_op == parallel_op || parallel_op == nullptr);
  }
  // Step 1: create regions
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;

  FieldSpace fs = runtime->create_field_space(ctx);
  FieldAllocator allocator = runtime->create_field_allocator(ctx, fs);
  switch (tensor->data_type) {
    case DT_HALF:
      allocator.allocate_field(sizeof(half), FID_DATA);
      break;
    case DT_FLOAT:
      allocator.allocate_field(sizeof(float), FID_DATA);
      break;
    case DT_DOUBLE:
      allocator.allocate_field(sizeof(double), FID_DATA);
      break;
    case DT_INT32:
      allocator.allocate_field(sizeof(int32_t), FID_DATA);
      break;
    case DT_INT64:
      allocator.allocate_field(sizeof(int64_t), FID_DATA);
      break;
    default:
      assert(false);
  }

  Point<NDIM> hi;
  for (int i = 0; i < NDIM; i++) {
    hi[i] = tensor->dims[i].size - 1;
  }
  Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
  IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
  tensor->region = runtime->create_logical_region(ctx, is, fs);
  if (tensor->create_gradients &&
      config.computationMode == COMP_MODE_TRAINING) {
    tensor->region_grad = runtime->create_logical_region(ctx, is, fs);
  }

  // Step 2: create partitions if parallel_op != NULL
  if (parallel_op != NULL) {
    IndexSpaceT<TDIM> part_is =
        (IndexSpaceT<TDIM>)get_or_create_task_is(tensor);
    // Rect<TDIM> part_rect = runtime->get_index_space_domain(ctx, part_is);
    Transform<NDIM, TDIM> transform;
    Point<NDIM> ext_hi;
    for (int i = 0; i < NDIM; i++) {
      int nparts = tensor->dims[i].degree;
      ext_hi[i] = (rect.hi[i] - rect.lo[i] + nparts) / nparts - 1;
    }
    Rect<NDIM> extent(Point<NDIM>::ZEROES(), ext_hi);
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < TDIM; j++) {
        if (tensor->dims[i].parallel_idx == j) {
          transform[i][j] = extent.hi[i] - extent.lo[i] + 1;
        } else {
          transform[i][j] = 0;
        }
      }
    }
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, part_is, transform, extent);
    assert(runtime->is_index_partition_disjoint(ctx, ip));
    assert(runtime->is_index_partition_complete(ctx, ip));
    tensor->part = runtime->get_logical_partition(ctx, tensor->region, ip);
    if (tensor->create_gradients &&
        config.computationMode == COMP_MODE_TRAINING) {
      tensor->part_grad =
          runtime->get_logical_partition(ctx, tensor->region_grad, ip);
    }
  }
  // Step 3: initialize the tensor
  if (tensor->initializer != NULL) {
    tensor->initializer->init(this, tensor);
  }
}

void FFModel::map_weight(ParallelTensor weight, Op const *op) {
  switch (weight->num_dims) {
#define DIMFUNC(DIM)                                                           \
  case DIM: {                                                                  \
    map_weight_with_dim<DIM>(weight, op);                                      \
    break;                                                                     \
  }
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default: {
      // Unsupported dim
      assert(false);
    }
  }
}

template <int NDIM>
void FFModel::map_weight_with_dim(ParallelTensor weight,
                                  Op const *parallel_op) {
  // Step 0: check we are the owner or the owner is NULL
  // in which case set the owner to us
  if (weight->owner_op == NULL) {
    weight->owner_op = parallel_op;
    weight->owner_idx = -1; // meaning tensor is not an output of op
  } else {
    assert(weight->owner_op == parallel_op);
  }
  assert(parallel_op != NULL);
  int tdim = parallel_op->outputs[0]->num_dims;
  switch (parallel_op->op_type) {
    case OP_LINEAR:
    case OP_EMBEDDING:
    case OP_MULTIHEAD_ATTENTION: {
      switch (tdim) {
#define DIMFUNC(TDIM)                                                          \
  case TDIM: {                                                                 \
    map_linear_weight<NDIM, TDIM>(weight, parallel_op);                        \
    break;                                                                     \
  }
        LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
        default: {
          assert(false);
        }
      }
      break;
    }
    case OP_CONV2D:
    case OP_BATCHNORM: {
      map_conv_weight<NDIM>(weight, parallel_op);
      break;
    }
    default: {
      fprintf(stderr,
              "FlexFlow currently does not support this weight"
              "type (%d). Report the error to the FlexFlow team.\n",
              parallel_op->op_type);
      assert(false && "Unsupported type for mapping weight");
    }
  }
}

bool FFModel::get_parallel_tensor_from_tensor(
    const Tensor tensor, ParallelTensor &parallel_tensor) const {
  // check if tensor->parallel_tensor is already set
  if (tensor->parallel_tensor != nullptr) {
    parallel_tensor = tensor->parallel_tensor;
    return true;
  }
  if (tensor->owner_layer != nullptr) {
    Op *mapped_op = nullptr;
    if (tensor->owner_layer->op_type == OP_INPUT) {
      // We use tensor_guid to match input operators
      size_t tensor_guid = tensor->owner_layer->outputs[0]->tensor_guid;
      for (auto const &op : operators) {
        if (op->op_type == OP_INPUT) {
          if (tensor_guid == ((NoOp *)op)->input_tensor_guid) {
            assert(mapped_op == nullptr);
            mapped_op = op;
          }
        }
      }
    } else {
      for (auto const &op : operators) {
        if (op->layer_guid == tensor->owner_layer->layer_guid) {
          assert(mapped_op == nullptr);
          mapped_op = op;
        }
      }
    }
    if (mapped_op != nullptr) {
      parallel_tensor = mapped_op->outputs[tensor->owner_idx];
      return true;
    }
  }
  assert(false);
  return true;
}

void FFModel::create_disjoint_partition(int num_dims,
                                        const ParallelDim dims[],
                                        IndexSpace const &part_is,
                                        LogicalRegion const &region,
                                        LogicalPartition &part) {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  Domain task_domain = runtime->get_index_space_domain(ctx, part_is);
  switch ((num_dims - 1) * MAX_TENSOR_DIM + task_domain.get_dim() - 1) {
#define DIMFUNC(NDIM, TDIM)                                                    \
  case (NDIM - 1) * MAX_TENSOR_DIM + (TDIM - 1): {                             \
    IndexSpaceT<TDIM> part_is_t(part_is);                                      \
    return create_disjoint_partition_with_dim2<NDIM, TDIM>(                    \
        dims, part_is_t, region, part);                                        \
  }
    LEGION_FOREACH_NN(DIMFUNC)
#undef DIMFUNC
    default:
      assert(false && "Unsupported NDIM/TDIM");
  }
}

template <int NDIM, int TDIM>
void FFModel::create_disjoint_partition_with_dim2(
    const ParallelDim dims[],
    IndexSpaceT<TDIM> const &part_is,
    LogicalRegion const &region,
    LogicalPartition &part) {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  // Rect<NDIM> part_rect = runtime->get_index_space_domain(ctx, part_is);
  Transform<NDIM, TDIM> transform;
  Point<NDIM> ext_hi;
  Rect<NDIM> rect =
      runtime->get_index_space_domain(ctx, region.get_index_space());
  for (int i = 0; i < NDIM; i++) {
    int nparts = dims[i].degree;
    ext_hi[i] = (rect.hi[i] - rect.lo[i] + nparts) / nparts - 1;
  }
  Rect<NDIM> extent(Point<NDIM>::ZEROES(), ext_hi);
  for (int i = 0; i < NDIM; i++) {
    for (int j = 0; j < TDIM; j++) {
      if (dims[i].parallel_idx == j) {
        transform[i][j] = extent.hi[i] - extent.lo[i] + 1;
      } else {
        transform[i][j] = 0;
      }
    }
  }
  IndexPartition ip = runtime->create_partition_by_restriction(
      ctx, region.get_index_space(), part_is, transform, extent);
  assert(runtime->is_index_partition_disjoint(ctx, ip));
  assert(runtime->is_index_partition_complete(ctx, ip));
  part = runtime->get_logical_partition(ctx, region, ip);
}

void FFModel::create_aliased_partition(int num_dims,
                                       const ParallelDim dims[],
                                       int aliased_dim,
                                       IndexSpace const &part_is,
                                       LogicalRegion const &region,
                                       LogicalPartition &part) {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  Domain task_domain = runtime->get_index_space_domain(ctx, part_is);
  switch ((num_dims - 1) * MAX_TENSOR_DIM + task_domain.get_dim() - 1) {
#define DIMFUNC(NDIM, TDIM)                                                    \
  case (NDIM - 1) * MAX_TENSOR_DIM + (TDIM - 1): {                             \
    IndexSpaceT<TDIM> part_is_t(part_is);                                      \
    return create_aliased_partition_with_dim2<NDIM, TDIM>(                     \
        dims, aliased_dim, part_is_t, region, part);                           \
  }
    LEGION_FOREACH_NN(DIMFUNC)
#undef DIMFUNC
    default:
      assert(false && "Unsupported NDIM/TDIM");
  }
}

template <int NDIM, int TDIM>
void FFModel::create_aliased_partition_with_dim2(
    const ParallelDim dims[],
    int aliased_dim,
    IndexSpaceT<TDIM> const &part_is,
    LogicalRegion const &region,
    LogicalPartition &part) {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  // Rect<NDIM> part_rect = runtime->get_index_space_domain(ctx, part_is);
  Transform<NDIM, TDIM> transform;
  Point<NDIM> ext_hi;
  Rect<NDIM> rect =
      runtime->get_index_space_domain(ctx, region.get_index_space());
  for (int i = 0; i < NDIM; i++) {
    int nparts = dims[i].degree;
    if (aliased_dim == i) {
      nparts = 1;
    }
    ext_hi[i] = (rect.hi[i] - rect.lo[i] + nparts) / nparts - 1;
  }
  Rect<NDIM> extent(Point<NDIM>::ZEROES(), ext_hi);
  for (int i = 0; i < NDIM; i++) {
    for (int j = 0; j < TDIM; j++) {
      if (dims[i].parallel_idx == j && i != aliased_dim) {
        transform[i][j] = extent.hi[i] - extent.lo[i] + 1;
      } else {
        transform[i][j] = 0;
      }
    }
  }
  IndexPartition ip = runtime->create_partition_by_restriction(
      ctx, region.get_index_space(), part_is, transform, extent);
  // assert(runtime->is_index_partition_disjoint(ctx, ip));
  assert(runtime->is_index_partition_complete(ctx, ip));
  part = runtime->get_logical_partition(ctx, region, ip);
}

template <int NDIM>
void FFModel::create_disjoint_partition(const ParallelTensor tensor,
                                        IndexSpaceT<NDIM> const &part_is,
                                        LogicalPartition &part_fwd,
                                        LogicalPartition &part_bwd) {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  // Check that dimension sizes match
  {
    assert(tensor->num_dims == NDIM);
    Domain domain = runtime->get_index_space_domain(ctx, part_is);
    assert(domain.get_dim() == NDIM);
  }
  Rect<NDIM> rect =
      runtime->get_index_space_domain(ctx, tensor->region.get_index_space());
  Rect<NDIM> part_rect = runtime->get_index_space_domain(ctx, part_is);
  Transform<NDIM, NDIM> transform;
  Point<NDIM> ext_hi;
  for (int i = 0; i < NDIM; i++) {
    int nparts = part_rect.hi[i] - part_rect.lo[i] + 1;
    ext_hi[i] = (rect.hi[i] - rect.lo[i] + nparts) / nparts - 1;
  }
  Rect<NDIM> extent(Point<NDIM>::ZEROES(), ext_hi);
  for (int i = 0; i < NDIM; i++) {
    for (int j = 0; j < NDIM; j++) {
      if (i == j) {
        transform[i][j] = extent.hi[i] - extent.lo[i] + 1;
      } else {
        transform[i][j] = 0;
      }
    }
  }
  IndexPartition ip = runtime->create_partition_by_restriction(
      ctx, tensor->region.get_index_space(), part_is, transform, extent);
  assert(runtime->is_index_partition_disjoint(ctx, ip));
  assert(runtime->is_index_partition_complete(ctx, ip));
  part_fwd = runtime->get_logical_partition(ctx, tensor->region, ip);
  if (tensor->region_grad != LogicalRegion::NO_REGION) {
    // Current assume forward and grad share the same index space
    assert(tensor->region.get_index_space() ==
           tensor->region_grad.get_index_space());
    part_bwd = runtime->get_logical_partition(ctx, tensor->region_grad, ip);
  } else {
    part_bwd = LogicalPartition::NO_PART;
  }
}

template <int NDIM, int TDIM>
void FFModel::create_data_parallel_partition_with_diff_dims(
    const ParallelTensor tensor,
    IndexSpaceT<TDIM> const &part_is,
    LogicalPartition &part_fwd,
    LogicalPartition &part_bwd) {
  assert(tensor->num_dims == NDIM);
  if (config.computationMode == COMP_MODE_TRAINING) {
    // Current assume forward and grad share the same index space
    if (tensor->region_grad != LogicalRegion::NO_REGION) {
      assert(tensor->region.get_index_space() ==
             tensor->region_grad.get_index_space());
    }
  }
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  Rect<NDIM> rect =
      runtime->get_index_space_domain(ctx, tensor->region.get_index_space());
  Rect<TDIM> part_rect = runtime->get_index_space_domain(ctx, part_is);
  // Assume it is data parallel
  for (int i = 0; i < TDIM - 1; i++) {
    assert(part_rect.lo[i] == part_rect.hi[i]);
  }
  Transform<NDIM, TDIM> transform;
  Point<NDIM> ext_hi;
  for (int i = 0; i < NDIM; i++) {
    int nparts = 1;
    if (i == NDIM - 1) {
      nparts = part_rect.hi[TDIM - 1] - part_rect.lo[TDIM - 1] + 1;
    }
    ext_hi[i] = (rect.hi[i] - rect.lo[i] + nparts) / nparts - 1;
  }
  Rect<NDIM> extent(Point<NDIM>::ZEROES(), ext_hi);
  for (int i = 0; i < NDIM; i++) {
    for (int j = 0; j < TDIM; j++) {
      transform[i][j] = 0;
    }
  }
  transform[NDIM - 1][TDIM - 1] = extent.hi[NDIM - 1] - extent.lo[NDIM - 1] + 1;
  IndexPartition ip = runtime->create_partition_by_restriction(
      ctx, tensor->region.get_index_space(), part_is, transform, extent);
  assert(runtime->is_index_partition_disjoint(ctx, ip));
  assert(runtime->is_index_partition_complete(ctx, ip));
  part_fwd = runtime->get_logical_partition(ctx, tensor->region, ip);
  if (config.computationMode == COMP_MODE_TRAINING) {
    if (tensor->region_grad != LogicalRegion::NO_REGION) {
      part_bwd = runtime->get_logical_partition(ctx, tensor->region_grad, ip);
    }
  } else {
    part_bwd = LogicalPartition::NO_PART;
  }
}

// This function assumes:
// 1. the outer most dim of weight is channel out
// 2. partition is 2D (sample, channel_out)

template <int NDIM, int TDIM>
void FFModel::map_linear_weight(ParallelTensor weight, Op const *op) {
  assert(op->op_type == OP_LINEAR);
  std::string pcname = op->name;
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  Rect<TDIM> part_rect = runtime->get_index_space_domain(ctx, op->parallel_is);
  int num_parts[TDIM];
  for (int i = 0; i < TDIM; i++) {
    num_parts[i] = part_rect.hi[i] - part_rect.lo[i] + 1;
  }
  FieldSpace fs = runtime->create_field_space(ctx);
  FieldAllocator allocator = runtime->create_field_allocator(ctx, fs);
  switch (weight->data_type) {
    case DT_FLOAT:
      allocator.allocate_field(sizeof(float), FID_DATA);
      break;
    case DT_DOUBLE:
      allocator.allocate_field(sizeof(double), FID_DATA);
      break;
    case DT_INT32:
      allocator.allocate_field(sizeof(int), FID_DATA);
      break;
    default:
      assert(false);
  }
  int out_channels = weight->dims[weight->num_dims - 1].size;
  // Step 1: forward region and partition
  if (weight->sync_type == ParameterSyncType::PS) {
    Point<NDIM> hi;
    for (int i = 0; i < NDIM; i++) {
      hi[i] = weight->dims[i].size - 1;
    }
    Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
    IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
    weight->region = runtime->create_logical_region(ctx, is, fs);
    assert(out_channels % num_parts[0] == 0);
    hi[NDIM - 1] = out_channels / num_parts[0] - 1;
    Rect<NDIM> extent(Point<NDIM>::ZEROES(), hi);
    Transform<NDIM, TDIM> transform;
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < TDIM; j++) {
        transform[i][j] = 0;
      }
    }
    transform[NDIM - 1][0] = out_channels / num_parts[0];
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, op->parallel_is, transform, extent);
    assert(runtime->is_index_partition_complete(ctx, ip));
    weight->part = runtime->get_logical_partition(ctx, weight->region, ip);
  } else if (weight->sync_type == ParameterSyncType::NCCL) {
    // FIXME: Currently only support the sample dimension for operators with
    // NCCL
    // for (int i = 0; i < TDIM-1; i++)
    //  assert(num_parts[i] == 1);
    Point<NDIM> hi;
    for (int i = 0; i < NDIM; i++) {
      hi[i] = weight->dims[i].size - 1;
    }
    int num_batches = 1;
    for (int i = 1; i < TDIM; i++) {
      num_batches *= num_parts[i];
    }
    hi[NDIM - 1] = num_batches * out_channels - 1;
    Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
    IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
    weight->region = runtime->create_logical_region(ctx, is, fs);
    hi[NDIM - 1] = out_channels / num_parts[0] - 1;
    Rect<NDIM> extent(Point<NDIM>::ZEROES(), hi);
    Transform<NDIM, TDIM> transform;
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < TDIM; j++) {
        transform[i][j] = 0;
      }
    }
    transform[NDIM - 1][0] = out_channels / num_parts[0];
    for (int i = 1; i < TDIM; i++) {
      transform[NDIM - 1][i] = transform[NDIM - 1][i - 1] * num_parts[i - 1];
    }
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, op->parallel_is, transform, extent);
    assert(runtime->is_index_partition_complete(ctx, ip));
    assert(runtime->is_index_partition_disjoint(ctx, ip));
    weight->part = runtime->get_logical_partition(ctx, weight->region, ip);
  } else {
    assert(false);
  }
  // Step 2: initialize region
  if (weight->initializer == NULL) {
    assert(false); // add weight initializer should be set before
  } else {
    weight->initializer->init(this, weight);
  }
  // Step 3: backward region
  if (weight->create_gradients &&
      config.computationMode == COMP_MODE_TRAINING) {
    Point<NDIM> hi;
    for (int i = 0; i < NDIM; i++) {
      hi[i] = weight->dims[i].size - 1;
    }
    int num_batches = 1;
    for (int i = 1; i < TDIM; i++) {
      num_batches *= num_parts[i];
    }
    hi[NDIM - 1] = num_batches * out_channels - 1;
    Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
    IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
    weight->region_grad = runtime->create_logical_region(ctx, is, fs);
    hi[NDIM - 1] = out_channels / num_parts[0] - 1;
    Rect<NDIM> extent(Point<NDIM>::ZEROES(), hi);
    Transform<NDIM, TDIM> transform;
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < TDIM; j++) {
        transform[i][j] = 0;
      }
    }
    transform[NDIM - 1][0] = out_channels / num_parts[0];
    for (int i = 1; i < TDIM; i++) {
      transform[NDIM - 1][i] = transform[NDIM - 1][i - 1] * num_parts[i - 1];
    }
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, op->parallel_is, transform, extent);
    assert(runtime->is_index_partition_complete(ctx, ip));
    assert(runtime->is_index_partition_disjoint(ctx, ip));
    weight->part_grad =
        runtime->get_logical_partition(ctx, weight->region_grad, ip);
  }
}

template <int NDIM>
void FFModel::map_conv_weight(ParallelTensor weight, Op const *op) {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  Rect<4> part_rect = runtime->get_index_space_domain(ctx, op->parallel_is);
  int num_par_n = part_rect.hi[3] - part_rect.lo[3] + 1;
  int num_par_c = part_rect.hi[2] - part_rect.lo[2] + 1;
  int num_par_h = part_rect.hi[1] - part_rect.lo[1] + 1;
  int num_par_w = part_rect.hi[0] - part_rect.lo[0] + 1;
  // Currently assume we do not split over the channel dimension
  assert(num_par_c == 1);
  FieldSpace fs = runtime->create_field_space(ctx);
  FieldAllocator allocator = runtime->create_field_allocator(ctx, fs);
  switch (weight->data_type) {
    case DT_FLOAT:
      allocator.allocate_field(sizeof(float), FID_DATA);
      break;
    case DT_DOUBLE:
      allocator.allocate_field(sizeof(double), FID_DATA);
      break;
    case DT_INT32:
      allocator.allocate_field(sizeof(int), FID_DATA);
      break;
    default:
      assert(false);
  }
  // Step 1: forward region and partition
  int out_channels = weight->dims[weight->num_dims - 1].size;
  if (weight->sync_type == ParameterSyncType::PS) {
    Point<NDIM> hi;
    for (int i = 0; i < NDIM; i++) {
      hi[i] = weight->dims[i].size - 1;
    }
    Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
    IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
    weight->region = runtime->create_logical_region(ctx, is, fs);
    Transform<NDIM, 4> transform;
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < 4; j++) {
        transform[i][j] = 0;
      }
    }
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, op->parallel_is, transform, rect);
    assert(runtime->is_index_partition_complete(ctx, ip));
    weight->part = runtime->get_logical_partition(ctx, weight->region, ip);
  } else if (weight->sync_type == ParameterSyncType::NCCL) {
    // Currently only support sample and attribute parallelism for NCCL
    // communication
    assert(num_par_c == 1);
    Point<NDIM> hi;
    for (int i = 0; i < NDIM; i++) {
      hi[i] = weight->dims[i].size - 1;
    }
    hi[NDIM - 1] = num_par_n * num_par_h * num_par_w * out_channels - 1;
    Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
    IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
    weight->region = runtime->create_logical_region(ctx, is, fs);
    hi[NDIM - 1] = out_channels - 1;
    Rect<NDIM> extent(Point<NDIM>::ZEROES(), hi);
    Transform<NDIM, 4> transform;
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < 4; j++) {
        transform[i][j] = 0;
      }
    }
    transform[NDIM - 1][0] = out_channels;
    transform[NDIM - 1][1] = out_channels * num_par_w;
    transform[NDIM - 1][2] = out_channels * num_par_w * num_par_h;
    transform[NDIM - 1][3] = out_channels * num_par_w * num_par_h * num_par_c;
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, op->parallel_is, transform, extent);
    assert(runtime->is_index_partition_complete(ctx, ip));
    assert(runtime->is_index_partition_disjoint(ctx, ip));
    weight->part = runtime->get_logical_partition(ctx, weight->region, ip);
  } else {
    // Unsupported Parameter type
    assert(false);
  }
  // Step 2: initialize region
  if (weight->initializer == NULL) {
    assert(false); // add weight initializer should be set before
  } else {
    weight->initializer->init(this, weight);
  }
  // Step 3: backward regin and partition
  if (weight->create_gradients &&
      config.computationMode == COMP_MODE_TRAINING) {
    Point<NDIM> hi;
    for (int i = 0; i < NDIM; i++) {
      hi[i] = weight->dims[i].size - 1;
    }
    hi[NDIM - 1] = num_par_n * num_par_h * num_par_w * out_channels - 1;
    Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
    IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
    weight->region_grad = runtime->create_logical_region(ctx, is, fs);
    hi[NDIM - 1] = out_channels - 1;
    Rect<NDIM> extent(Point<NDIM>::ZEROES(), hi);
    Transform<NDIM, 4> transform;
    for (int i = 0; i < NDIM; i++) {
      for (int j = 0; j < 4; j++) {
        transform[i][j] = 0;
      }
    }
    transform[NDIM - 1][0] = out_channels;
    transform[NDIM - 1][1] = out_channels * num_par_w;
    transform[NDIM - 1][2] = out_channels * num_par_w * num_par_h;
    transform[NDIM - 1][3] = out_channels * num_par_w * num_par_h * num_par_c;
    IndexPartition ip = runtime->create_partition_by_restriction(
        ctx, is, op->parallel_is, transform, extent);
    assert(runtime->is_index_partition_complete(ctx, ip));
    assert(runtime->is_index_partition_disjoint(ctx, ip));
    weight->part_grad =
        runtime->get_logical_partition(ctx, weight->region_grad, ip);
  }
}

template <int NDIM, int TDIM>
ParallelTensor FFModel::create_linear_replica(int const dims[],
                                              IndexSpaceT<TDIM> const &task_is,
                                              DataType data_type) {
  // No need to create replica for INFERENCE
  assert(config.computationMode == COMP_MODE_TRAINING);
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  assert(NDIM >= 2);
  Rect<TDIM> part_rect = runtime->get_index_space_domain(ctx, task_is);
  int num_parts[TDIM];
  for (int i = 0; i < TDIM; i++) {
    num_parts[i] = part_rect.hi[i] - part_rect.lo[i] + 1;
  }
  ParallelTensor replica = new ParallelTensorBase();
  replica->parallel_tensor_guid = parallel_tensor_global_guid++;
  replica->num_dims = NDIM;
  replica->data_type = data_type;
  for (int i = 0; i < NDIM; i++) {
    replica->dims[i].size = dims[NDIM - 1 - i];
  }
  FieldSpace fs = runtime->create_field_space(ctx);
  FieldAllocator allocator = runtime->create_field_allocator(ctx, fs);
  switch (data_type) {
    case DT_FLOAT:
      allocator.allocate_field(sizeof(float), FID_DATA);
      break;
    case DT_DOUBLE:
      allocator.allocate_field(sizeof(double), FID_DATA);
      break;
    case DT_INT32:
      allocator.allocate_field(sizeof(int), FID_DATA);
      break;
    default:
      assert(false);
  }
  Point<NDIM> hi;
  for (int i = 0; i < NDIM; i++) {
    hi[i] = dims[NDIM - 1 - i] - 1;
  }
  Rect<NDIM> rect(Point<NDIM>::ZEROES(), hi);
  IndexSpaceT<NDIM> is = runtime->create_index_space(ctx, rect);
  replica->region_grad = runtime->create_logical_region(ctx, is, fs);
  assert(dims[0] == num_parts[0]);
  // assert(dims[1] % num_parts[1] == 0);
  hi[NDIM - 1] = dims[0] / num_parts[0] - 1;        // replication dim
  hi[NDIM - 2] = dims[1] / num_parts[TDIM - 1] - 1; // sample dim
  Rect<NDIM> extent(Point<NDIM>::ZEROES(), hi);
  Transform<NDIM, TDIM> transform;
  for (int i = 0; i < NDIM; i++) {
    for (int j = 0; j < TDIM; j++) {
      transform[i][j] = 0;
    }
  }
  transform[NDIM - 1][0] = hi[NDIM - 1] + 1;
  transform[NDIM - 2][TDIM - 1] = hi[NDIM - 2] + 1;
  // transform[NDIM-2][1] = dims[1] / num_parts[1];
  IndexPartition ip = runtime->create_partition_by_restriction(
      ctx, is, task_is, transform, extent);
  assert(runtime->is_index_partition_disjoint(ctx, ip));
  assert(runtime->is_index_partition_complete(ctx, ip));
  replica->part_grad =
      runtime->get_logical_partition(ctx, replica->region_grad, ip);
  return replica;
}

IndexSpace FFModel::get_task_is(MachineView const &view) const {
  auto const &iter = all_task_is.find(view);
  assert(iter != all_task_is.end());
  return iter->second;
}

IndexSpace FFModel::get_task_is(ParallelConfig const &pc) const {
  MachineView view;
  view.ndims = pc.nDims;
  for (int i = 0; i < view.ndims; i++) {
    view.dim[i] = pc.dim[i];
  }
  return get_task_is(view);
}

IndexSpace FFModel::get_or_create_task_is(const ParallelTensor tensor) {
  MachineView view;
  view.ndims = 0;
  for (int i = 0; i < tensor->num_dims; i++) {
    if (tensor->dims[i].parallel_idx >= 0) {
      view.dim[tensor->dims[i].parallel_idx] = tensor->dims[i].degree;
      view.ndims++;
    }
  }
  if (view.ndims == 0) {
    view.ndims = 1;
    view.dim[0] = 1;
  }
  return get_or_create_task_is(view);
}

IndexSpace FFModel::get_or_create_task_is(ParallelConfig const &pc) {
  MachineView view;
  view.ndims = pc.nDims;
  for (int i = 0; i < view.ndims; i++) {
    view.dim[i] = pc.dim[i];
  }
  return get_or_create_task_is(view);
}

IndexSpace FFModel::get_or_create_task_is(MachineView const &view) {
  if (all_task_is.find(view) != all_task_is.end()) {
    return all_task_is[view];
  }
  IndexSpace task_is;
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  switch (view.ndims) {
#define DIMFUNC(DIM)                                                           \
  case DIM: {                                                                  \
    Rect<DIM> task_rect;                                                       \
    for (int i = 0; i < DIM; i++) {                                            \
      task_rect.lo[i] = 0;                                                     \
      task_rect.hi[i] = view.dim[i] - 1;                                       \
    }                                                                          \
    task_is = runtime->create_index_space(ctx, task_rect);                     \
    break;                                                                     \
  }
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default:
      assert(false);
  }
  printf("ndim(%d) dims[%d %d %d %d]\n",
         view.ndims,
         view.dim[0],
         view.dim[1],
         view.dim[2],
         view.dim[3]);
  all_task_is[view] = task_is;
  return task_is;
}

IndexSpace FFModel::get_or_create_task_is(Domain const &domain) {
  MachineView view;
  view.ndims = domain.get_dim();
  for (int i = 0; i < view.ndims; i++) {
    view.dim[i] = domain.hi()[i] - domain.lo()[i] + 1;
  }
  return get_or_create_task_is(view);
}

/*
IndexSpace FFModel::get_or_create_task_is(int ndims, const std::string& pcname)
{
  ParallelConfig pc;
  bool result = config.find_parallel_config(ndims, pcname, pc);
  assert(result);
  return get_or_create_task_is(pc);
}

IndexSpace FFModel::get_task_is(int ndims, const std::string& pcname) const
{
  ParallelConfig pc;
  bool result = config.find_parallel_config(ndims, pcname, pc);
  assert(result);
  return get_task_is(pc);
}
*/

IndexSpace FFModel::get_task_is(Domain const &domain) const {
  MachineView view;
  view.ndims = domain.get_dim();
  for (int i = 0; i < view.ndims; i++) {
    view.dim[i] = domain.hi()[i] - domain.lo()[i] + 1;
  }
  auto const &iter = all_task_is.find(view);
  assert(iter != all_task_is.end());
  return iter->second;
}

void FFModel::reset_metrics() {
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  TaskLauncher launcher(UPDATE_METRICS_TASK_ID,
                        TaskArgument(metrics_op, sizeof(Metrics)));
  current_metrics = runtime->execute_task(ctx, launcher);
}

void FFModel::init_operators() {
  for (size_t i = 0; i < operators.size(); i++) {
    operators[i]->init(*this);
  }
}

void FFModel::forward(int seq_length) {
  iter_config.seq_length = seq_length;
  for (size_t i = 0; i < operators.size(); i++) {
    operators[i]->forward(*this);
  }
}

void FFModel::recompile_on_condition(RecompileState &r) {
  if (r.trigger()) {
    r.alter();
  }
}

void FFModel::compute_metrics() {
  Op *final_operator = get_final_operator();
  assert(final_operator->numOutputs == 1);
  metrics_op->compute(this, final_operator->outputs[0], parallel_label_tensor);
}

void FFModel::get_metrics() {
  metrics_input = operators.size() - 1;
}

void FFModel::backward(int seq_length) {
  iter_config.seq_length = seq_length;
  assert(config.computationMode == COMP_MODE_TRAINING);
  // Compute metrics
  compute_metrics();
  // Compute the gradients of the final operator wrt loss
  Op *final_operator = get_final_operator();
  assert(final_operator->numOutputs == 1);
  loss_op->backward(this, final_operator->outputs[0], parallel_label_tensor);
  // Perform backpropagation
  // std::set<LogicalRegion> resetedInputGrads;
  for (int l = operators.size() - 1; l >= 0; l--) {
#ifdef ENABLE_RESNET_INPUT_GRADIENT_OPTIMIZATION
    for (int i = 0; i < operators[l]->numInputs; i++) {
      if (resetedInputGrads.find(operators[l]->inputs[i]->region) ==
          resetedInputGrads.end()) {
        resetedInputGrads.insert(operators[l]->inputs[i]->region);
      } else {
        // This input's gradients has been reseted by other operators
        // So we should not do it again
        operators[l]->resetInputGrads[i] = false;
      }
    }
#endif
    // TODO: If operator serves for metrics and for further prop
    // if(l == metrics_input && metrics_input < (int)operators.size()-1)
    //  continue;
    operators[l]->backward(*this);
  }
}

void FFModel::update() {
  optimizer->next();
  for (size_t i = 0; i < parameters.size(); i++) {
    optimizer->update(parameters[i]);
  }
}

Op *FFModel::get_final_operator() const {
  int idx = operators.size() - 1;
  while (operators[idx]->op_type == OP_INPUT ||
         operators[idx]->op_type == OP_WEIGHT) {
    idx--;
  }
  // assert that the final operator has exactly one output
  assert(operators[idx]->numOutputs == 1);
  return operators[idx];
}

void FFModel::compile(Optimizer *_optimizer,
                      LossType loss_type,
                      std::vector<MetricsType> const &metrics,
                      CompMode comp_mode) {
  optimizer = _optimizer;
  compile(loss_type, metrics, comp_mode);
}

bool FFModel::apply_fusion(std::vector<Op *> const &operators,
                           std::vector<Op *> &new_operators) {
  // Context ctx = config.lg_ctx;
  // Runtime* runtime = config.lg_hlr;
  for (size_t l = 1; l < operators.size() - 1; l++) {
    // don't fuse input and weight operator since they don't involve any
    // forward/backward task launches
    if (operators[l]->op_type == OP_INPUT ||
        operators[l]->op_type == OP_WEIGHT) {
      continue;
    }
    // don't fuse parallel op since they have different parallel_is in
    // forward/backward
    if (operators[l]->is_parallel_op()) {
      continue;
    }
    size_t start = 0;
    {
      Op *opl = operators[l];
      for (int idx = 0; idx < opl->numInputs; idx++) {
        bool found = false;
        for (size_t i = 0; i < l; i++) {
          if (opl->inputs[idx]->owner_op == operators[i]) {
            assert(!found);
            found = true;
            if (i > start) {
              start = i;
            }
          }
        }
        assert(found || (opl->inputs[idx]->owner_op == NULL));
      }
    }
    for (size_t i = start; i < l; i++) {
      // Domain d1 =
      // runtime->get_index_space_domain(operators[l]->outputs[0]->parallel_is);
      // Domain d2 =
      // runtime->get_index_space_domain(operators[i]->outputs[0]->parallel_is);
      MachineView view1 = operators[l]->outputs[0]->machine_view;
      MachineView view2 = operators[i]->outputs[0]->machine_view;
      if (view1 == view2) {
        FusedOp *fused_op = nullptr;
        bool allocate_new_fused_op = false;
        if (operators[i]->op_type == OP_FUSED) {
          fused_op = (FusedOp *)operators[i];
        } else {
          //  cannot be an in-place operator
          if (operators[i]->has_inplace_output()) {
            continue;
          }
          // don't fuse input and weight operator since they don't involve any
          // forward/backward kernels
          if (operators[i]->op_type == OP_INPUT ||
              operators[i]->op_type == OP_WEIGHT) {
            continue;
          }
          // don't fuse parallel op since they have different parallel_is in
          // forward/backward
          if (operators[i]->is_parallel_op()) {
            continue;
          }
          fused_op = new FusedOp(*this, operators[i]);
          allocate_new_fused_op = true;
        }
        if (fused_op->add_operator(*this, operators[l])) {
          // Construct new operators
          new_operators.clear();
          for (size_t j = 0; j < i; j++) {
            new_operators.push_back(operators[j]);
          }
          new_operators.push_back(fused_op);
          for (size_t j = i + 1; j < operators.size(); j++) {
            if (j == l) {
              continue; // l and i are fused
            }
            Op *op = operators[j];
            // Update input tensors that belong to operator[l] or operator[i]
            for (int idx = 0; idx < op->numInputs; idx++) {
              if ((op->inputs[idx]->owner_op == operators[l]) ||
                  (op->inputs[idx]->owner_op == operators[i])) {
                int found = -1;
                for (int k = 0; k < fused_op->numOutputs; k++) {
                  if (fused_op->outputs[k]->region == op->inputs[idx]->region) {
                    assert(found == -1);
                    found = k;
                  }
                }
                assert(found >= 0);
                op->inputs[idx] = fused_op->outputs[found];
              }
            }
            // Insert op
            new_operators.push_back(op);
          }
          // We are exact one operator fewer than the original
          assert(new_operators.size() + 1 == operators.size());
          return true;
        } else {
          // TODO: delete fused_op to avoid memory leakage
          if (allocate_new_fused_op) {
            delete fused_op;
          }
          continue;
        }
      }
    }
  }
  return false;
}

Op *FFModel::create_operator_from_layer(
    Layer *layer, std::vector<ParallelTensor> const &inputs) {
  switch (layer->op_type) {
    case OP_INPUT: {
      // Input op cannot have an input
      assert(inputs.size() == 0);
      // Current assume we add one dimension before each tensor
      Tensor tensor = layer->outputs[0];
      int num_dims = tensor->num_dims;
      ParallelDim dims[MAX_TENSOR_DIM];
      for (int j = 0; j < num_dims; j++) {
        dims[j].size = tensor->dims[j];
        dims[j].degree = 1;
        dims[j].parallel_idx = -1;
        dims[j].is_replica_dim = false;
      }
      dims[num_dims].size = 1;
      dims[num_dims].degree = 1;
      dims[num_dims].parallel_idx = -1;
      dims[num_dims].is_replica_dim = true;
      // create_parallel_tensor adds an NoOp into operators
      ParallelTensor pt =
          create_parallel_tensor_legion_ordering(num_dims + 1,
                                                 dims,
                                                 tensor->data_type,
                                                 nullptr,
                                                 0,
                                                 true /*gradients*/,
                                                 tensor->tensor_guid);
      // assert that this tensor hasn't been mapped before
      assert(tensor->parallel_tensor == nullptr);
      tensor->parallel_tensor = pt;
      // start from data parllel tensor
      if (config.only_data_parallel) {
        Repartition *part = new Repartition(
            *this, pt, num_dims - 1, config.numNodes * config.workersPerNode);
        operators.push_back(part);
      }
      return operators[operators.size() - 1];
    }
    case OP_MULTIHEAD_ATTENTION: {
      Op *op =
          MultiHeadAttention::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_BATCHMATMUL: {
      Op *op = BatchMatmul::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_CAST: {
      Op *op = Cast::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_CONCAT: {
      Op *op = Concat::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_CONV2D: {
      Op *op = Conv2D::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_DROPOUT: {
      Op *op = Dropout::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_EMBEDDING: {
      Op *op = Embedding::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_EW_ADD:
    case OP_EW_SUB:
    case OP_EW_MUL:
    case OP_EW_DIV:
    case OP_EW_MAX:
    case OP_EW_MIN: {
      Op *op = ElementBinary::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_EXP:
    case OP_SIN:
    case OP_COS:
    case OP_SCALAR_MULTIPLY:
    case OP_SCALAR_ADD:
    case OP_SCALAR_SUB:
    case OP_SCALAR_TRUE_DIV:
    case OP_POW:
    case OP_RELU:
    case OP_SIGMOID:
    case OP_TANH:
    case OP_IDENTITY:
    case OP_GELU:
    case OP_ELU: {
      Op *op = ElementUnary::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_FLAT: {
      Op *op = Flat::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_GATHER: {
      Op *op = Gather::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_LAYERNORM: {
      Op *op = LayerNorm::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_LINEAR: {
      Op *op = Linear::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_POOL2D: {
      Op *op = Pool2D::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_REDUCE_SUM: {
      Op *op = Reduce::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_RESHAPE: {
      Op *op = Reshape::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_SOFTMAX: {
      Op *op = Softmax::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_SPLIT: {
      Op *op = Split::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_TRANSPOSE: {
      Op *op = Transpose::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_TOPK: {
      Op *op = TopK::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_GROUP_BY: {
      Op *op = Group_by::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_AGGREGATE: {
      Op *op = Aggregate::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    case OP_AGG_SPEC: {
      Op *op = Aggregate::create_operator_from_layer(*this, layer, inputs);
      operators.push_back(op);
      return op;
    }
    default:
      assert(false);
  }
}

void FFModel::create_operators_from_layers() {
  std::map<const Tensor, ParallelTensor> tensors_to_parallel_tensors;
  for (auto const &l : layers) {
    std::vector<ParallelTensor> inputs;
    for (int i = 0; i < l->numInputs; i++) {
      // create new input tensors
      assert(tensors_to_parallel_tensors.find(l->inputs[i]) !=
             tensors_to_parallel_tensors.end());
      inputs.push_back(tensors_to_parallel_tensors[l->inputs[i]]);
    }
    Op *op = create_operator_from_layer(l, inputs);
    assert(op->numOutputs == l->numOutputs);
    for (int i = 0; i < op->numOutputs; i++) {
      tensors_to_parallel_tensors[l->outputs[i]] = op->outputs[i];
    }
  }
}

void FFModel::compile(LossType loss_type,
                      std::vector<MetricsType> const &metrics,
                      CompMode comp_mode) {
  if (metrics_input == -1) {
    metrics_input = operators.size() - 1;
  }
  Context ctx = config.lg_ctx;
  Runtime *runtime = config.lg_hlr;
  config.computationMode = comp_mode;
  // if (config.import_strategy_file.length() > 0) {
  //   load_strategies_from_file(config.import_strategy_file,
  //   config.strategies);
  // }
  //  Construct operators from layers
  if (config.only_data_parallel) {
    fprintf(stderr,
            "Note: only_data_parallel is specified, FlexFlow compiles a "
            "data-parallel PCG.\n");
  }
  create_operators_from_layers();
  // Launch the graph optimize task
  {
    FFModel *model = this;
    TaskLauncher launcher(GRAPH_OPTIMIZE_TASK_ID,
                          TaskArgument(&model, sizeof(FFModel *)));
    Future future = runtime->execute_task(ctx, launcher);

    PCG::GraphOptimalViewSerialized ret =
        future.get_result<PCG::GraphOptimalViewSerialized>();
    Deserializer dez(ret.data, ret.total_bytes);
    // Reconstruct operators
    PCG::Graph *best_graph = new PCG::Graph(this);
    std::unordered_map<PCG::Node, MachineView> optimal_views;
    deserialize_graph_optimal_view(dez, best_graph, optimal_views);
    operators.clear();
    convert_graph_to_operators(best_graph, optimal_views);
    best_graph->print_dot();
    delete best_graph;
    for (auto const &layer : layers) {
      // map inputs to parallel tensor
      if (layer->op_type == OP_INPUT) {
        Tensor tensor = layer->outputs[0];
        ParallelTensor parallel_tensor = nullptr;
        for (auto const &op : operators) {
          if (op->op_type == OP_INPUT) {
            NoOp *noop = (NoOp *)op;
            if (noop->input_tensor_guid == tensor->tensor_guid) {
              parallel_tensor = op->outputs[0];
            }
          }
        }
        assert(parallel_tensor != nullptr);
        tensor->parallel_tensor = parallel_tensor;
      }
      // map weights to parallel_tensor
      for (int i = 0; i < layer->numWeights; i++) {
        assert(layer->weights[i] != nullptr);
        Tensor weight = layer->weights[i];
        ParallelTensor parallel_weight = nullptr;
        for (auto const &op : operators) {
          if (op->layer_guid == layer->layer_guid) {
            assert(op->op_type == layer->op_type);
            assert(op->numWeights == layer->numWeights);
            parallel_weight = op->weights[i];
          }
        }
        assert(parallel_weight != nullptr);
        weight->parallel_tensor = parallel_weight;
      }
    }
  }

  bool repl_labels = (operators[operators.size() - 1]->op_type == OP_AGG_SPEC);
  loss_op = new Loss(loss_type, repl_labels);
  metrics_op = new Metrics(loss_type, metrics);

  // Init performance metrics
  TaskLauncher launcher(UPDATE_METRICS_TASK_ID,
                        TaskArgument(metrics_op, sizeof(Metrics)));
  current_metrics = runtime->execute_task(ctx, launcher);

  // Perform inplace optimizations
  if (config.enable_inplace_optimizations) {
    for (size_t l = 1; l < operators.size(); l++) {
      if (operators[l]->can_inplace_output()) {
        // Assume outputs[0] is inplace with inputs[0]
        assert(operators[l]->numOutputs == 1);
        if (operators[l]->inputs[0]->owner_op != NULL) {
          // int dim1 = operators[l]->outputs[0]->num_dims;
          // int dim2 = operators[l]->inputs[0]->num_dims;
          MachineView view1 = operators[l]->outputs[0]->machine_view;
          MachineView view2 = operators[l]->inputs[0]->machine_view;
          if (view1 == view2) {
            // Check no others also need operators[l]->inputs[0]
            bool found = false;
            for (size_t i = 0; i < operators.size(); i++) {
              if (i == l) {
                continue;
              }
              for (int j = 0; j < operators[i]->numInputs; j++) {
                if ((operators[i]->inputs[j]->owner_op ==
                     operators[l]->inputs[0]->owner_op) &&
                    (operators[i]->inputs[j]->owner_idx ==
                     operators[l]->inputs[0]->owner_idx)) {
                  found = true;
                }
              }
            }
            if (!found) {
              // Perform inplace
              operators[l]->do_inplace_output();
            }
          }
        }
      }
    }
  }

  for (size_t l = 0; l < operators.size(); l++) {
    Op *op = operators[l];
    for (int i = 0; i < op->numInputs; i++) {
      assert(op->inputs[i]->owner_op != NULL);
    }
    for (int i = 0; i < op->numWeights; i++) {
      assert(op->weights[i]->owner_op != NULL);
      assert(op->weights[i]->region != LogicalRegion::NO_REGION);
      parameters.push_back(op->weights[i]);
    }
    op->map_output_tensors(*this);
    // for (int i = 0; i < op->numOutputs; i++) {
    //   // Output tensor
    //   map_tensor(op->outputs[i], op);
    // }
    if (op->is_parallel_op()) {
      ((ParallelOp *)op)->create_input_partition(*this);
    }
    // op->map_output_tensors(*this);
  }

  // Check correctness
  for (size_t l = 0; l < operators.size(); l++) {
    Op *op = operators[l];
    for (int i = 0; i < op->numOutputs; i++) {
      assert(op->outputs[i]->owner_op == op);
      assert(op->outputs[i]->owner_idx == i);
      assert(op->outputs[i]->parallel_tensor_guid != 0);
    }
  }

  // If an operator's input is training data
  // No need to compute its gradients
  for (size_t l = 0; l < operators.size(); l++) {
    Op *op = operators[l];
    for (int i = 0; i < op->numInputs; i++) {
      assert(op->inputs[i]->owner_op != nullptr);
      if (op->inputs[i]->owner_op->op_type == OP_INPUT) {
        op->trainableInputs[i] = false;
      }
    }
  }

  // Perform fusion optimizations
  if (config.perform_fusion) {
    fprintf(stderr, "Applying fusion optimizations during compilation...\n");
    fprintf(stderr, "%zu operators before fusion...\n", operators.size());
    std::vector<Op *> new_operators;
    std::vector<Op *> old_operators = operators;
    while (apply_fusion(operators, new_operators)) {
      for (size_t i = 0; i < new_operators.size(); i++) {
        for (int idx = 0; idx < new_operators[i]->numInputs; idx++) {
          for (size_t j = i + 1; j < new_operators.size(); j++) {
            if (new_operators[i]->inputs[idx]->owner_op == new_operators[j]) {
              assert(false);
            }
          }
        }
      }
      operators = new_operators;
    }
    // Check integrity
    for (size_t l = 0; l < operators.size(); l++) {
      if (operators[l]->op_type == OP_FUSED) {
        FusedOp *fused = (FusedOp *)operators[l];
        int ioff = 0, woff = 0, ooff = 0;
        for (int op = 0; op < fused->numOperators; op++) {
          Op *old_op = fused->operators[op];
          for (int i = 0; i < fused->op_num_inputs[op]; i++) {
            int my_off = fused->op_input_idx[i + ioff];
            if (fused->op_input_source[i + ioff] == FusedOp::SOURCE_INPUT) {
              assert(fused->inputs[my_off]->region ==
                     old_op->inputs[i]->region);
            } else if (fused->op_input_source[i + ioff] ==
                       FusedOp::SOURCE_OUTPUT) {
              assert(fused->outputs[my_off]->region ==
                     old_op->inputs[i]->region);
            } else {
              assert(false);
            }
          }
          for (int i = 0; i < fused->op_num_weights[op]; i++) {
            int my_off = fused->op_weight_idx[i + woff];
            assert(fused->op_weight_source[i + woff] == FusedOp::SOURCE_WEIGHT);
            assert(fused->weights[my_off]->region ==
                   old_op->weights[i]->region);
          }
          for (int i = 0; i < fused->op_num_outputs[op]; i++) {
            int my_off = fused->op_output_idx[i + ooff];
            assert(fused->op_output_source[i + ooff] == FusedOp::SOURCE_OUTPUT);
            assert(fused->outputs[my_off]->region ==
                   old_op->outputs[i]->region);
          }
          ioff += fused->op_num_inputs[op];
          woff += fused->op_num_weights[op];
          ooff += fused->op_num_outputs[op];
        }
      } else {
        bool found = false;
        for (size_t i = 0; i < old_operators.size(); i++) {
          if (old_operators[i] == operators[l]) {
            assert(!found);
            found = true;
          }
        }
        assert(found);
      }
    }
    fprintf(stderr, "%zu operators after fusion...\n", operators.size());
    for (size_t i = 0; i < operators.size(); i++) {
      Op *op = operators[i];
      printf("operator[%zu]: type(%s) guid(%lu)\n",
             i,
             get_operator_type_name(operators[i]->op_type).c_str(),
             operators[i]->op_guid);
      for (int j = 0; j < op->numInputs; j++) {
        LogicalRegion handle = op->inputs[j]->region;
        printf("inputs[%d] region(%d,%d,%d)\n",
               j,
               handle.get_index_space().get_id(),
               handle.get_field_space().get_id(),
               handle.get_tree_id());
      }
      for (int j = 0; j < op->numOutputs; j++) {
        LogicalRegion handle = op->outputs[j]->region;
        printf("outputs[%d] region(%d,%d,%d)\n",
               j,
               handle.get_index_space().get_id(),
               handle.get_field_space().get_id(),
               handle.get_tree_id());
      }
      for (int j = 0; j < op->numWeights; j++) {
        LogicalRegion handle = op->weights[j]->region;
        printf("weights[%d] region(%d,%d,%d)\n",
               j,
               handle.get_index_space().get_id(),
               handle.get_field_space().get_id(),
               handle.get_tree_id());
      }
    }
  }
  Op *final_operator = get_final_operator();
  // FIXME: currently assume the final operator has exactly one output
  assert(final_operator->numOutputs == 1);
  for (size_t i = 0; i < operators.size(); i++) {
    Op *op = operators[i];
    printf("operator[%zu]: type(%d)\n", i, operators[i]->op_type);
    for (int j = 0; j < op->numInputs; j++) {
      LogicalRegion handle = op->inputs[j]->region;
      printf("inputs[%d] region(%d,%d,%d)\n",
             j,
             handle.get_index_space().get_id(),
             handle.get_field_space().get_id(),
             handle.get_tree_id());
    }
    for (int j = 0; j < op->numOutputs; j++) {
      LogicalRegion handle = op->outputs[j]->region;
      printf("outputs[%d] region(%d,%d,%d)\n",
             j,
             handle.get_index_space().get_id(),
             handle.get_field_space().get_id(),
             handle.get_tree_id());
    }
  }
  // assert(final_operator->outputs[0].num_dims == 2);
  ParallelDim p_dims[MAX_TENSOR_DIM];
  int dims[MAX_TENSOR_DIM];
  int num_p_dims = final_operator->outputs[0]->num_dims;
  int num_dims = 0;
  // FIXME: Currently assume 1st input for 1st operator = batch_size
  for (int i = 0; i < num_p_dims; i++) {
    p_dims[i] = final_operator->outputs[0]->dims[i];
    if (!p_dims[i].is_replica_dim) {
      dims[num_dims++] = p_dims[i].size;
    }
  }
  DataType label_type = DT_FLOAT;
  if (loss_type == LOSS_SPARSE_CATEGORICAL_CROSSENTROPY) {
    // assign dims[num_dims-1] = 1 for sparse categorical labels
    assert(p_dims[0].degree == 1);
    p_dims[0].size = 1;
    dims[0] = 1;
    label_type = DT_INT32;
  }
  // create label tensor
  switch (num_dims) {
#define DIMFUNC(DIM)                                                           \
  case DIM: {                                                                  \
    label_tensor = create_tensor_legion_ordering(                              \
        num_dims, dims, label_type, NULL, 0 /*idx*/, false /*create_grad*/);   \
    parallel_label_tensor = create_parallel_tensor_legion_ordering(            \
        num_p_dims, p_dims, label_type);                                       \
    label_tensor->parallel_tensor = parallel_label_tensor;                     \
    parallel_label_tensor->machine_view =                                      \
        final_operator->outputs[0]->machine_view;                              \
    map_tensor(parallel_label_tensor, parallel_label_tensor->owner_op);        \
    break;                                                                     \
  }
    LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC
    default: {
      assert(false && "Unsupported dim");
    }
  }
  // init optimizer
  assert(optimizer != NULL);
  optimizer->init();

#ifdef FF_USE_NCCL
  if (config.computationMode == COMP_MODE_TRAINING) {
    // init all nccl communicators
    for (size_t l = 0; l < operators.size(); l++) {
      // Only create nccl for weights
      if (operators[l]->op_type != OP_WEIGHT) {
        continue;
      }
      MachineView view = operators[l]->outputs[0]->machine_view;
      if (view_hash_to_nccl_comms.find(view.hash()) ==
          view_hash_to_nccl_comms.end()) {
        TaskLauncher launcher(NCCL_GETUNIQUEID_TASK_ID, TaskArgument(NULL, 0));
        Future future = runtime->execute_task(ctx, launcher);
        ncclUniqueId ncclId = future.get_result<ncclUniqueId>();
        IndexSpace task_is = get_or_create_task_is(view);
        ArgumentMap argmap;
        IndexLauncher index_launcher(
            NCCL_INIT_COMMS_TASK_ID,
            task_is,
            TaskArgument(&ncclId, sizeof(ncclUniqueId)),
            argmap,
            Predicate::TRUE_PRED,
            false /*must*/,
            0 /*mapper_id*/,
            view.hash() /*MappingTagID*/);
        FutureMap fm = runtime->execute_index_space(ctx, index_launcher);
        fm.wait_all_results();
        int idx = 0;
        Domain task_domain = runtime->get_index_space_domain(ctx, task_is);
        ncclComm_t *nccl_comms =
            (ncclComm_t *)malloc(sizeof(ncclComm_t) * task_domain.get_volume());
        for (Domain::DomainPointIterator it(task_domain); it; it++, idx++) {
          nccl_comms[idx] = fm.get_result<ncclComm_t>(*it);
        }
        view_hash_to_nccl_comms[view.hash()] = nccl_comms;
      }
    }
  }
#endif
}

struct PropagationEdgeInfo {
  Op *dstOp;
  size_t size;
};

float randf() {
  return static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

#ifdef FF_USE_PROPAGATE
void FFModel::propagate(std::map<Op *, ParallelConfig> const &current,
                        std::map<Op *, ParallelConfig> &next) const {
  next = current;
  size_t opId = std::rand() % (operators.size() - 1);
  // TODO: need to make sure opId is not an output operator of the model
  assert(opId != operators.size() - 1);

  std::vector<PropagationEdgeInfo> choosable_edges;
  std::unordered_set<Op *> opsSeen;

  auto bwd_edge_map = this->get_bwd_edge_map();

  Op *selected_op = this->operators[opId];
  do {
    opsSeen.insert(selected_op);
    choosable_edges.clear();
    for (int i = 0; i < selected_op->numInputs; i++) {
      auto const &input = selected_op->inputs[i];
      if (opsSeen.find(input.owner_op) == opsSeen.end()) {
        PropagationEdgeInfo edgeInfo;
        edgeInfo.dstOp = selected_op->inputs[i].owner_op;
        if (edgeInfo.dstOp == NULL) {
          continue;
        }
        if (!edgeInfo.dstOp->is_adoptable_parallel_config(
                *this, next.at(selected_op))) {
          continue;
        }
        assert(edgeInfo.dstOp != NULL);
        edgeInfo.size = selected_op->inputs[i].get_volume();
        choosable_edges.push_back(edgeInfo);
      }
    }
    if (bwd_edge_map.find(selected_op) != bwd_edge_map.end()) {
      for (auto const &kv : bwd_edge_map.at(selected_op)) {
        if (opsSeen.find(kv.first) == opsSeen.end()) {
          PropagationEdgeInfo edgeInfo;
          edgeInfo.dstOp = kv.first;
          assert(edgeInfo.dstOp != NULL);
          if (!edgeInfo.dstOp->is_adoptable_parallel_config(
                  *this, next.at(selected_op))) {
            continue;
          }
          edgeInfo.size = kv.second;
          choosable_edges.push_back(edgeInfo);
        }
      }
    }

    if (choosable_edges.size() == 0) {
      break;
    }

    float avg_edge_size = 0.0f;
    for (auto const &edge : choosable_edges) {
      avg_edge_size += edge.size;
    }
    avg_edge_size /= choosable_edges.size();
    std::vector<float> edge_weights;
    for (auto const &edge : choosable_edges) {
      edge_weights.push_back(FFModel::PROPAGATION_SIZE_WEIGHT * edge.size +
                             avg_edge_size *
                                 (1 - FFModel::PROPAGATION_SIZE_WEIGHT));
    }
    assert(edge_weights.size() == choosable_edges.size());
    PropagationEdgeInfo chosenEdgeInfo =
        select_random(choosable_edges, edge_weights);

    auto const &dstOp = chosenEdgeInfo.dstOp;
    if (next.at(selected_op).is_data_parallel()) {
      next[dstOp] =
          next.at(selected_op)
              .change_data_parallel_dimensionality(dstOp->get_dimension());
      assert(dstOp->is_valid_parallel_config(*this, next.at(dstOp)));
    }
    selected_op = chosenEdgeInfo.dstOp;
  } while (randf() < FFModel::CONTINUE_PROPAGATION_CHANCE);
}
#endif

void FFModel::rewrite(std::map<Op const *, ParallelConfig> const &current,
                      std::map<Op const *, ParallelConfig> &next,
                      bool use_propagation) const {
  next = current;
  float propagate_chance;
  if (use_propagation) {
    propagate_chance = FFModel::PROPAGATION_CHANCE;
  } else {
    propagate_chance = 0.0f;
  }

  if (randf() < propagate_chance) {
#ifdef FF_USE_PROPAGATE
    this->propagate(current, next);
#endif
  } else {
    size_t opId = std::rand() % operators.size();
    // TODO: need to make sure opId is not an output operator of the model
    if (opId == operators.size() - 1) {
      return;
    }
    next[operators[opId]] = operators[opId]->get_random_parallel_config(*this);
  }
}

void FFModel::mcmc_optimize(std::map<Op const *, ParallelConfig> &best,
                            size_t budget,
                            float alpha,
                            CompMode comp_mode,
                            bool use_propagation) const {
  // Start from data parallel
  std::map<Op const *, ParallelConfig> current, next;
  float best_runtime = simulator->simulate_runtime(this, best, comp_mode);
  current = best;
  float current_runtime = best_runtime;
  size_t reset_span = budget / 100, last_reset_iter = 0;
  if (reset_span == 0) {
    reset_span = 1;
  }
  if (reset_span > 1000) {
    reset_span = 1000;
  }
  for (size_t iter = 0; iter <= budget; iter++) {
    // Reset the current strategy to be the best strategy
    if (iter - last_reset_iter >= reset_span) {
      current = best;
      current_runtime = best_runtime;
      last_reset_iter = iter;
    }
    rewrite(current, next, use_propagation);
    float next_runtime = simulator->simulate_runtime(this, next, comp_mode);
    if (iter % 1000 == 0) {
      printf("iteration(%zu) current_strategy(%.4lf) best_strategy(%.4lf)\n",
             iter,
             current_runtime,
             best_runtime);
    }
    float rn = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    // float ratio = (next_runtime - current_runtime) / current_runtime;
    float diff = (next_runtime - current_runtime);
    if (next_runtime < best_runtime) {
      best_runtime = next_runtime;
      best = next;
    }
    if (next_runtime < current_runtime) {
      current = next;
      current_runtime = next_runtime;
    } else if (rn < std::exp(-alpha * diff)) {
      current = next;
      current_runtime = next_runtime;
    }
  }
  printf("=========== Best Discovered Strategy ==========\n");
  simulator->simulate_runtime(
      this, best, comp_mode, this->config.export_strategy_task_graph_file);
  std::map<Op const *, ParallelConfig>::const_iterator it;
  for (it = best.begin(); it != best.end(); it++) {
    printf("[%s] num_dims(%d) dims[", it->first->name, it->second.nDims);
    for (int i = 0; i < it->second.nDims; i++) {
      if (i < it->second.nDims - 1) {
        printf("%d,", it->second.dim[i]);
      } else {
        printf("%d", it->second.dim[i]);
      }
    }
    printf("] device_ids[");
    for (int i = 0; i < it->second.num_parts(); i++) {
      if (i < it->second.num_parts() - 1) {
        printf("%d,", it->second.device_ids[i]);
      } else {
        printf("%d", it->second.device_ids[i]);
      }
    }
    printf("]\n");
  }
  printf("============= MCMC Search Finished ============\n\n");
}

void FFModel::zero_gradients(void) {
  for (int l = operators.size() - 1; l >= 0; l--) {
    operators[l]->zero_grad(*this);
  }
}

void FFModel::print_layers(int id) {
  if (id == -1) {
    for (size_t i = 0; i < layers.size(); i++) {
      layers[i]->print();
    }
  } else {
    layers[id]->print();
  }
}

std::unordered_map<Op *, std::vector<std::pair<Op *, int>>>
    FFModel::get_bwd_edge_map() const {
  std::unordered_map<Op *, std::vector<std::pair<Op *, int>>> bwd_edge_map;
  for (auto const &op : this->operators) {
    for (int i = 0; i < op->numInputs; i++) {
      Op *src = (Op *)op->inputs[i]->owner_op;
      bwd_edge_map[src].push_back({op, op->inputs[i]->get_volume()});
    }
  }

  return bwd_edge_map;
};

PerfMetrics
    FFModel::update_metrics_task(Task const *task,
                                 std::vector<PhysicalRegion> const &regions,
                                 Context ctx,
                                 Runtime *runtime) {
  Metrics *m = (Metrics *)task->args;
  if (task->futures.size() == 0) {
    // Create an empty future
    PerfMetrics perf;
    return perf;
  }
  assert(task->futures.size() > 1);
  PerfMetrics all_metrics = task->futures[0].get_result<PerfMetrics>();
  for (size_t i = 1; i < task->futures.size(); i++) {
    PerfMetrics one_metrics = task->futures[i].get_result<PerfMetrics>();
    all_metrics.update(one_metrics);
  }
  all_metrics.print(m);
  // fprintf(stderr, "acc_train_loss: %.4lf train_accuracy: %.2lf%%(%d/%d)\n",
  //         all_metrics.train_loss / all_metrics.train_all,
  //         all_metrics.train_correct * 100.0f / all_metrics.train_all,
  //         all_metrics.train_correct, all_metrics.train_all);
  return all_metrics;
}

// TODO: Move to an appropriate place
template <>
std::tuple<> get_input_shape(std::tuple<> const &) {
  return std::tuple<>();
}

template <>
std::tuple<ParallelTensorShape, ParallelTensorShape, ParallelTensorShape>
    get_input_shape(
        std::tuple<ParallelTensor, ParallelTensor, ParallelTensor> const
            &inputs) {
  return std::make_tuple(std::get<0>(inputs)->get_shape(),
                         std::get<1>(inputs)->get_shape(),
                         std::get<2>(inputs)->get_shape());
}

template <>
ParallelTensorShape get_input_shape(ParallelTensor const &input) {
  return input->get_shape();
}

template <>
std::pair<ParallelTensorShape, ParallelTensorShape>
    get_input_shape(std::pair<ParallelTensor, ParallelTensor> const &inputs) {
  return std::make_pair(inputs.first->get_shape(), inputs.second->get_shape());
}

template <>
std::vector<ParallelTensorShape>
    get_input_shape(std::vector<ParallelTensor> const &inputs) {
  std::vector<ParallelTensorShape> shapes;
  for (auto const &input : inputs) {
    shapes.push_back(input->get_shape());
  }
  return shapes;
}

void Op::prefetch(FFModel const &ff) {
  // TODO: perform prefetch for performance imporvement
}

// ========================================================
// class FFIterationConfig
// ========================================================
FFIterationConfig::FFIterationConfig() {
  seq_length = -1;
}

void FFIterationConfig::reset() {
  seq_length = -1;
}

// ========================================================
// class FFConfig
// ========================================================

// Default Config Parameters
struct DefaultConfig {
  const static int epochs = 1;
  // const static int iterations = 1;
  const static int batchSize = 64;
  const static bool profiling = false;
  constexpr static float learningRate = 0.01f;
  constexpr static float weightDecay = 0.0001f;
  const static size_t workSpaceSize = (size_t)1 * 1024 * 1024 * 1024; // 2GB
  const static int numNodes = 1;
  const static int workersPerNode = 0;
  const static int cpusPerNode = 0;
  const static size_t searchBudget = -1;
  const static size_t simulatorWorkSpaceSize =
      (size_t)2 * 1024 * 1024 * 1024; // 2GB
  constexpr static float searchAlpha = 1.2f;
  const static bool searchOverlapBackwardUpdate = false;
  const static bool onlyDataParallel = false;
  const static bool enableSampleParallel = true;
  const static bool enableParameterParallel = false;
  const static bool enableAttributeParallel = false;
  const static bool enableInplaceOptimizations = false;
  const static bool allowTensorOpMathConversion = false;
  const static int machine_model_version = 0;
  const static int simulator_segment_size = 16777216; // 16 MB
  const static int simulator_max_num_segments = 1;
  const static int base_optimize_threshold = 10;
  const static bool enable_control_replication = true;
  // The default python data loader type is 2 to enable control replication
  const static int python_data_loader_type = 2;
};

FFConfig::FFConfig() {
  epochs = DefaultConfig::epochs;
  // iterations = DefaultConfig::iterations;
  batchSize = DefaultConfig::batchSize;
  profiling = DefaultConfig::profiling;
  learningRate = DefaultConfig::learningRate;
  weightDecay = DefaultConfig::weightDecay;
  workSpaceSize = DefaultConfig::workSpaceSize;
  numNodes = DefaultConfig::numNodes;
  cpusPerNode = DefaultConfig::cpusPerNode;
  workersPerNode = DefaultConfig::workersPerNode;
  simulator_work_space_size = DefaultConfig::simulatorWorkSpaceSize;
  search_budget = DefaultConfig::searchBudget;
  search_alpha = DefaultConfig::searchAlpha;
  search_overlap_backward_update = DefaultConfig::searchOverlapBackwardUpdate;
  computationMode = COMP_MODE_TRAINING;
  only_data_parallel = DefaultConfig::onlyDataParallel;
  enable_sample_parallel = DefaultConfig::enableSampleParallel;
  enable_parameter_parallel = DefaultConfig::enableParameterParallel;
  enable_attribute_parallel = DefaultConfig::enableAttributeParallel;
  enable_inplace_optimizations = DefaultConfig::enableInplaceOptimizations;
  allow_tensor_op_math_conversion = DefaultConfig::allowTensorOpMathConversion;
  machine_model_version = DefaultConfig::machine_model_version;
  simulator_segment_size = DefaultConfig::simulator_segment_size;
  simulator_max_num_segments = DefaultConfig::simulator_max_num_segments;
  enable_control_replication = DefaultConfig::enable_control_replication;
  python_data_loader_type = DefaultConfig::python_data_loader_type;
  machine_model_file = "";
  import_strategy_file = "";
  export_strategy_file = "";
  export_strategy_task_graph_file = "";
  include_costs_dot_graph = false;
  export_strategy_computation_graph_file = "";
  dataset_path = "";
  substitution_json_path = tl::nullopt;
  syntheticInput = false;
  perform_fusion = false;
  base_optimize_threshold = DefaultConfig::base_optimize_threshold;

  // Use Real::Machine::get_address_space_count() to obtain the number of nodes
  numNodes = Realm::Machine::get_machine().get_address_space_count();

  Runtime *runtime = Runtime::get_runtime();
  lg_hlr = runtime;
  lg_ctx = Runtime::get_context();
  field_space = runtime->create_field_space(lg_ctx);
}


// template instantiations
#define DIMFUNC(DIM)                                                           \
  template Tensor FFModel::create_tensor<DIM>(int const dims[],                \
                                              DataType data_type,              \
                                              Layer const *owner_op,           \
                                              int owner_idx,                   \
                                              bool create_grad);               \
  template ParallelTensor FFModel::create_parallel_tensor<DIM>(                \
      const ParallelDim dims[],                                                \
      DataType data_type,                                                      \
      Op const *owner_op,                                                      \
      int owner_idx,                                                           \
      bool create_grad,                                                        \
      size_t input_tensor_guid);                                               \
  template ParallelParameter FFModel::create_parallel_weight<DIM>(             \
      const ParallelDim dims[],                                                \
      DataType data_type,                                                      \
      Op const *owner_op,                                                      \
      bool create_grad,                                                        \
      Initializer *initializer,                                                \
      ParameterSyncType sync_type);                                            \
  template void FFModel::map_tensor_with_dim<DIM>(ParallelTensor tensor,       \
                                                  Op const *parallel_op);      \
  template void FFModel::map_weight_with_dim<DIM>(ParallelTensor weight,       \
                                                  Op const *parallel_op);      \
  template Tensor FFModel::create_constant<DIM>(                               \
      int const *dims, float value, DataType data_type);                       \
  template void FFModel::create_disjoint_partition<DIM>(                       \
      const ParallelTensor tensor,                                             \
      IndexSpaceT<DIM> const &part_is,                                         \
      LogicalPartition &part_fwd,                                              \
      LogicalPartition &part_bwd);
LEGION_FOREACH_N(DIMFUNC)
#undef DIMFUNC

#define DIMFUNC(D1, D2)                                                        \
  template void FFModel::map_tensor_with_dim2<D1, D2>(ParallelTensor tensor,   \
                                                      Op const *parallel_op);  \
  template void FFModel::create_disjoint_partition_with_dim2<D1, D2>(          \
      const ParallelDim dims[],                                                \
      IndexSpaceT<D2> const &part_is,                                          \
      LogicalRegion const &region,                                             \
      LogicalPartition &part);                                                 \
  template void FFModel::create_aliased_partition_with_dim2<D1, D2>(           \
      const ParallelDim dims[],                                                \
      int aliased_dim,                                                         \
      IndexSpaceT<D2> const &part_is,                                          \
      LogicalRegion const &region,                                             \
      LogicalPartition &part);                                                 \
  template void                                                                \
      FFModel::create_data_parallel_partition_with_diff_dims<D1, D2>(          \
          const ParallelTensor tensor,                                         \
          IndexSpaceT<D2> const &part_is,                                      \
          LogicalPartition &part_fwd,                                          \
          LogicalPartition &part_bwd);
LEGION_FOREACH_NN(DIMFUNC)
#undef DIMFUNC

template void FFModel::map_conv_weight<4>(ParallelTensor weight,
                                          Op const *parallel_op);
template void FFModel::map_conv_weight<1>(ParallelTensor weight,
                                          Op const *parallel_op);

#define DIMFUNC(D1, D2)                                                        \
  template void FFModel::map_linear_weight<D1, D2>(ParallelTensor p,           \
                                                   Op const *op);
LEGION_FOREACH_NN(DIMFUNC)
#undef DIMFUNC

#define DIMFUNC(D1, D2)                                                        \
  template ParallelTensor FFModel::create_linear_replica<D1>(                  \
      int const *dims, IndexSpaceT<D2> const &part_is, DataType data_type);
LEGION_FOREACH_NN(DIMFUNC)
#undef DIMFUNC

}; // namespace FlexFlow