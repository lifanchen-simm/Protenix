// Copyright 2021- HPC-AI Technology Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and

#include <torch/extension.h>
#include <c10/cuda/CUDAGuard.h>

#include <cassert>
#include <vector>
#include <functional>

#include "compat.h"

void compute_n1_n2(at::Tensor input, at::IntArrayRef normalized_shape, int& n1, int& n2) {
    int idiff = input.ndimension() - normalized_shape.size();
    n2 = 1;
    for (int i = 0; i < (int)normalized_shape.size(); ++i) {
        assert(input.sizes()[i + idiff] == normalized_shape[i]);
        n2 *= normalized_shape[i];
    }
    n1 = 1;
    for (int i = 0; i < idiff; ++i) {
        n1 *= input.sizes()[i];
    }
}

void check_args(at::Tensor input, at::IntArrayRef normalized_shape, int& n1, int& n2) {
    int64_t normalized_ndim = normalized_shape.size();

    if (normalized_ndim < 1) {
        std::stringstream ss;
        ss << "Expected normalized_shape to be at least 1-dimensional, i.e., "
           << "containing at least one element, but got normalized_shape=" << normalized_shape;
        throw std::runtime_error(ss.str());
    }

    auto input_shape = input.sizes();
    auto input_ndim = input.dim();

    if (input_ndim < normalized_ndim ||
        !input_shape.slice(input_ndim - normalized_ndim).equals(normalized_shape)) {
        std::stringstream ss;
        ss << "Given normalized_shape=" << normalized_shape << ", expected input with shape [*";
        for (auto size : normalized_shape) {
            ss << ", " << size;
        }
        ss << "], but got input of size" << input_shape;
        throw std::runtime_error(ss.str());
    }

    compute_n1_n2(input, normalized_shape, n1, n2);
}

void cuda_layer_norm(at::Tensor* output, at::Tensor* mean, at::Tensor* invvar, at::Tensor* input,
                     int n1, int n2, at::IntArrayRef normalized_shape, at::Tensor* gamma,
                     at::Tensor* beta, double epsilon);

#define CHECK_CUDA(x) TORCH_CHECK(x.is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) TORCH_CHECK(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) \
    CHECK_CUDA(x);     \
    CHECK_CONTIGUOUS(x)

std::vector<at::Tensor> layer_norm_affine(at::Tensor input, at::IntArrayRef normalized_shape,
                                          at::Tensor *gamma, at::Tensor *beta, double epsilon) {
    CHECK_INPUT(input);
    // CHECK_INPUT((*gamma));
    // CHECK_INPUT((*beta));
    int n1, n2;
    check_args(input, normalized_shape, n1, n2);

    const at::cuda::OptionalCUDAGuard device_guard(device_of(input));

    at::Tensor output = at::empty_like(input, input.options().dtype(input.scalar_type()));
    at::Tensor mean = at::empty({n1}, input.options().dtype(at::ScalarType::Float));
    at::Tensor invvar = at::empty_like(mean);

    cuda_layer_norm(&output, &mean, &invvar, &input, n1, n2, normalized_shape, gamma, beta, epsilon);

    return {output, mean, invvar};
}

void cuda_layer_norm_gradient(at::Tensor* dout, at::Tensor* mean, at::Tensor* invvar,
                              at::Tensor* input, int n1, int n2, at::IntArrayRef normalized_shape,
                              at::Tensor* gamma, at::Tensor* beta, double epsilon,
                              at::Tensor* grad_input, at::Tensor* grad_gamma,
                              at::Tensor* grad_beta);

std::vector<at::Tensor> layer_norm_gradient_affine(at::Tensor dout, at::Tensor mean,
                                                   at::Tensor invvar, at::Tensor input,
                                                   at::IntArrayRef normalized_shape,
                                                   at::Tensor* gamma, at::Tensor* beta,
                                                   double epsilon) {
    CHECK_INPUT(dout);
    CHECK_INPUT(mean);
    CHECK_INPUT(invvar);
    CHECK_INPUT(input);
    int n1, n2;
    check_args(input, normalized_shape, n1, n2);

    const at::cuda::OptionalCUDAGuard device_guard(device_of(input));

    at::Tensor grad_input = at::empty_like(input);

    at::Tensor grad_gamma;
    at::Tensor grad_beta;
    if (gamma != NULL)
        grad_gamma = at::empty_like(*gamma);
    if (beta != NULL)
        grad_beta = at::empty_like(*beta);
    
    if (gamma != NULL) {
        if(beta != NULL) {
            cuda_layer_norm_gradient(&dout, &mean, &invvar, &input, n1, n2, normalized_shape, gamma, beta,
                             epsilon, &grad_input, &grad_gamma, &grad_beta);
        } else {
            cuda_layer_norm_gradient(&dout, &mean, &invvar, &input, n1, n2, normalized_shape, gamma, beta,
                             epsilon, &grad_input, &grad_gamma, NULL);
        }
    } else {
        if(beta != NULL) {
            cuda_layer_norm_gradient(&dout, &mean, &invvar, &input, n1, n2, normalized_shape, gamma, beta,
                             epsilon, &grad_input, NULL, &grad_beta);
        } else {
            cuda_layer_norm_gradient(&dout, &mean, &invvar, &input, n1, n2, normalized_shape, gamma, beta,
                             epsilon, &grad_input, NULL, NULL);
        }
    }
    return {grad_input, grad_gamma, grad_beta};
}


PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
    m.def("forward_none_affine", [](at::Tensor input, at::IntArrayRef normalized_shape, double epsilon) {
        return layer_norm_affine(input, normalized_shape, NULL, NULL, epsilon);
    }, "LayerNorm forward (CUDA)");
    
    m.def("forward_with_bias_affine", [](at::Tensor input, at::IntArrayRef normalized_shape, at::Tensor *beta, double epsilon) {
        return layer_norm_affine(input, normalized_shape, NULL, beta, epsilon);
    }, "LayerNorm forward (CUDA)");
    
    m.def("forward_with_weight_affine", [](at::Tensor input, at::IntArrayRef normalized_shape, at::Tensor *gamma, double epsilon) {
        return layer_norm_affine(input, normalized_shape, gamma, NULL, epsilon);
    }, "LayerNorm forward (CUDA)");
    
    m.def("forward_with_both_affine", [](at::Tensor input, at::IntArrayRef normalized_shape, at::Tensor *gamma, at::Tensor *beta, double epsilon) {
        return layer_norm_affine(input, normalized_shape, gamma, beta, epsilon);
    }, "LayerNorm forward (CUDA)");
    
    m.def("backward_none_affine", [](at::Tensor dout, at::Tensor mean, at::Tensor invvar, at::Tensor input,
                                     at::IntArrayRef normalized_shape, double epsilon) {
        return layer_norm_gradient_affine(dout, mean, invvar, input, normalized_shape, NULL, NULL, epsilon);

    }, "LayerNorm backward (CUDA)");

    m.def("backward_with_bias_affine", [](at::Tensor dout, at::Tensor mean, at::Tensor invvar, at::Tensor input,
                                     at::IntArrayRef normalized_shape, at::Tensor *beta, double epsilon) {
        return layer_norm_gradient_affine(dout, mean, invvar, input, normalized_shape, NULL, beta, epsilon);
    }, "LayerNorm backward (CUDA)");

    m.def("backward_with_weight_affine", [](at::Tensor dout, at::Tensor mean, at::Tensor invvar, at::Tensor input,
                                     at::IntArrayRef normalized_shape, at::Tensor *gamma, double epsilon) {
        return layer_norm_gradient_affine(dout, mean, invvar, input, normalized_shape, gamma, NULL, epsilon);
    }, "LayerNorm backward (CUDA)");

    m.def("backward_with_both_affine", [](at::Tensor dout, at::Tensor mean, at::Tensor invvar, at::Tensor input,
                                     at::IntArrayRef normalized_shape, at::Tensor *gamma, at::Tensor *beta, double epsilon) {
        return layer_norm_gradient_affine(dout, mean, invvar, input, normalized_shape, gamma, beta, epsilon);
    }, "LayerNorm backward (CUDA)");
}
