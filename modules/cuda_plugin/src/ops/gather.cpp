// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "gather.hpp"

#include <fmt/format.h>

#include <cuda_operation_registry.hpp>
#include <error.hpp>
#include <gsl/gsl_assert>
#include <ngraph/op/gather.hpp>
#include <ngraph/type/element_type.hpp>
#include <numeric>

#include "converters.hpp"

namespace CUDAPlugin {

namespace {

constexpr unsigned ELS_PER_THREAD_CHUNKS = 2;
constexpr unsigned ELS_PER_THREAD_DICTS = 1;

}  // namespace

GatherOp::GatherOp(const CreationContext& context,
                   const ngraph::Node& node,
                   IndexCollection&& inputIds,
                   IndexCollection&& outputIds)
    : OperationBase(context, node, std::move(inputIds), std::move(outputIds)) {
    Expects(node.get_input_size() == 3);
    Expects(node.get_output_size() == 1);
    const auto gather_v7 = dynamic_cast<const ngraph::op::v7::Gather*>(&node);
    const auto gather_v1 = dynamic_cast<const ngraph::op::v1::Gather*>(&node);
    Expects(gather_v7 || gather_v1);

    const auto gather_base = dynamic_cast<const ngraph::op::util::GatherBase*>(&node);
    Expects(gather_base);

    // For now CUDA operators support only static shapes
    Expects(node.get_input_partial_shape(0).rank().is_static() && node.get_input_partial_shape(1).rank().is_static() &&
            node.get_input_partial_shape(2).rank().is_static());

    const ngraph::element::Type_t element_type = node.get_input_element_type(0);
    switch (element_type) {
        case ngraph::element::Type_t::undefined:
        case ngraph::element::Type_t::dynamic:
        case ngraph::element::Type_t::u1:
            throwIEException(fmt::format("Params element type = {} is not supported by Gather operation!",
                                         static_cast<ngraph::element::Type_t>(element_type)));
    }
    Expects(node.get_output_element_type(0) == element_type);

    const auto& dict_shape = node.get_input_shape(0);
    const auto& dict_shape_size = dict_shape.size();
    const auto& indices_shape = node.get_input_shape(1);
    const auto& out_shape = node.get_output_shape(0);
    const auto axis_shape = node.get_input_shape(2);

    const ngraph::element::Type_t indices_type = node.get_input_element_type(1);
    if (indices_type != ngraph::element::Type_t::i64 && indices_type != ngraph::element::Type_t::i32) {
        throwIEException(fmt::format("Params index type = {} is not supported by Gather operation!", indices_type));
    }

    const auto axis = gather_base->get_axis();
    Expects(axis >= 0 && axis < dict_shape_size);

    int64_t batch_dims = 0;
    if (gather_v7) {
        batch_dims = gather_v7->get_batch_dims();
        Expects(batch_dims >= 0 && batch_dims < dict_shape_size && batch_dims < indices_shape.size() &&
                batch_dims <= axis);

        bool batch_check_ok = true;
        for (int i = 0; i < batch_dims; ++i) {
            if (dict_shape[i] != indices_shape[i]) {
                batch_check_ok = false;
                break;
            }
        }
        Expects(batch_check_ok);
    }

    const unsigned num_dicts =
        std::accumulate(dict_shape.cbegin() + batch_dims, dict_shape.cbegin() + axis, 1, std::multiplies<unsigned>());
    const unsigned index_range = dict_shape[axis];
    const unsigned data_length =
        std::accumulate(dict_shape.cbegin() + axis + 1, dict_shape.cend(), 1, std::multiplies<unsigned>());

    if (data_length == 0) {
        throwIEException("data_length == 0: incorrect input parameters dimension!");
    }

    const unsigned indices_size =
        std::accumulate(indices_shape.cbegin() + batch_dims, indices_shape.cend(), 1, std::multiplies<unsigned>());
    const auto out_size =
        std::accumulate(out_shape.cbegin() + batch_dims, out_shape.cend(), 1, std::multiplies<unsigned>());

    const auto batch_count =
        std::accumulate(dict_shape.cbegin(), dict_shape.cbegin() + batch_dims, 1, std::multiplies<unsigned>());
    const unsigned dicts_batch_stride =
        std::accumulate(dict_shape.cbegin() + batch_dims, dict_shape.cend(), 1, std::multiplies<unsigned>());
    const unsigned indices_batch_stride =
        std::accumulate(indices_shape.cbegin() + batch_dims, indices_shape.cend(), 1, std::multiplies<unsigned>());
    const unsigned out_batch_stride =
        std::accumulate(out_shape.cbegin() + batch_dims, out_shape.cend(), 1, std::multiplies<unsigned>());

    const auto max_indices_index = indices_size - 1;
    const auto max_dict_index = num_dicts - 1;

    const bool boundary_ok =
        data_length <= out_size - (data_length * (max_indices_index + max_dict_index * indices_size));
    Expects(boundary_ok);

    const unsigned num_chunks = data_length % ELS_PER_THREAD_CHUNKS == 0 ? data_length / ELS_PER_THREAD_CHUNKS
                                                                         : data_length / ELS_PER_THREAD_CHUNKS + 1;

    const auto& device_props = context.device().props();
    const auto max_block_size = device_props.maxThreadsPerBlock;
    const auto max_grid_size = device_props.maxGridSize;

    const bool gather_chunks = std::max(num_chunks, num_dicts) == num_chunks;

    unsigned blocks_per_grid{};
    unsigned threads_per_block{};
    unsigned grid_dim_x{};

    if (gather_chunks) {
        blocks_per_grid =
            num_chunks % max_block_size == 0 ? num_chunks / max_block_size : num_chunks / max_block_size + 1;
        threads_per_block = blocks_per_grid == 1 ? num_chunks : max_block_size;
        grid_dim_x = num_dicts * batch_count;

        Expects(grid_dim_x <= max_grid_size[0]);
        Expects(indices_size <= max_grid_size[1]);
        Expects(blocks_per_grid <= max_grid_size[2]);
    } else {
        blocks_per_grid = num_dicts % max_block_size == 0 ? num_dicts / max_block_size : num_dicts / max_block_size + 1;
        threads_per_block = blocks_per_grid == 1 ? num_dicts : max_block_size;
        grid_dim_x = data_length * batch_count;

        Expects(grid_dim_x <= max_grid_size[0]);
        Expects(indices_size <= max_grid_size[1]);
        Expects(blocks_per_grid <= max_grid_size[2]);
    }

    gather_kernel_ = kernel::Gather{convertDataType<CUDAPlugin::kernel::Type_t>(element_type),
                                    convertDataType<CUDAPlugin::kernel::Type_t>(indices_type),
                                    num_dicts,
                                    index_range,
                                    data_length,
                                    indices_size,
                                    gather_chunks,
                                    blocks_per_grid,
                                    threads_per_block,
                                    grid_dim_x,
                                    dicts_batch_stride,
                                    indices_batch_stride,
                                    out_batch_stride,
                                    ELS_PER_THREAD_CHUNKS,
                                    ELS_PER_THREAD_DICTS};
}

void GatherOp::Execute(const InferenceRequestContext& context,
                       Inputs inputs,
                       Outputs outputs,
                       const Workbuffers&) const {
    Expects(inputs.size() == 3);
    Expects(outputs.size() == 1);

    (*gather_kernel_)(context.getThreadContext().stream().get(), inputs[0].get(), inputs[1].get(), outputs[0].get());
}

OPERATION_REGISTER(GatherOp, Gather);
}  // namespace CUDAPlugin