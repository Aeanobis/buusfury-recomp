// Times AI upscaling models on a GBA-sized frame: ai_bench <model.param> <model.bin>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "net.h"
#include "gpu.h"

int main(int argc, char** argv) {
	ncnn::create_gpu_instance();
	printf("GPU: %s\n", ncnn::get_gpu_info(ncnn::get_default_gpu_index()).device_name());
	ncnn::Net net;
	net.opt.use_vulkan_compute = true;
	net.opt.use_fp16_packed = true;
	net.opt.use_fp16_storage = true;
	net.opt.use_fp16_arithmetic = argc > 5;
	if (net.load_param(argv[1]) || net.load_model(argv[2])) {
		return 1;
	}
	int side = argc > 3 ? atoi(argv[3]) : 0;
	int count = argc > 4 ? atoi(argv[4]) : 24;
	ncnn::Mat in(side ? side : 240, side ? side : 160, 3);
	in.fill(0.5f);
	auto all0 = std::chrono::steady_clock::now();
	for (int i = 0; i < count; ++i) {
		auto t0 = std::chrono::steady_clock::now();
		ncnn::Extractor ex = net.create_extractor();
		ex.input("data", in);
		ncnn::Mat out;
		ex.extract("output", out);
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (0) {
			printf("run %d: %.1f ms, output %dx%d\n", i, ms, out.w, out.h);
		}
	}
	printf("%dx%d fp16-arith %d: %.2f ms each\n", in.w, in.h, (int) net.opt.use_fp16_arithmetic, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - all0).count() / count);
	net.clear();
	ncnn::destroy_gpu_instance();
	return 0;
}
