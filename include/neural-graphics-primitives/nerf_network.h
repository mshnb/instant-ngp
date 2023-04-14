/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/** @file   nerf_network.h
 *  @author Thomas Müller, NVIDIA
 *  @brief  A network that first processes 3D position to density and
 *          subsequently direction to color.
 */

#pragma once

#include <tiny-cuda-nn/common.h>

#include <tiny-cuda-nn/encoding.h>
#include <tiny-cuda-nn/gpu_matrix.h>
#include <tiny-cuda-nn/gpu_memory.h>
#include <tiny-cuda-nn/multi_stream.h>
#include <tiny-cuda-nn/network.h>

#include <tiny-cuda-nn/network_with_input_encoding.h>

NGP_NAMESPACE_BEGIN

template <typename T>
__global__ void extract_density(
	const uint32_t n_elements,
	const uint32_t density_stride,
	const uint32_t rgbd_stride,
	const T* __restrict__ density,
	T* __restrict__ rgbd
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	rgbd[i * rgbd_stride] = density[i * density_stride];
}

template <typename T>
__global__ void extract_uv(
	const uint32_t n_elements,
	const uint32_t uv_stride,
	const uint32_t output_stride,
	const T* __restrict__ uv,
	T* __restrict__ output
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	output[i * output_stride] = uv[i * uv_stride];
}

template <typename T>
__global__ void extract_rgb(
	const uint32_t n_elements,
	const uint32_t rgb_stride,
	const uint32_t output_stride,
	const T* __restrict__ rgbd,
	T* __restrict__ rgb
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	const uint32_t elem_idx = i / 3;
	const uint32_t dim_idx = i - elem_idx * 3;

	rgb[elem_idx*rgb_stride + dim_idx] = rgbd[elem_idx*output_stride + dim_idx];
}

template <typename T>
__global__ void add_density_gradient(
	const uint32_t n_elements,
	const uint32_t rgbd_stride,
	const T* __restrict__ rgbd,
	const uint32_t density_stride,
	T* __restrict__ density
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	density[i * density_stride] += rgbd[i * rgbd_stride + 3];
}

template <typename T>
__global__ void add_gradient(
	const uint32_t n_elements,
	const T* __restrict__ input,
	T* __restrict__ output
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	output[i] += input[i];
}

template <typename T>
__global__ void scale_gradient(
	const uint32_t n_elements,
	const float scale,
	T* __restrict__ output
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	output[i] = (T)scale * output[i];
}

template <typename T>
__global__ void repeat_vec(
	const uint32_t n_elements,
	const uint32_t output_stride,
	const vec3 input,
	T* output
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	T* v = &output[i * output_stride];

	v[0] = input.x;
	v[1] = input.y;
	v[2] = input.z;
}

template <typename T>
__global__ void generate_uv_grid(
	const uint32_t n_elements,
	const uint32_t tex_size,
	T* __restrict__ output
) {
	const uint32_t i = threadIdx.x + blockIdx.x * blockDim.x;
	if (i >= n_elements) return;

	const uint32_t y = i / tex_size;
	const uint32_t x = i - y * tex_size;

	output[i] = (T)x / (T)tex_size;
	output[i + n_elements] = (T)y / (T)tex_size;
}

template <typename T>
class NerfNetwork : public tcnn::Network<float, T> {
public:
	using json = nlohmann::json;

	NerfNetwork(uint32_t n_pos_dims, uint32_t n_dir_dims, uint32_t n_extra_dims, uint32_t dir_offset, const json& pos_encoding, const json& dir_encoding, const json& density_network, const json& uv_network, const json& rgb_network) : m_n_pos_dims{n_pos_dims}, m_n_dir_dims{n_dir_dims}, m_dir_offset{dir_offset}, m_n_extra_dims{n_extra_dims} {
		m_pos_encoding.reset(tcnn::create_encoding<T>(n_pos_dims, pos_encoding, density_network.contains("otype") && (tcnn::equals_case_insensitive(density_network["otype"], "FullyFusedMLP") || tcnn::equals_case_insensitive(density_network["otype"], "MegakernelMLP")) ? 16u : 8u));

		uint32_t rgb_alignment = tcnn::minimum_alignment(rgb_network);
		m_dir_encoding.reset(tcnn::create_encoding<T>(m_n_dir_dims + m_n_extra_dims, dir_encoding, rgb_alignment));

		json local_density_network_config = density_network;
		local_density_network_config["n_input_dims"] = m_pos_encoding->padded_output_width();
		if (!density_network.contains("n_output_dims")) {
			local_density_network_config["n_output_dims"] = 1; // 16;
		}
		m_density_network.reset(tcnn::create_network<T>(local_density_network_config));

		json local_uv_network_config = uv_network;
		local_uv_network_config["n_input_dims"] = m_pos_encoding->padded_output_width();
		if (!uv_network.contains("n_output_dims")) {
			local_uv_network_config["n_output_dims"] = 2;
		}
		m_uv_network.reset(tcnn::create_network<T>(local_uv_network_config));

		//m_uv_encoding.reset(tcnn::create_encoding<T>(local_uv_network_config["n_output_dims"], uv_encoding, 16u));

		//m_rgb_network_input_width = tcnn::next_multiple(m_dir_encoding->padded_output_width() + m_uv_encoding->padded_output_width() + m_density_network->padded_output_width(), rgb_alignment);
		m_rgb_network_input_width = tcnn::next_multiple(m_dir_encoding->padded_output_width() + m_uv_network->padded_output_width(), rgb_alignment);

		json local_rgb_network_config = rgb_network;
		local_rgb_network_config["n_input_dims"] = m_rgb_network_input_width;
		local_rgb_network_config["n_output_dims"] = 3;
		m_rgb_network.reset(tcnn::create_network<T>(local_rgb_network_config));
	}

	virtual ~NerfNetwork() { }

	void inference_mixed_precision_impl(cudaStream_t stream, const tcnn::GPUMatrixDynamic<float>& input, tcnn::GPUMatrixDynamic<T>& output, bool use_inference_params = true) override {
		uint32_t batch_size = input.n();
		tcnn::GPUMatrixDynamic<T> density_network_input{m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout()};
		tcnn::GPUMatrixDynamic<T> rgb_network_input{m_rgb_network_input_width, batch_size, stream, m_dir_encoding->preferred_output_layout()};

		//tcnn::GPUMatrixDynamic<T> density_network_output = rgb_network_input.slice_rows(m_dir_encoding->padded_output_width() + m_uv_encoding->padded_output_width(), m_density_network->padded_output_width());
		tcnn::GPUMatrixDynamic<T> density_network_output = tcnn::GPUMatrixDynamic<T>{m_density_network->padded_output_width(), batch_size, stream};
		//tcnn::GPUMatrixDynamic<T> uv_network_output{ m_uv_network->padded_output_width(), batch_size, stream };
		//tcnn::GPUMatrixDynamic<T> uv_encoding_output = rgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), m_uv_encoding->padded_output_width());

		tcnn::GPUMatrixDynamic<T> uv_network_output = rgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), m_uv_network->padded_output_width());
		tcnn::GPUMatrixDynamic<T> rgb_network_output{output.data(), m_rgb_network->padded_output_width(), batch_size, output.layout()};

		//density
		m_pos_encoding->inference_mixed_precision(
			stream,
			input.slice_rows(0, m_pos_encoding->input_width()),
			density_network_input,
			use_inference_params
		);
		m_density_network->inference_mixed_precision(
			stream,
			density_network_input,
			density_network_output,
			use_inference_params
		);

		//uv
		m_uv_network->inference_mixed_precision(
			stream,
			density_network_input,
			uv_network_output,
			use_inference_params
		);

		fill_unused_rgb_input(stream, rgb_network_input.data(), batch_size);

		//tcnn::GPUMatrixDynamic<float> uv_encoding_input{ m_uv_encoding->input_width(), batch_size, stream };
		//tcnn::linear_kernel(tcnn::cast_from<T>, 0, stream, (uint32_t)uv_encoding_input.n_elements(), uv_network_output.data(), uv_encoding_input.data());
		//m_uv_encoding->inference_mixed_precision(
		//	stream,
		//	uv_encoding_input,
		//	uv_encoding_output,
		//	use_inference_params
		//);

		auto dir_out = rgb_network_input.slice_rows(0, m_dir_encoding->padded_output_width());
		m_dir_encoding->inference_mixed_precision(
			stream,
			input.slice_rows(m_dir_offset, m_dir_encoding->input_width()),
			dir_out,
			use_inference_params
		);

		m_rgb_network->inference_mixed_precision(
			stream,
			rgb_network_input,
			rgb_network_output,
			use_inference_params
		);

		tcnn::linear_kernel(extract_density<T>, 0, stream,
			batch_size,
			density_network_output.layout() == tcnn::AoS ? density_network_output.stride() : 1,
			output.layout() == tcnn::AoS ? padded_output_width() : 1,
			density_network_output.data(),
			output.data() + 3 * (output.layout() == tcnn::AoS ? 1 : batch_size)
		);

		//u
		tcnn::linear_kernel(extract_uv<T>, 0, stream,
			batch_size,
			uv_network_output.layout() == tcnn::AoS ? uv_network_output.stride() : 1,
			output.layout() == tcnn::AoS ? padded_output_width() : 1,
			uv_network_output.data(),
			output.data() + 4 * (output.layout() == tcnn::AoS ? 1 : batch_size)
		);

		//v
		tcnn::linear_kernel(extract_uv<T>, 0, stream,
			batch_size,
			uv_network_output.layout() == tcnn::AoS ? uv_network_output.stride() : 1,
			output.layout() == tcnn::AoS ? padded_output_width() : 1,
			uv_network_output.data() + (uv_network_output.layout() == tcnn::AoS ? 1 : batch_size),
			output.data() + 5 * (output.layout() == tcnn::AoS ? 1 : batch_size)
		);
	}

	uint32_t padded_density_output_width() const {
		return m_density_network->padded_output_width();
	}

	std::unique_ptr<tcnn::Context> forward_impl(cudaStream_t stream, const tcnn::GPUMatrixDynamic<float>& input, tcnn::GPUMatrixDynamic<T>* output = nullptr, bool use_inference_params = false, bool prepare_input_gradients = false) override {
		// Make sure our temporary buffers have the correct size for the given batch size
		uint32_t batch_size = input.n();

		auto forward = std::make_unique<ForwardContext>();

		forward->density_network_input = tcnn::GPUMatrixDynamic<T>{m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout()};
		//forward->uv_encoding_input = tcnn::GPUMatrixDynamic<float>{ m_uv_encoding->input_width(), batch_size, stream };
		forward->rgb_network_input = tcnn::GPUMatrixDynamic<T>{m_rgb_network_input_width, batch_size, stream, m_dir_encoding->preferred_output_layout()};

		//density
		forward->pos_encoding_ctx = m_pos_encoding->forward(
			stream,
			input.slice_rows(0, m_pos_encoding->input_width()),
			&forward->density_network_input,
			use_inference_params,
			prepare_input_gradients
		);

		//forward->density_network_output = forward->rgb_network_input.slice_rows(m_dir_encoding->padded_output_width() + m_uv_encoding->padded_output_width(), m_density_network->padded_output_width());
		forward->density_network_output = tcnn::GPUMatrixDynamic<T>{ m_density_network->padded_output_width(), batch_size, stream };
		forward->density_network_ctx = m_density_network->forward(
			stream,
			forward->density_network_input,
			&forward->density_network_output,
			use_inference_params,
			prepare_input_gradients
		);

		//uv
		//forward->uv_network_output = tcnn::GPUMatrixDynamic<T>{ m_uv_network->padded_output_width(), batch_size, stream };
		forward->uv_network_output = forward->rgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), m_uv_network->padded_output_width());
		forward->uv_network_ctx = m_uv_network->forward(
			stream,
			forward->density_network_input,
			&forward->uv_network_output,
			use_inference_params,
			prepare_input_gradients
		);

		fill_unused_rgb_input(stream, forward->rgb_network_input.data(), batch_size);

		//tcnn::linear_kernel(tcnn::cast_from<T>, 0, stream, (uint32_t)forward->uv_encoding_input.n_elements(), forward->uv_network_output.data(), forward->uv_encoding_input.data());
		//forward->uv_encoding_output = forward->rgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), m_uv_encoding->padded_output_width());
		//forward->uv_encoding_ctx = m_uv_encoding->forward(
		//	stream,
		//	forward->uv_encoding_input,
		//	&forward->uv_encoding_output,
		//	use_inference_params,
		//	true
		//);

		auto dir_out = forward->rgb_network_input.slice_rows(0, m_dir_encoding->padded_output_width());
		forward->dir_encoding_ctx = m_dir_encoding->forward(
			stream,
			input.slice_rows(m_dir_offset, m_dir_encoding->input_width()),
			&dir_out,
			use_inference_params,
			prepare_input_gradients
		);

		if (output) {
			forward->rgb_network_output = tcnn::GPUMatrixDynamic<T>{output->data(), m_rgb_network->padded_output_width(), batch_size, output->layout()};
		}

		forward->rgb_network_ctx = m_rgb_network->forward(stream, forward->rgb_network_input, output ? &forward->rgb_network_output : nullptr, use_inference_params, prepare_input_gradients);

		if (output) {
			tcnn::linear_kernel(extract_density<T>, 0, stream,
				batch_size,
				m_dir_encoding->preferred_output_layout() == tcnn::AoS ? forward->density_network_output.stride() : 1,
				padded_output_width(),
				forward->density_network_output.data(),
				output->data() + 3
			);

			//u
			//tcnn::linear_kernel(extract_density<T>, 0, stream,
			//	batch_size,
			//	forward->uv_network_output.layout() == tcnn::AoS ? forward->uv_network_output.stride() : 1,
			//	padded_output_width(),
			//	forward->uv_network_output.data(),
			//	output->data() + 4
			//);

			//v
			//tcnn::linear_kernel(extract_density<T>, 0, stream,
			//	batch_size,
			//	forward->uv_network_output.layout() == tcnn::AoS ? forward->uv_network_output.stride() : 1,
			//	padded_output_width(),
			//	forward->uv_network_output.data() + (forward->uv_network_output.layout() == tcnn::AoS ? 1 : batch_size),
			//	output->data() + 5
			//);
		}

		return forward;
	}

	void backward_impl(
		cudaStream_t stream,
		const tcnn::Context& ctx,
		const tcnn::GPUMatrixDynamic<float>& input,
		const tcnn::GPUMatrixDynamic<T>& output,
		const tcnn::GPUMatrixDynamic<T>& dL_doutput,
		tcnn::GPUMatrixDynamic<float>* dL_dinput = nullptr,
		bool use_inference_params = false,
		tcnn::EGradientMode param_gradients_mode = tcnn::EGradientMode::Overwrite
	) override {
		const auto& forward = dynamic_cast<const ForwardContext&>(ctx);

		// Make sure our teporary buffers have the correct size for the given batch size
		uint32_t batch_size = input.n();

		tcnn::GPUMatrix<T> dL_drgb{m_rgb_network->padded_output_width(), batch_size, stream};
		CUDA_CHECK_THROW(cudaMemsetAsync(dL_drgb.data(), 0, dL_drgb.n_bytes(), stream));
		tcnn::linear_kernel(extract_rgb<T>, 0, stream,
			batch_size*3, dL_drgb.m(), dL_doutput.m(), dL_doutput.data(), dL_drgb.data()
		);

		const tcnn::GPUMatrixDynamic<T> rgb_network_output{(T*)output.data(), m_rgb_network->padded_output_width(), batch_size, output.layout()};
		tcnn::GPUMatrixDynamic<T> dL_drgb_network_input{m_rgb_network_input_width, batch_size, stream, m_dir_encoding->preferred_output_layout()};
		m_rgb_network->backward(
			stream,
			*forward.rgb_network_ctx,
			forward.rgb_network_input,
			rgb_network_output,
			dL_drgb,
			&dL_drgb_network_input,
			use_inference_params,
			param_gradients_mode
		);

		// Backprop through dir encoding if it is trainable or if we need input gradients
		if (m_dir_encoding->n_params() > 0 || dL_dinput) {
			tcnn::GPUMatrixDynamic<T> dL_ddir_encoding_output = dL_drgb_network_input.slice_rows(0, m_dir_encoding->padded_output_width());
			tcnn::GPUMatrixDynamic<float> dL_ddir_encoding_input;
			if (dL_dinput) {
				dL_ddir_encoding_input = dL_dinput->slice_rows(m_dir_offset, m_dir_encoding->input_width());
			}

			m_dir_encoding->backward(
				stream,
				*forward.dir_encoding_ctx,
				input.slice_rows(m_dir_offset, m_dir_encoding->input_width()),
				forward.rgb_network_input.slice_rows(0, m_dir_encoding->padded_output_width()),
				dL_ddir_encoding_output,
				dL_dinput ? &dL_ddir_encoding_input : nullptr,
				use_inference_params,
				param_gradients_mode
			);
		}

		//density
		//tcnn::GPUMatrixDynamic<T> dL_ddensity_network_output = dL_drgb_network_input.slice_rows(m_dir_encoding->padded_output_width() + m_uv_encoding->padded_output_width(), m_density_network->padded_output_width());
		tcnn::GPUMatrixDynamic<T> dL_ddensity_network_output = tcnn::GPUMatrixDynamic<T>{ m_density_network->padded_output_width(), batch_size, stream };
		CUDA_CHECK_THROW(cudaMemsetAsync(dL_ddensity_network_output.data(), 0, dL_ddensity_network_output.n_bytes(), stream));
		tcnn::linear_kernel(add_density_gradient<T>, 0, stream,
			batch_size,
			dL_doutput.m(),
			dL_doutput.data(),
			dL_ddensity_network_output.layout() == tcnn::RM ? 1 : dL_ddensity_network_output.stride(),
			dL_ddensity_network_output.data()
		);

		tcnn::GPUMatrixDynamic<T> dL_ddensity_network_input;
		if (m_pos_encoding->n_params() > 0 || dL_dinput) {
			dL_ddensity_network_input = tcnn::GPUMatrixDynamic<T>{m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout()};
		}

		m_density_network->backward(
			stream,
			*forward.density_network_ctx,
			forward.density_network_input,
			forward.density_network_output,
			dL_ddensity_network_output,
			dL_ddensity_network_input.data() ? &dL_ddensity_network_input : nullptr,
			use_inference_params,
			param_gradients_mode
		);

		//uv encoding
		//tcnn::GPUMatrixDynamic<T> dL_duv_encoding_output = dL_drgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), m_uv_encoding->padded_output_width());
		//tcnn::GPUMatrixDynamic<T> dL_duv_network_output{ m_uv_network->padded_output_width(), batch_size, stream };

		//if (m_uv_encoding->n_params() > 0) {
		//	tcnn::GPUMatrixDynamic<float> dL_duv_encoding_input{ m_uv_encoding->input_width(), batch_size, stream };

		//	m_uv_encoding->backward(
		//		stream,
		//		*forward.uv_encoding_ctx,
		//		forward.uv_encoding_input,
		//		forward.uv_encoding_output,
		//		dL_duv_encoding_output,
		//		&dL_duv_encoding_input,
		//		use_inference_params,
		//		param_gradients_mode
		//	);

		//	//covert float to half
		//	CUDA_CHECK_THROW(cudaMemsetAsync(dL_duv_network_output.data(), 0, dL_duv_network_output.n_bytes(), stream));
		//	tcnn::linear_kernel(tcnn::cast<T>, 0, stream, (uint32_t)dL_duv_encoding_input.n_elements(), dL_duv_encoding_input.data(), dL_duv_network_output.data());
		//}
		//else {
		//	CUDA_CHECK_THROW(cudaMemcpy(dL_duv_network_output.data(), dL_duv_encoding_output.data(), dL_duv_encoding_output.n_bytes(), cudaMemcpyDeviceToDevice));
		//}

		tcnn::GPUMatrixDynamic<T> dL_duv_network_output = dL_drgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), m_uv_network->padded_output_width());
		fill_unused_rgb_input(stream, dL_drgb_network_input.data(), batch_size);

		//scale uv network's grandient
		tcnn::linear_kernel(scale_gradient<T>, 0, stream,
			dL_duv_network_output.n_elements(),
			m_uv_network_scale,
			dL_duv_network_output.data()
		);

		tcnn::GPUMatrixDynamic<T> dL_duv_network_input = tcnn::GPUMatrixDynamic<T>{ m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout() };
		m_uv_network->backward(
			stream,
			*forward.uv_network_ctx,
			forward.density_network_input,
			forward.uv_network_output,
			dL_duv_network_output,
			&dL_duv_network_input,
			use_inference_params,
			param_gradients_mode
		);

		//unscale uv network's grandient
		//tcnn::linear_kernel(scale_gradient<T>, 0, stream,
		//	dL_duv_network_input.n_elements(),
		//	1.f / m_uv_network_scale,
		//	dL_duv_network_input.data()
		//);

		if (dL_ddensity_network_input.data()) {
			tcnn::linear_kernel(add_gradient<T>, 0, stream,
				dL_ddensity_network_input.n_elements(),
				dL_duv_network_input.data(),
				dL_ddensity_network_input.data()
			);
		}

		// Backprop through pos encoding if it is trainable or if we need input gradients
		if (dL_ddensity_network_input.data()) {
			tcnn::GPUMatrixDynamic<float> dL_dpos_encoding_input;
			if (dL_dinput) {
				dL_dpos_encoding_input = dL_dinput->slice_rows(0, m_pos_encoding->input_width());
			}

			m_pos_encoding->backward(
				stream,
				*forward.pos_encoding_ctx,
				input.slice_rows(0, m_pos_encoding->input_width()),
				forward.density_network_input,
				dL_ddensity_network_input,
				dL_dinput ? &dL_dpos_encoding_input : nullptr,
				use_inference_params,
				param_gradients_mode
			);
		}
	}

	void density(cudaStream_t stream, const tcnn::GPUMatrixDynamic<float>& input, tcnn::GPUMatrixDynamic<T>& output, bool use_inference_params = true) {
		if (input.layout() != tcnn::CM) {
			throw std::runtime_error("NerfNetwork::density input must be in column major format.");
		}

		uint32_t batch_size = output.n();
		tcnn::GPUMatrixDynamic<T> density_network_input{m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout()};

		m_pos_encoding->inference_mixed_precision(
			stream,
			input.slice_rows(0, m_pos_encoding->input_width()),
			density_network_input,
			use_inference_params
		);

		m_density_network->inference_mixed_precision(stream, density_network_input, output, use_inference_params);
	}

	std::unique_ptr<tcnn::Context> density_forward(cudaStream_t stream, const tcnn::GPUMatrixDynamic<float>& input, tcnn::GPUMatrixDynamic<T>* output = nullptr, bool use_inference_params = false, bool prepare_input_gradients = false) {
		if (input.layout() != tcnn::CM) {
			throw std::runtime_error("NerfNetwork::density_forward input must be in column major format.");
		}

		// Make sure our temporary buffers have the correct size for the given batch size
		uint32_t batch_size = input.n();

		auto forward = std::make_unique<ForwardContext>();

		forward->density_network_input = tcnn::GPUMatrixDynamic<T>{m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout()};

		forward->pos_encoding_ctx = m_pos_encoding->forward(
			stream,
			input.slice_rows(0, m_pos_encoding->input_width()),
			&forward->density_network_input,
			use_inference_params,
			prepare_input_gradients
		);

		if (output) {
			forward->density_network_output = tcnn::GPUMatrixDynamic<T>{output->data(), m_density_network->padded_output_width(), batch_size, output->layout()};
		}

		forward->density_network_ctx = m_density_network->forward(stream, forward->density_network_input, output ? &forward->density_network_output : nullptr, use_inference_params, prepare_input_gradients);

		return forward;
	}

	void density_backward(
		cudaStream_t stream,
		const tcnn::Context& ctx,
		const tcnn::GPUMatrixDynamic<float>& input,
		const tcnn::GPUMatrixDynamic<T>& output,
		const tcnn::GPUMatrixDynamic<T>& dL_doutput,
		tcnn::GPUMatrixDynamic<float>* dL_dinput = nullptr,
		bool use_inference_params = false,
		tcnn::EGradientMode param_gradients_mode = tcnn::EGradientMode::Overwrite
	) {
		if (input.layout() != tcnn::CM || (dL_dinput && dL_dinput->layout() != tcnn::CM)) {
			throw std::runtime_error("NerfNetwork::density_backward input must be in column major format.");
		}

		const auto& forward = dynamic_cast<const ForwardContext&>(ctx);

		// Make sure our temporary buffers have the correct size for the given batch size
		uint32_t batch_size = input.n();

		tcnn::GPUMatrixDynamic<T> dL_ddensity_network_input;
		if (m_pos_encoding->n_params() > 0 || dL_dinput) {
			dL_ddensity_network_input = tcnn::GPUMatrixDynamic<T>{m_pos_encoding->padded_output_width(), batch_size, stream, m_pos_encoding->preferred_output_layout()};
		}

		m_density_network->backward(stream, *forward.density_network_ctx, forward.density_network_input, output, dL_doutput, dL_ddensity_network_input.data() ? &dL_ddensity_network_input : nullptr, use_inference_params, param_gradients_mode);

		// Backprop through pos encoding if it is trainable or if we need input gradients
		if (dL_ddensity_network_input.data()) {
			tcnn::GPUMatrixDynamic<float> dL_dpos_encoding_input;
			if (dL_dinput) {
				dL_dpos_encoding_input = dL_dinput->slice_rows(0, m_pos_encoding->input_width());
			}

			m_pos_encoding->backward(
				stream,
				*forward.pos_encoding_ctx,
				input.slice_rows(0, m_pos_encoding->input_width()),
				forward.density_network_input,
				dL_ddensity_network_input,
				dL_dinput ? &dL_dpos_encoding_input : nullptr,
				use_inference_params,
				param_gradients_mode
			);
		}
	}

	void uv2texture(cudaStream_t stream, uint32_t texture_size, const vec3& dir, tcnn::GPUMatrixDynamic<T>& output) {
		uint32_t batch_size = output.n();

		tcnn::GPUMatrixDynamic<float> dir_encoding_input{ m_dir_encoding->input_width(), batch_size, stream };
		tcnn::linear_kernel(repeat_vec<float>, 0, stream,
			batch_size,
			dir_encoding_input.m(),
			dir,
			dir_encoding_input.data()
		);

		tcnn::GPUMatrixDynamic<T> rgb_network_input{ m_rgb_network_input_width, batch_size, stream, m_dir_encoding->preferred_output_layout() };
		tcnn::GPUMatrixDynamic<T> dir_encoding_output = rgb_network_input.slice_rows(0, m_dir_encoding->padded_output_width());
		m_dir_encoding->inference_mixed_precision(
			stream,
			dir_encoding_input,
			dir_encoding_output
		);

		if (!m_uv_grid.data()) {
			m_uv_grid = tcnn::GPUMatrixDynamic<T>{ 2, batch_size, stream, rgb_network_input.layout() }; //rm
			tcnn::linear_kernel(generate_uv_grid<T>, 0, stream,
				batch_size,
				texture_size,
				m_uv_grid.data()
			);
		}

		tcnn::GPUMatrixDynamic<T> uv_grid = rgb_network_input.slice_rows(m_dir_encoding->padded_output_width(), 2);
		CUDA_CHECK_THROW(cudaMemcpyAsync(uv_grid.data(), m_uv_grid.data(), m_uv_grid.n_bytes(), cudaMemcpyDeviceToDevice, stream));
		fill_unused_rgb_input(stream, rgb_network_input.data(), batch_size);

		m_rgb_network->inference_mixed_precision(
			stream,
			rgb_network_input,
			output
		);
	}

	void fill_unused_rgb_input(cudaStream_t stream, T* ptr, uint32_t batch_size) {
		uint32_t fill_zero_offset = (m_dir_encoding->padded_output_width() + 2) * batch_size;
		uint32_t fill_zero_size = (m_uv_network->padded_output_width() - 2) * batch_size;
		CUDA_CHECK_THROW(cudaMemsetAsync(ptr + fill_zero_offset, 0, fill_zero_size * sizeof(T), stream));
	}

	void set_params_impl(T* params, T* inference_params, T* gradients) override {
		size_t offset = 0;
		m_density_network->set_params(params + offset, inference_params + offset, gradients + offset);
		offset += m_density_network->n_params();

		m_uv_network->set_params(params + offset, inference_params + offset, gradients + offset);
		offset += m_uv_network->n_params();

		m_rgb_network->set_params(params + offset, inference_params + offset, gradients + offset);
		offset += m_rgb_network->n_params();

		m_pos_encoding->set_params(params + offset, inference_params + offset, gradients + offset);
		offset += m_pos_encoding->n_params();

		//m_uv_encoding->set_params(params + offset, inference_params + offset, gradients + offset);
		//offset += m_uv_encoding->n_params();

		m_dir_encoding->set_params(params + offset, inference_params + offset, gradients + offset);
		offset += m_dir_encoding->n_params();
	}

	void initialize_params(tcnn::pcg32& rnd, float* params_full_precision, float scale = 1) override {
		m_density_network->initialize_params(rnd, params_full_precision, scale);
		params_full_precision += m_density_network->n_params();

		m_uv_network->initialize_params(rnd, params_full_precision, scale);
		params_full_precision += m_uv_network->n_params();

		m_rgb_network->initialize_params(rnd, params_full_precision, scale);
		params_full_precision += m_rgb_network->n_params();

		m_pos_encoding->initialize_params(rnd, params_full_precision, scale);
		params_full_precision += m_pos_encoding->n_params();

		//m_uv_encoding->initialize_params(rnd, params_full_precision, scale);
		//params_full_precision += m_uv_encoding->n_params();

		m_dir_encoding->initialize_params(rnd, params_full_precision, scale);
		params_full_precision += m_dir_encoding->n_params();
	}

	size_t n_params() const override {
		return m_pos_encoding->n_params()
			+ m_density_network->n_params()
			//+ m_uv_encoding->n_params()
			+ m_uv_network->n_params()
			+ m_dir_encoding->n_params()
			+ m_rgb_network->n_params();
	}

	uint32_t padded_output_width() const override {
		return std::max(m_rgb_network->padded_output_width(), (uint32_t)4);
	}

	uint32_t input_width() const override {
		return m_dir_offset + m_n_dir_dims + m_n_extra_dims;
	}

	uint32_t output_width() const override {
		return 4;
	}

	uint32_t n_extra_dims() const {
		return m_n_extra_dims;
	}

	uint32_t required_input_alignment() const override {
		return 1; // No alignment required due to encoding
	}

	std::vector<std::pair<uint32_t, uint32_t>> layer_sizes() const override {
		auto layers = m_density_network->layer_sizes();
		auto uv_layers = m_uv_network->layer_sizes();
		auto rgb_layers = m_rgb_network->layer_sizes();
		layers.insert(layers.end(), uv_layers.begin(), uv_layers.end());
		layers.insert(layers.end(), rgb_layers.begin(), rgb_layers.end());
		return layers;
	}

	uint32_t width(uint32_t layer) const override {
		if (layer == 0) {
			return m_pos_encoding->padded_output_width();
		} else if (layer < m_density_network->num_forward_activations() + 1) {
			return m_density_network->width(layer - 1);
		} else if (layer < m_density_network->num_forward_activations() + m_uv_network->num_forward_activations() + 1) {
			return m_uv_network->width(layer - m_density_network->num_forward_activations() - 1);
		} else if (layer == m_density_network->num_forward_activations() + m_uv_network->num_forward_activations() + 1) {
			return m_rgb_network_input_width;
		} else {
			return m_rgb_network->width(layer - m_density_network->num_forward_activations() - m_uv_network->num_forward_activations() - 2);
		}
	}

	uint32_t num_forward_activations() const override {
		return m_density_network->num_forward_activations()
			+ m_uv_network->num_forward_activations()
			+ m_rgb_network->num_forward_activations()
			+ 2;
	}

	std::pair<const T*, tcnn::MatrixLayout> forward_activations(const tcnn::Context& ctx, uint32_t layer) const override {
		const auto& forward = dynamic_cast<const ForwardContext&>(ctx);
		if (layer == 0) {
			return {forward.density_network_input.data(), m_pos_encoding->preferred_output_layout()};
		} else if (layer < m_density_network->num_forward_activations() + 1) {
			return m_density_network->forward_activations(*forward.density_network_ctx, layer - 1);
		} else if (layer < m_density_network->num_forward_activations() + m_uv_network->num_forward_activations() + 1) {
			return m_uv_network->forward_activations(*forward.uv_network_ctx, layer - m_density_network->num_forward_activations() - 1);
		} else if (layer == m_density_network->num_forward_activations() + m_uv_network->num_forward_activations() + 1) {
			return {forward.rgb_network_input.data(), m_dir_encoding->preferred_output_layout()};
		} else {
			return m_rgb_network->forward_activations(*forward.rgb_network_ctx, layer - 2 - m_density_network->num_forward_activations() - m_uv_network->num_forward_activations());
		}
	}

	const std::shared_ptr<tcnn::Encoding<T>>& pos_encoding() const {
		return m_pos_encoding;
	}

	const std::shared_ptr<tcnn::Encoding<T>>& dir_encoding() const {
		return m_dir_encoding;
	}

	const std::shared_ptr<tcnn::Network<T>>& density_network() const {
		return m_density_network;
	}

	const std::shared_ptr<tcnn::Network<T>>& rgb_network() const {
		return m_rgb_network;
	}

	float uv_network_scale() const {
		return m_uv_network_scale;
	}

	void set_uv_network_scale(float scale) {
		m_uv_network_scale = scale;
	}

	tcnn::json hyperparams() const override {
		json density_network_hyperparams = m_density_network->hyperparams();
		density_network_hyperparams["n_output_dims"] = m_density_network->padded_output_width();
		return {
			{"otype", "NerfNetwork"},
			{"pos_encoding", m_pos_encoding->hyperparams()},
			//{"uv_encoding", m_uv_encoding->hyperparams()},
			{"dir_encoding", m_dir_encoding->hyperparams()},
			{"density_network", density_network_hyperparams},
			{"uv_network", m_uv_network->hyperparams()},
			{"rgb_network", m_rgb_network->hyperparams()},
		};
	}

private:
	std::shared_ptr<tcnn::Network<T>> m_density_network;
	std::shared_ptr<tcnn::Network<T>> m_uv_network;
	std::shared_ptr<tcnn::Network<T>> m_rgb_network;
	std::shared_ptr<tcnn::Encoding<T>> m_pos_encoding;
	std::shared_ptr<tcnn::Encoding<T>> m_dir_encoding;
	//std::shared_ptr<tcnn::Encoding<T>> m_uv_encoding;

	uint32_t m_rgb_network_input_width;
	uint32_t m_n_pos_dims;
	uint32_t m_n_dir_dims;
	uint32_t m_n_extra_dims; // extra dimensions are assumed to be part of a compound encoding with dir_dims
	uint32_t m_dir_offset;

	float m_uv_network_scale;
	tcnn::GPUMatrixDynamic<T> m_uv_grid;

	// // Storage of forward pass data
	struct ForwardContext : public tcnn::Context {
		tcnn::GPUMatrixDynamic<T> density_network_input;
		tcnn::GPUMatrixDynamic<T> density_network_output;
		tcnn::GPUMatrixDynamic<T> rgb_network_input;
		tcnn::GPUMatrix<T> rgb_network_output;

		//tcnn::GPUMatrixDynamic<T> uv_network_input;
		tcnn::GPUMatrixDynamic<T> uv_network_output;
		tcnn::GPUMatrixDynamic<float> uv_encoding_input;
		tcnn::GPUMatrixDynamic<T> uv_encoding_output;

		std::unique_ptr<Context> pos_encoding_ctx;
		std::unique_ptr<Context> uv_encoding_ctx;
		std::unique_ptr<Context> dir_encoding_ctx;

		std::unique_ptr<Context> density_network_ctx;
		std::unique_ptr<Context> uv_network_ctx;
		std::unique_ptr<Context> rgb_network_ctx;
	};
};

NGP_NAMESPACE_END
