// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "common/types.h"

// sotc: SOTC_FRAME_HASH=<folder> (off by default). Per guest frame, a fingerprint of the final game
// image, so that two offline runs can tell whether a flag changed what is drawn. The GPU side
// (downscale, readback ring) is SotcFrameHashRing in vk_presenter.cpp; this file is the CPU side.
namespace Vulkan::SotcFrameHash {

/// Size of the downscaled image the hashes are computed on (16:9, RGBA8).
inline constexpr u32 kWidth = 256;
inline constexpr u32 kHeight = 144;

/// SOTC_FRAME_HASH, the folder for hashes.csv and the PNGs (empty when off). Read once.
const std::string& Folder();

/// True when SOTC_FRAME_HASH names a folder.
bool Enabled();

/// SOTC_FRAME_HASH_PNG_EVERY: write frame_NNNNNN.png every N frames (0, the default, never).
/// Read once.
u32 PngEvery();

/// Writes <folder>/hashes.csv ("frame,exact,phash" per frame, "frame,skip,skip" for a frame the GPU
/// ring could not take) and the PNGs, on its own thread. Push and Skip do not block on the disk.
class Sink {
public:
    Sink(std::filesystem::path folder, u32 png_every);
    ~Sink();

    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;

    /// pixels: kWidth * kHeight * 4 bytes, RGBA8, tightly packed.
    void Push(u64 frame, std::vector<u8>&& pixels);
    void Skip(u64 frame);

    friend void DrainActive(int timeout_ms); // sotc: reads the queue before the process exits

private:
    struct Item {
        u64 frame{};
        std::vector<u8> pixels;
        bool skipped{};
    };

    void Run();

    std::filesystem::path folder;
    u32 png_every;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<Item> queue;
    bool stopping{};
    std::ofstream csv;
    std::jthread thread;
};

/// Exact 64-bit hash (XXH3) of the pixels, and the 8x8 average hash of their luma (bit i set when
/// cell i is at least the mean of the 8x8 cell means). Both are pure functions of the pixels.
/// Waits (at most timeout_ms) until the active sink wrote every frame queued so far (call before exit).
void DrainActive(int timeout_ms);
u64 ExactHash(const u8* rgba);
u64 AverageHash(const u8* rgba);

} // namespace Vulkan::SotcFrameHash
