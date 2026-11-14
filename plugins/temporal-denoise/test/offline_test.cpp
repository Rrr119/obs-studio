/*
 * Offline test for the temporal denoise filter algorithm.
 *
 * Mirrors the GPU shader (temporal-denoise.effect) on the CPU, operating
 * directly on YUV420P (I420) frames - the same luma/chroma weighting the
 * shader applies after decoding NV12/YUV420P sources.
 *
 * Pipeline:
 *   1. generate a synthetic clean sequence (static textured background +
 *      moving box, with a hard scene cut in the middle)
 *   2. add Gaussian noise -> "noisy" sequence
 *   3. run the temporal denoiser -> "denoised" sequence
 *   4. report PSNR (noisy vs clean, denoised vs clean), static-region
 *      noise variance before/after, and post-scene-cut PSNR (ghosting)
 *
 * Exit code is 0 when denoising actually improves the metrics.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

struct Params {
	int width = 320;
	int height = 240;
	int frames = 90;
	double noise_sigma = 12.0;
	double strength = 0.85;
	double motion_threshold = 0.04;
	double detail = 0.5;
	double scene_reset_threshold = 0.12;
	std::string write_prefix; // if non-empty, dump yuv files
};

struct Frame {
	int w, h;
	std::vector<uint8_t> y, u, v; // y: w*h, u/v: (w/2)*(h/2)
	Frame(int w_, int h_) : w(w_), h(h_), y(w_ * h_), u(w_ / 2 * h_ / 2), v(w_ / 2 * h_ / 2) {}
};

static double smoothstep(double e0, double e1, double x)
{
	double t = std::min(std::max((x - e0) / (e1 - e0), 0.0), 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// ---------------------------------------------------------------------------
// Synthetic sequence
// ---------------------------------------------------------------------------

static void generate_clean_frame(Frame &f, int index, int total, uint32_t texture_seed)
{
	const int scene = (index < total / 2) ? 0 : 1;
	std::mt19937 tex_rng(texture_seed + scene * 7919u);

	// Static background: gradient + fixed per-pixel texture (detail).
	std::vector<uint8_t> texture(f.w * f.h);
	for (auto &p : texture)
		p = static_cast<uint8_t>(tex_rng() & 0xFF);

	for (int yy = 0; yy < f.h; yy++) {
		for (int xx = 0; xx < f.w; xx++) {
			double grad = scene == 0 ? 60.0 + 80.0 * yy / f.h
						 : 40.0 + 100.0 * xx / f.w;
			double tex = (texture[yy * f.w + xx] - 128) * 0.15;
			f.y[yy * f.w + xx] = static_cast<uint8_t>(
				std::min(std::max(grad + tex, 0.0), 255.0));
		}
	}
	for (size_t i = 0; i < f.u.size(); i++) {
		f.u[i] = static_cast<uint8_t>(scene == 0 ? 110 : 140);
		f.v[i] = static_cast<uint8_t>(scene == 0 ? 150 : 100);
	}

	// Moving bright box, confined to the bottom half so the top-left
	// quarter stays static for the noise-variance measurement.
	const int bw = f.w / 8, bh = f.h / 8;
	const int span = f.w - bw;
	int bx = 20 + (index * 3) % (2 * span);
	if (bx > span)
		bx = 2 * span - bx;
	const int by = f.h / 2 + 10;
	for (int yy = by; yy < by + bh && yy < f.h; yy++)
		for (int xx = bx; xx < bx + bw && xx < f.w; xx++)
			f.y[yy * f.w + xx] = 220;
	for (int yy = by / 2; yy < (by + bh) / 2 && yy < f.h / 2; yy++)
		for (int xx = bx / 2; xx < (bx + bw) / 2 && xx < f.w / 2; xx++) {
			f.u[yy * (f.w / 2) + xx] = 90;
			f.v[yy * (f.w / 2) + xx] = 200;
		}
}

static void add_noise(Frame &f, double sigma, std::mt19937 &rng)
{
	std::normal_distribution<double> gauss(0.0, sigma);
	auto noisify = [&](std::vector<uint8_t> &plane) {
		for (auto &p : plane) {
			double v = p + gauss(rng);
			p = static_cast<uint8_t>(std::min(std::max(v, 0.0), 255.0));
		}
	};
	noisify(f.y);
	noisify(f.u);
	noisify(f.v);
}

// ---------------------------------------------------------------------------
// Temporal denoiser (CPU mirror of temporal-denoise.effect)
// ---------------------------------------------------------------------------

class TemporalDenoiser {
	Params p;
	int w, h, cw, ch;
	std::vector<double> hist_y, hist_u, hist_v;
	bool history_valid = false;

public:
	TemporalDenoiser(const Params &params)
		: p(params), w(params.width), h(params.height), cw(params.width / 2),
		  ch(params.height / 2), hist_y(w * h), hist_u(cw * ch), hist_v(cw * ch)
	{
	}

	void reset()
	{
		history_valid = false;
	}

	void process(Frame &f)
	{
		// Scene-cut statistic: average |delta luma| on a 16x9 grid.
		double scene_stat = 0.0;
		if (history_valid) {
			for (int gy = 0; gy < 9; gy++)
				for (int gx = 0; gx < 16; gx++) {
					int sx = (gx * 2 + 1) * w / 32;
					int sy = (gy * 2 + 1) * h / 18;
					scene_stat += std::fabs(f.y[sy * w + sx] / 255.0 -
								hist_y[sy * w + sx]);
				}
			scene_stat /= 144.0;
		}
		const double keep_history =
			(!history_valid || scene_stat > p.scene_reset_threshold) ? 0.0 : 1.0;

		auto luma_at = [&](int x, int y) {
			x = std::min(std::max(x, 0), w - 1);
			y = std::min(std::max(y, 0), h - 1);
			return f.y[y * w + x] / 255.0;
		};
		auto hist_at = [&](int x, int y) {
			x = std::min(std::max(x, 0), w - 1);
			y = std::min(std::max(y, 0), h - 1);
			return hist_y[y * w + x];
		};

		std::vector<double> out_y(w * h);
		for (int yy = 0; yy < h; yy++) {
			for (int xx = 0; xx < w; xx++) {
				const int i = yy * w + xx;
				const double cur = f.y[i] / 255.0;

				// 3x3-averaged luma: noise-robust motion estimate.
				double cur_avg = 0.0, hist_avg = 0.0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						cur_avg += luma_at(xx + dx, yy + dy);
						hist_avg += hist_at(xx + dx, yy + dy);
					}
				cur_avg /= 9.0;
				hist_avg /= 9.0;

				const double diff = std::fabs(cur_avg - hist_avg);
				const double motion =
					smoothstep(p.motion_threshold * 0.5,
						   p.motion_threshold * 1.5, diff);

				// High-frequency energy = center vs. local average.
				const double edge = std::fabs(cur - cur_avg);
				const double detail_keep =
					1.0 - p.detail * std::min(edge * 4.0, 1.0);

				const double weight = p.strength * (1.0 - motion) *
						      detail_keep * keep_history;

				out_y[i] = cur + (hist_y[i] - cur) * weight;
			}
		}

		// Chroma: same motion weight (from the co-located luma), x1.25.
		for (int plane = 0; plane < 2; plane++) {
			std::vector<uint8_t> &src = plane == 0 ? f.u : f.v;
			std::vector<double> &hist = plane == 0 ? hist_u : hist_v;
			std::vector<double> out(cw * ch);
			for (int yy = 0; yy < ch; yy++) {
				for (int xx = 0; xx < cw; xx++) {
					const int i = yy * cw + xx;
					const double cur = src[i] / 255.0;
					const double ly = out_y[(yy * 2) * w + xx * 2];
					const double lhist = hist_y[(yy * 2) * w + xx * 2];
					const double diff = std::fabs(ly - lhist);
					const double motion =
						smoothstep(p.motion_threshold * 0.5,
							   p.motion_threshold * 1.5, diff);
					const double weight =
						std::min(p.strength * (1.0 - motion) *
								 keep_history * 1.25,
							 1.0);
					out[i] = cur + (hist[i] - cur) * weight;
				}
			}
			for (int i = 0; i < cw * ch; i++) {
				src[i] = static_cast<uint8_t>(std::min(
					std::max(out[i] * 255.0, 0.0), 255.0));
				hist[i] = out[i];
			}
		}

		for (int i = 0; i < w * h; i++) {
			f.y[i] = static_cast<uint8_t>(
				std::min(std::max(out_y[i] * 255.0, 0.0), 255.0));
			hist_y[i] = out_y[i];
		}
		history_valid = true;
	}
};

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static double mse_y(const Frame &a, const Frame &b)
{
	double sum = 0.0;
	for (size_t i = 0; i < a.y.size(); i++) {
		double d = (double)a.y[i] - (double)b.y[i];
		sum += d * d;
	}
	return sum / a.y.size();
}

static double psnr_from_mse(double mse)
{
	if (mse <= 1e-9)
		return 99.0;
	return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Mean squared error restricted to the static top-left quarter.
static double static_region_noise_var(const Frame &a, const Frame &b)
{
	double sum = 0.0;
	int count = 0;
	for (int yy = 0; yy < a.h / 2; yy++)
		for (int xx = 0; xx < a.w / 2; xx++) {
			double d = (double)a.y[yy * a.w + xx] - (double)b.y[yy * a.w + xx];
			sum += d * d;
			count++;
		}
	return sum / count;
}

static void write_yuv(const std::string &path, const std::vector<Frame> &frames)
{
	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp)
		return;
	for (const auto &f : frames) {
		fwrite(f.y.data(), 1, f.y.size(), fp);
		fwrite(f.u.data(), 1, f.u.size(), fp);
		fwrite(f.v.data(), 1, f.v.size(), fp);
	}
	fclose(fp);
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
	Params p;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		auto next = [&]() { return argv[++i]; };
		if (arg == "--width")
			p.width = atoi(next());
		else if (arg == "--height")
			p.height = atoi(next());
		else if (arg == "--frames")
			p.frames = atoi(next());
		else if (arg == "--noise")
			p.noise_sigma = atof(next());
		else if (arg == "--strength")
			p.strength = atof(next());
		else if (arg == "--motion-threshold")
			p.motion_threshold = atof(next());
		else if (arg == "--detail")
			p.detail = atof(next());
		else if (arg == "--write")
			p.write_prefix = next();
		else {
			fprintf(stderr,
				"usage: %s [--width W] [--height H] [--frames N] [--noise S]\n"
				"          [--strength S] [--motion-threshold T] [--detail D]\n"
				"          [--write PREFIX]\n",
				argv[0]);
			return 2;
		}
	}
	if (p.width % 2 || p.height % 2) {
		fprintf(stderr, "width/height must be even (YUV420P)\n");
		return 2;
	}

	const int n = p.frames;
	std::vector<Frame> clean, noisy, denoised;
	clean.reserve(n);
	noisy.reserve(n);
	denoised.reserve(n);

	std::mt19937 rng(1234);
	for (int i = 0; i < n; i++) {
		clean.emplace_back(p.width, p.height);
		generate_clean_frame(clean.back(), i, n, 42);
		noisy.push_back(clean.back());
		add_noise(noisy.back(), p.noise_sigma, rng);
		denoised.push_back(noisy.back());
	}

	TemporalDenoiser denoiser(p);
	double psnr_noisy_sum = 0.0, psnr_denoised_sum = 0.0;
	double var_noisy_sum = 0.0, var_denoised_sum = 0.0;
	int var_count = 0;
	const int cut = n / 2;

	printf("%4s  %8s  %8s  %8s\n", "frm", "psnr_in", "psnr_out", "static");
	for (int i = 0; i < n; i++) {
		denoiser.process(denoised[i]);

		const double mse_in = mse_y(noisy[i], clean[i]);
		const double mse_out = mse_y(denoised[i], clean[i]);
		const double psnr_in = psnr_from_mse(mse_in);
		const double psnr_out = psnr_from_mse(mse_out);
		psnr_noisy_sum += psnr_in;
		psnr_denoised_sum += psnr_out;

		// Skip warm-up and the frames right after the scene cut for
		// the steady-state static variance measurement.
		if (i > 5 && i != cut && i != cut + 1) {
			var_noisy_sum += static_region_noise_var(noisy[i], clean[i]);
			var_denoised_sum += static_region_noise_var(denoised[i], clean[i]);
			var_count++;
		}

		if (i < 3 || i == cut - 1 || i == cut || i == cut + 1 || i == cut + 2 ||
		    i == n - 1)
			printf("%4d  %8.2f  %8.2f  %8.1f\n", i, psnr_in, psnr_out,
			       static_region_noise_var(denoised[i], clean[i]));
	}

	const double avg_in = psnr_noisy_sum / n;
	const double avg_out = psnr_denoised_sum / n;
	const double var_in = var_noisy_sum / var_count;
	const double var_out = var_denoised_sum / var_count;

	printf("\n=== summary ===\n");
	printf("frames:                %d (%dx%d, scene cut at %d)\n", n, p.width, p.height,
	       cut);
	printf("noise sigma:           %.1f\n", p.noise_sigma);
	printf("avg PSNR in (noisy):   %.2f dB\n", avg_in);
	printf("avg PSNR out (denoise):%.2f dB  (%+.2f dB)\n", avg_out, avg_out - avg_in);
	printf("static noise var in:   %.1f\n", var_in);
	printf("static noise var out:  %.1f  (%.1f%% reduction)\n", var_out,
	       100.0 * (1.0 - var_out / var_in));

	// Ghosting check: PSNR right after the scene cut must not collapse.
	double psnr_after_cut = psnr_from_mse(mse_y(denoised[cut], clean[cut]));
	printf("PSNR at scene cut:     %.2f dB (input %.2f dB)\n", psnr_after_cut,
	       psnr_from_mse(mse_y(noisy[cut], clean[cut])));

	if (!p.write_prefix.empty()) {
		write_yuv(p.write_prefix + "_clean.yuv", clean);
		write_yuv(p.write_prefix + "_noisy.yuv", noisy);
		write_yuv(p.write_prefix + "_denoised.yuv", denoised);
		printf("wrote %s_{clean,noisy,denoised}.yuv (YUV420P)\n",
		       p.write_prefix.c_str());
	}

	const bool ok = avg_out > avg_in && var_out < var_in * 0.5 &&
			psnr_after_cut > avg_in - 3.0;
	printf("result: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
