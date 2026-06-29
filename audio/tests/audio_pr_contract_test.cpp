/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "audio_resampler.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

bool near(float actual, float expected, float tolerance = 1e-5f) {
    return std::fabs(actual - expected) <= tolerance;
}

void verify_resampler_contract() {
    Resampler::Config same_rate_config;
    same_rate_config.input_sample_rate = 16000;
    same_rate_config.output_sample_rate = 16000;
    same_rate_config.channels = 1;
    same_rate_config.method = ResampleMethod::LINEAR_UPSAMPLE;
    Resampler same_rate(same_rate_config);
    const std::vector<float> samples = {0.0f, 0.25f, -0.5f, 1.0f};
    const auto copied = same_rate.process(samples);
    require(copied == samples, "same-rate resampling must return an exact copy");

    Resampler::Config up_config;
    up_config.input_sample_rate = 2;
    up_config.output_sample_rate = 4;
    up_config.channels = 1;
    up_config.method = ResampleMethod::LINEAR_UPSAMPLE;
    Resampler upsample(up_config);
    const auto up = upsample.process(std::vector<float>{0.0f, 1.0f});
    require(up.size() == 4, "2x upsample must produce four samples from two inputs");
    require(near(up[0], 0.0f), "upsample[0] must equal first input");
    require(near(up[1], 0.5f), "upsample[1] must be linear midpoint");
    require(near(up[2], 1.0f), "upsample[2] must reach second input");
    require(near(up[3], 1.0f), "upsample[3] must clamp to last input");
    require(upsample.isUpsampling(), "upsampler must report upsampling");
    require(near(static_cast<float>(upsample.getRatio()), 2.0f), "upsampler ratio must be 2");

    Resampler::Config down_config;
    down_config.input_sample_rate = 4;
    down_config.output_sample_rate = 2;
    down_config.channels = 1;
    down_config.method = ResampleMethod::LINEAR_DOWNSAMPLE;
    Resampler downsample(down_config);
    const auto down = downsample.process(std::vector<float>{0.0f, 1.0f, 2.0f, 3.0f});
    require(down.size() == 2, "2x downsample must produce two samples from four inputs");
    require(near(down[0], 0.0f), "downsample[0] must equal first input");
    require(near(down[1], 2.0f), "downsample[1] must sample the second source step");
    require(downsample.isDownsampling(), "downsampler must report downsampling");

    const auto estimated = Resampler::estimateOutputSize(100, 16000, 48000);
    require(estimated >= 300, "estimateOutputSize must cover expected converted samples");

    float c_output[8] = {};
    const float c_input[2] = {0.0f, 1.0f};
    const int c_count = resample_simple(c_input, 2, 2, 4, 1, RESAMPLE_METHOD_LINEAR, c_output, 8);
    require(c_count == 4, "C API simple resample must return generated sample count");
    require(near(c_output[1], 0.5f), "C API simple resample must use linear interpolation");
}

void verify_invalid_input_error_path() {
    Resampler::Config invalid_rate;
    invalid_rate.input_sample_rate = 0;
    invalid_rate.output_sample_rate = 16000;
    invalid_rate.channels = 1;
    Resampler bad_rate(invalid_rate);
    require(!bad_rate.initialize(), "zero input sample rate must fail initialization");

    Resampler::Config invalid_channels;
    invalid_channels.input_sample_rate = 16000;
    invalid_channels.output_sample_rate = 48000;
    invalid_channels.channels = 0;
    Resampler bad_channels(invalid_channels);
    require(!bad_channels.initialize(), "zero channel count must fail initialization");

    require(resampler_create(0, 16000, 1, RESAMPLE_METHOD_LINEAR) == nullptr,
            "C API create must reject invalid sample rate");
    require(resampler_process(nullptr, nullptr, 0, nullptr, 0) == -1,
            "C API process must reject null handle and buffers");

    ResamplerHandle* handle = resampler_create(2, 4, 1, RESAMPLE_METHOD_LINEAR);
    require(handle != nullptr, "C API create must accept valid linear config");
    const float input[2] = {0.0f, 1.0f};
    float too_small[2] = {};
    const int count = resampler_process(handle, input, 2, too_small, 2);
    require(count == -1, "C API process must reject undersized output buffer");
    resampler_destroy(handle);
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one test mode argument");
    const std::string mode = argv[1];

    if (mode == "--resampler-contract") {
        verify_resampler_contract();
    } else if (mode == "--invalid-input-error-path") {
        verify_invalid_input_error_path();
    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return 2;
    }

    std::cout << "PASS " << mode << std::endl;
    return 0;
}
