// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_FRAME_HASH, the CPU side. See sotc_frame_hash.h.

#include <thread>
#include "video_core/renderer_vulkan/sotc_frame_hash.h"

#include <array>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>
#include <stb_image_write.h>
#include <xxhash.h>

#include "common/logging/log.h"
#include "common/thread.h"

namespace Vulkan::SotcFrameHash {

namespace {

constexpr u64 kBytes = u64{kWidth} * kHeight * 4;

u32 ParsePngEvery() {
    const char* v = std::getenv("SOTC_FRAME_HASH_PNG_EVERY");
    if (!v || v[0] == '\0') {
        return 0;
    }
    return static_cast<u32>(std::strtoul(v, nullptr, 10));
}

void WriteToStream(void* context, void* data, int size) {
    static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size);
}

} // namespace

const std::string& Folder() {
    static const std::string folder = [] {
        const char* v = std::getenv("SOTC_FRAME_HASH");
        return v ? std::string(v) : std::string();
    }();
    return folder;
}

bool Enabled() {
    return !Folder().empty();
}

u32 PngEvery() {
    static const u32 every = ParsePngEvery();
    return every;
}

namespace {
std::atomic<Sink*> g_active{nullptr}; // the sink of the running presenter (DrainActive)
} // namespace

void DrainActive(int timeout_ms) {
    Sink* sink = g_active.load(std::memory_order_acquire);
    for (int waited = 0; sink && waited < timeout_ms; waited += 5) {
        {
            std::scoped_lock lk{sink->mutex};
            if (sink->queue.empty()) {
                waited = timeout_ms; // the item being written finishes within the sleep below
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Sink::Sink(std::filesystem::path folder_, u32 png_every_)
    : folder{std::move(folder_)}, png_every{png_every_} {
    g_active.store(this, std::memory_order_release);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    // One run per folder: the file is started over, so two runs into it do not interleave.
    csv.open(folder / "hashes.csv", std::ios::out | std::ios::trunc);
    if (!csv) {
        LOG_ERROR(Render_Vulkan, "SOTC_FRAME_HASH: cannot write {}", (folder / "hashes.csv").string());
    }
    csv << "frame,exact,phash\n";
    csv.flush();
    thread = std::jthread([this] { Run(); });
}

Sink::~Sink() {
    g_active.store(nullptr, std::memory_order_release);
    {
        std::scoped_lock lk{mutex};
        stopping = true;
    }
    cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

void Sink::Push(u64 frame, std::vector<u8>&& pixels) {
    {
        std::scoped_lock lk{mutex};
        queue.push(Item{frame, std::move(pixels), false});
    }
    cv.notify_one();
}

void Sink::Skip(u64 frame) {
    {
        std::scoped_lock lk{mutex};
        queue.push(Item{frame, {}, true});
    }
    cv.notify_one();
}

void Sink::Run() {
    Common::SetCurrentThreadName("shadPS4:SotcFrameHash");
    for (;;) {
        Item item;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, [this] { return stopping || !queue.empty(); });
            if (queue.empty()) {
                return; // stopping, and everything queued is written
            }
            item = std::move(queue.front());
            queue.pop();
        }
        if (item.skipped) {
            csv << fmt::format("{},skip,skip\n", item.frame);
            csv.flush();
            continue;
        }
        csv << fmt::format("{},{:016x},{:016x}\n", item.frame, ExactHash(item.pixels.data()),
                           AverageHash(item.pixels.data()));
        csv.flush();
        if (png_every != 0 && item.frame % png_every == 0) {
            std::ofstream png(folder / fmt::format("frame_{:06}.png", item.frame),
                              std::ios::binary | std::ios::trunc);
            stbi_write_png_to_func(&WriteToStream, &png, static_cast<int>(kWidth),
                                   static_cast<int>(kHeight), 4, item.pixels.data(),
                                   static_cast<int>(kWidth * 4));
        }
    }
}

u64 ExactHash(const u8* rgba) {
    return XXH3_64bits(rgba, kBytes);
}

u64 AverageHash(const u8* rgba) {
    constexpr u32 cell_w = kWidth / 8;
    constexpr u32 cell_h = kHeight / 8;
    constexpr u32 cell_pixels = cell_w * cell_h;
    std::array<u64, 64> sums{};
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const u8* p = rgba + (static_cast<u64>(y) * kWidth + x) * 4;
            // Integer BT.601 luma of R, G, B.
            const u32 luma = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
            sums[(y / cell_h) * 8 + x / cell_w] += luma;
        }
    }
    std::array<u64, 64> means{};
    u64 total = 0;
    for (size_t i = 0; i < means.size(); ++i) {
        means[i] = sums[i] / cell_pixels;
        total += means[i];
    }
    u64 hash = 0;
    for (size_t i = 0; i < means.size(); ++i) {
        // means[i] >= total / 64, in integers.
        if (means[i] * 64 >= total) {
            hash |= u64{1} << i;
        }
    }
    return hash;
}

} // namespace Vulkan::SotcFrameHash
