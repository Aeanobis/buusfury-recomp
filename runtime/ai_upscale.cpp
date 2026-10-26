// "HD" picture mode: the frame is redrawn by a neural network (Real-ESRGAN's
// anime model, run on the GPU through ncnn/Vulkan).
//
// The network takes about 47 ms for a whole frame on an RTX 3070, three times
// too slow for 60 fps. So the frame is cut into 32x32 tiles and each tile's
// result is cached by its exact pixels (plus an 8 pixel border of context).
// The tile grid moves with the background's scroll position, so while the map
// scrolls the same tiles keep turning up and only what actually changed has to
// go through the network. That happens on a worker thread, several tiles per
// run because each run has a fixed overhead of about 4 ms.
//
// Until a tile's result arrives, each of its pixels is taken from the previous
// HD frame if that pixel has not changed (allowing for scrolling), and from the
// ordinary upscaler otherwise.
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu.h"
#include "net.h"

namespace {

const int W = 240, H = 160, SCALE = 4;
const int TILE = 32, PAD = 6, IN = TILE + 2 * PAD, OUT = TILE * SCALE;
const int BATCH = 5; // tiles per network run
const size_t CACHE_TILES = 6000; // about 64 KB each

struct Request {
	uint64_t key;
	std::vector<uint32_t> pixels; // IN * IN, 0x00BBGGRR
};

struct Entry {
	std::vector<uint32_t> pixels; // OUT * OUT, 0x00RRGGBB
	uint64_t lastUsed;
};

ncnn::Net* net;
std::thread worker;
std::mutex lock;
std::condition_variable wake;
std::deque<Request> queue;
std::unordered_set<uint64_t> inFlight;
std::unordered_map<uint64_t, Entry> cache;
uint64_t frameNumber;
bool quit, failed;
unsigned statHits, statMisses, statInferred;
uint64_t statHdPixels, statPixels;

uint64_t hashPixels(const uint32_t* p, int n) {
	uint64_t h = 0x9E3779B97F4A7C15ULL;
	for (int i = 0; i < n; ++i) {
		h ^= p[i] & 0xFFFFFF;
		h *= 0xFF51AFD7ED558CCDULL;
		h ^= h >> 29;
	}
	return h;
}

// Runs the network on several tiles at once, laid side by side
void infer(const std::vector<Request>& batch, std::vector<Entry>& results) {
	int n = (int) batch.size();
	ncnn::Mat in(IN * n, IN, 3);
	float* r = in.channel(0);
	float* g = in.channel(1);
	float* b = in.channel(2);
	for (int t = 0; t < n; ++t) {
		for (int y = 0; y < IN; ++y) {
			for (int x = 0; x < IN; ++x) {
				uint32_t c = batch[t].pixels[y * IN + x];
				int i = y * IN * n + t * IN + x;
				r[i] = (c & 0xFF) / 255.0f;
				g[i] = ((c >> 8) & 0xFF) / 255.0f;
				b[i] = ((c >> 16) & 0xFF) / 255.0f;
			}
		}
	}
	ncnn::Extractor ex = net->create_extractor();
	ex.input("data", in);
	ncnn::Mat res;
	ex.extract("output", res);
	results.resize(n);
	if (res.w != IN * n * SCALE || res.h != IN * SCALE || res.c != 3) {
		failed = true;
		return;
	}
	const float* rr = res.channel(0);
	const float* rg = res.channel(1);
	const float* rb = res.channel(2);
	for (int t = 0; t < n; ++t) {
		results[t].pixels.resize(OUT * OUT);
		for (int y = 0; y < OUT; ++y) {
			for (int x = 0; x < OUT; ++x) {
				int i = (y + PAD * SCALE) * res.w + (t * IN + PAD) * SCALE + x;
				int R = (int) (std::min(std::max(rr[i], 0.0f), 1.0f) * 255.0f + 0.5f);
				int G = (int) (std::min(std::max(rg[i], 0.0f), 1.0f) * 255.0f + 0.5f);
				int B = (int) (std::min(std::max(rb[i], 0.0f), 1.0f) * 255.0f + 0.5f);
				results[t].pixels[y * OUT + x] = (R << 16) | (G << 8) | B;
			}
		}
	}
}

void run() {
	for (;;) {
		std::vector<Request> batch;
		{
			std::unique_lock<std::mutex> l(lock);
			wake.wait(l, [] { return quit || !queue.empty(); });
			if (quit) {
				return;
			}
			while (!queue.empty() && (int) batch.size() < BATCH) {
				batch.push_back(std::move(queue.front()));
				queue.pop_front();
			}
		}
		std::vector<Entry> results;
		infer(batch, results);
		std::lock_guard<std::mutex> l(lock);
		for (size_t t = 0; t < batch.size(); ++t) {
			inFlight.erase(batch[t].key);
			if (!failed) {
				results[t].lastUsed = frameNumber;
				cache[batch[t].key] = std::move(results[t]);
				++statInferred;
			}
		}
		if (cache.size() > CACHE_TILES) { // drop the least recently shown quarter
			std::vector<std::pair<uint64_t, uint64_t>> ages;
			ages.reserve(cache.size());
			for (auto& kv : cache) {
				ages.emplace_back(kv.second.lastUsed, kv.first);
			}
			std::nth_element(ages.begin(), ages.begin() + ages.size() / 4, ages.end());
			for (size_t i = 0; i < ages.size() / 4; ++i) {
				cache.erase(ages[i].second);
			}
		}
	}
}

} // namespace

extern "C" int rcAiInit(const char* param, const char* bin) {
	if (net) {
		return 0;
	}
	if (ncnn::create_gpu_instance() || ncnn::get_gpu_count() < 1) {
		fprintf(stderr, "HD mode: no Vulkan GPU available\n");
		return -1;
	}
	net = new ncnn::Net;
	net->opt.use_vulkan_compute = true;
	net->opt.use_fp16_packed = true;
	net->opt.use_fp16_storage = true;
	net->opt.use_fp16_arithmetic = false;
	net->opt.num_threads = 2;
	if (net->load_param(param) || net->load_model(bin)) {
		fprintf(stderr, "HD mode: cannot load the model from %s\n", param);
		delete net;
		net = nullptr;
		return -1;
	}
	fprintf(stderr, "HD mode: using %s\n", ncnn::get_gpu_info(ncnn::get_default_gpu_index()).device_name());
	quit = false;
	worker = std::thread(run);
	return 0;
}

extern "C" void rcAiShutdown(void) {
	if (!net) {
		return;
	}
	{
		std::lock_guard<std::mutex> l(lock);
		quit = true;
	}
	wake.notify_all();
	worker.join();
	delete net;
	net = nullptr;
	ncnn::destroy_gpu_instance();
}

// Tiles still waiting for the network
extern "C" int rcAiPending(void) {
	std::lock_guard<std::mutex> l(lock);
	return (int) (queue.size() + inFlight.size());
}

// Percentage of pixels shown in HD since the last call
extern "C" int rcAiCoverage(void) {
	std::lock_guard<std::mutex> l(lock);
	int pct = statPixels ? (int) (statHdPixels * 100 / statPixels) : 0;
	statHdPixels = statPixels = 0;
	return pct;
}

extern "C" void rcAiStats(unsigned* hits, unsigned* misses, unsigned* inferred, unsigned* cached) {
	std::lock_guard<std::mutex> l(lock);
	*hits = statHits;
	*misses = statMisses;
	*inferred = statInferred;
	*cached = (unsigned) cache.size();
	statHits = statMisses = statInferred = 0;
}

// frame: W x H game picture, 0x00BBGGRR. fallback and out: 4x size, 0x00RRGGBB.
// (scrollX, scrollY): background scroll, used to line the tile grid up with the map.
extern "C" void rcAiCompose(const uint32_t* frame, const uint32_t* fallback, uint32_t* out, int scrollX, int scrollY) {
	// What was on screen last time, for reusing unchanged pixels
	static std::vector<uint32_t> prevFrame(W * H), prevOut(W * SCALE * H * SCALE), in(IN * IN);
	static std::vector<uint8_t> prevHd(W * H), hd(W * H); // which pixels were shown in HD
	static int prevScrollX, prevScrollY;
	static bool havePrev;
	int dx = ((scrollX - prevScrollX + 256 + 512) % 512) - 256; // how far the map moved
	int dy = ((scrollY - prevScrollY + 256 + 512) % 512) - 256;
	int ox = -(((scrollX % TILE) + TILE) % TILE);
	int oy = -(((scrollY % TILE) + TILE) % TILE);
	struct Wanted {
		Request rq;
		int distance;
	};
	std::vector<Wanted> wanted;
	std::unique_lock<std::mutex> l(lock);
	++frameNumber;
	for (int ty = oy; ty < H; ty += TILE) {
		for (int tx = ox; tx < W; tx += TILE) {
			// Past the screen edge the picture is mirrored. Repeating the edge
			// pixel instead makes streaks that the network then draws as streaks.
			for (int y = 0; y < IN; ++y) {
				int sy = ty - PAD + y;
				sy = sy < 0 ? -sy - 1 : sy >= H ? 2 * H - 1 - sy : sy;
				for (int x = 0; x < IN; ++x) {
					int sx = tx - PAD + x;
					sx = sx < 0 ? -sx - 1 : sx >= W ? 2 * W - 1 - sx : sx;
					in[y * IN + x] = frame[sy * W + sx];
				}
			}
			uint64_t key = hashPixels(in.data(), IN * IN);
			auto it = cache.find(key);
			int px0 = std::max(tx, 0), px1 = std::min(tx + TILE, W);
			int py0 = std::max(ty, 0), py1 = std::min(ty + TILE, H);
			if (it != cache.end()) {
				it->second.lastUsed = frameNumber;
				const uint32_t* src = it->second.pixels.data();
				++statHits;
				for (int y = py0 * SCALE; y < py1 * SCALE; ++y) {
					memcpy(out + y * W * SCALE + px0 * SCALE, src + (y - ty * SCALE) * OUT + (px0 - tx) * SCALE, (px1 - px0) * SCALE * sizeof(uint32_t));
				}
				for (int y = py0; y < py1; ++y) {
					memset(&hd[y * W + px0], 1, px1 - px0);
				}
				continue;
			}
			++statMisses;
			if (!inFlight.count(key)) {
				int cx = tx + TILE / 2 - W / 2, cy = ty + TILE / 2 - H / 2;
				wanted.push_back({ { key, in }, cx * cx + cy * cy });
			}
			// Not computed yet: per pixel, keep last frame's HD result where the
			// picture has not changed, otherwise use the ordinary upscaler
			for (int y = py0; y < py1; ++y) {
				for (int x = px0; x < px1; ++x) {
					int sx = x + dx, sy = y + dy;
					// "Unchanged" means the pixel and its four neighbours all match,
					// so that flat colours do not match by coincidence
					bool reuse = havePrev && sx >= 1 && sx < W - 1 && sy >= 1 && sy < H - 1 && x >= 1 && x < W - 1 && y >= 1 && y < H - 1 &&
					             prevHd[sy * W + sx] &&
					             prevFrame[sy * W + sx] == frame[y * W + x] &&
					             prevFrame[sy * W + sx - 1] == frame[y * W + x - 1] &&
					             prevFrame[sy * W + sx + 1] == frame[y * W + x + 1] &&
					             prevFrame[(sy - 1) * W + sx] == frame[(y - 1) * W + x] &&
					             prevFrame[(sy + 1) * W + sx] == frame[(y + 1) * W + x];
					hd[y * W + x] = reuse;
					for (int k = 0; k < SCALE; ++k) {
						const uint32_t* from = reuse ? &prevOut[(sy * SCALE + k) * W * SCALE + sx * SCALE]
						                             : fallback + (y * SCALE + k) * W * SCALE + x * SCALE;
						memcpy(out + (y * SCALE + k) * W * SCALE + x * SCALE, from, SCALE * sizeof(uint32_t));
					}
				}
			}
		}
	}
	// Only what is on screen now is worth computing: forget older requests.
	// The middle of the screen, where the player is, goes first.
	for (auto& rq : queue) {
		inFlight.erase(rq.key);
	}
	queue.clear();
	std::sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b) { return a.distance < b.distance; });
	for (auto& w : wanted) {
		inFlight.insert(w.rq.key);
		queue.push_back(std::move(w.rq));
	}
	l.unlock();
	wake.notify_one();
	memcpy(prevFrame.data(), frame, W * H * sizeof(uint32_t));
	memcpy(prevOut.data(), out, W * SCALE * H * SCALE * sizeof(uint32_t));
	{
		unsigned n = 0;
		for (int i = 0; i < W * H; ++i) {
			n += hd[i];
		}
		std::lock_guard<std::mutex> l2(lock);
		statHdPixels += n;
		statPixels += W * H;
	}
	prevHd.swap(hd);
	prevScrollX = scrollX;
	prevScrollY = scrollY;
	havePrev = true;
}

