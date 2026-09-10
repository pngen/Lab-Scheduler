// Real accelerator work for the CUDA scheduling proof.

#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>

__global__ void vector_add_kernel(const float* left, const float* right, float* out, int count) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < count) {
        out[index] = left[index] + right[index];
    }
}

extern "C" int lab_scheduler_cuda_vector_add(int device, int count, float* host_out, char* message,
                                             int message_size) {
    if (count <= 0 || count > (1 << 22)) {
        std::snprintf(message, static_cast<std::size_t>(message_size), "invalid element count");
        return 1;
    }
    if (cudaSetDevice(device) != cudaSuccess) {
        std::snprintf(message, static_cast<std::size_t>(message_size), "cudaSetDevice failed");
        return 2;
    }

    const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(float);
    float* device_left = nullptr;
    float* device_right = nullptr;
    float* device_out = nullptr;
    if (cudaMalloc(&device_left, bytes) != cudaSuccess || cudaMalloc(&device_right, bytes) != cudaSuccess ||
        cudaMalloc(&device_out, bytes) != cudaSuccess) {
        std::snprintf(message, static_cast<std::size_t>(message_size), "cudaMalloc failed");
        return 3;
    }

    float* host_left = new float[static_cast<std::size_t>(count)];
    float* host_right = new float[static_cast<std::size_t>(count)];
    for (int index = 0; index < count; ++index) {
        host_left[index] = static_cast<float>(index) * 0.5f;
        host_right[index] = static_cast<float>(index) * 0.25f;
    }

    int result = 0;
    if (cudaMemcpy(device_left, host_left, bytes, cudaMemcpyHostToDevice) != cudaSuccess ||
        cudaMemcpy(device_right, host_right, bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
        std::snprintf(message, static_cast<std::size_t>(message_size), "cudaMemcpy host to device failed");
        result = 4;
    }
    if (result == 0) {
        const int threads = 256;
        const int blocks = (count + threads - 1) / threads;
        vector_add_kernel<<<blocks, threads>>>(device_left, device_right, device_out, count);
        if (cudaGetLastError() != cudaSuccess) {
            std::snprintf(message, static_cast<std::size_t>(message_size), "kernel launch failed");
            result = 5;
        }
    }
    if (result == 0) {
        if (cudaDeviceSynchronize() != cudaSuccess) {
            std::snprintf(message, static_cast<std::size_t>(message_size), "cudaDeviceSynchronize failed");
            result = 6;
        }
    }
    if (result == 0) {
        if (cudaMemcpy(host_out, device_out, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
            std::snprintf(message, static_cast<std::size_t>(message_size), "cudaMemcpy device to host failed");
            result = 7;
        }
    }
    if (result == 0) {
        for (int index = 0; index < count; ++index) {
            const float expected = host_left[index] + host_right[index];
            if (host_out[index] != expected) {
                std::snprintf(message, static_cast<std::size_t>(message_size),
                              "parity mismatch at element %d", index);
                result = 8;
                break;
            }
        }
    }

    delete[] host_left;
    delete[] host_right;
    cudaFree(device_left);
    cudaFree(device_right);
    cudaFree(device_out);
    if (result == 0) {
        std::snprintf(message, static_cast<std::size_t>(message_size), "vector add verified");
    }
    return result;
}

extern "C" int lab_scheduler_cuda_device_count() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess) {
        return 0;
    }
    return count;
}

extern "C" int lab_scheduler_cuda_device_info(int device, char* name, int name_size, unsigned long long* memory,
                                              int* compute_major, int* compute_minor, int* multiprocessors) {
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, device) != cudaSuccess) {
        return 1;
    }
    std::snprintf(name, static_cast<std::size_t>(name_size), "%s", properties.name);
    *memory = static_cast<unsigned long long>(properties.totalGlobalMem);
    *compute_major = properties.major;
    *compute_minor = properties.minor;
    *multiprocessors = properties.multiProcessorCount;
    return 0;
}
