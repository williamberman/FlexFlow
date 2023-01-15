#ifndef _FLEXFLOW_RESHAPE_PARAMS_H
#define _FLEXFLOW_RESHAPE_PARAMS_H

#include "op-meta/parallel_tensor_shape.h"

namespace FlexFlow {

struct ReshapeParams {
  std::vector<int> shape;
  bool is_valid(ParallelTensorShape const &) const;
};
bool operator==(ReshapeParams const &, ReshapeParams const &);

} // namespace FlexFlow

namespace std {
template <>
struct hash<FlexFlow::ReshapeParams> {
  size_t operator()(FlexFlow::ReshapeParams const &) const;
};
} // namespace std

#endif // _FLEXFLOW_RESHAPE_PARAMS_H