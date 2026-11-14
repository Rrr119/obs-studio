/*
 * Temporal denoise video filter for OBS Studio.
 *
 * Motion-adaptive temporal (IIR) denoiser running entirely on the GPU:
 *   pass 1: render the filtered source into an input texture
 *   pass 2: render a 1x1 "scene stat" texture (average luma difference
 *           vs. the history frame) used for scene-cut detection
 *   pass 3: denoise pass blending current frame with the history
 *           (previously denoised) frame, written into the next history
 *           buffer (ping-pong, so no extra copy is needed)
 *   pass 4: draw the result
 *
 * No frame data is ever read back to the CPU.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#define SETTING_STRENGTH "strength"
#define SETTING_MOTION_THRESHOLD "motion_threshold"
#define SETTING_DETAIL "detail"

/* Average |delta luma| above this is treated as a scene cut. */
#define SCENE_RESET_THRESHOLD 0.12f

struct temporal_denoise_data {
	obs_source_t *context;

	gs_effect_t *effect;
	gs_eparam_t *param_image;
	gs_eparam_t *param_history;
	gs_eparam_t *param_scene_stat;
	gs_eparam_t *param_strength;
	gs_eparam_t *param_motion_threshold;
	gs_eparam_t *param_detail;
	gs_eparam_t *param_history_valid;
	gs_eparam_t *param_scene_reset_threshold;
	gs_eparam_t *param_texel_size;

	gs_texrender_t *input_render;
	gs_texrender_t *history[2]; /* ping-pong */
	gs_texrender_t *scene_stat; /* 1x1 */
	int read_index;
	bool history_valid;

	uint32_t width;
	uint32_t height;

	float strength;
	float motion_threshold;
	float detail;
};

static const char *temporal_denoise_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("TemporalDenoise");
}

static void temporal_denoise_update(void *data, obs_data_t *settings)
{
	struct temporal_denoise_data *filter = data;

	filter->strength = (float)obs_data_get_double(settings, SETTING_STRENGTH);
	filter->motion_threshold = (float)obs_data_get_double(settings, SETTING_MOTION_THRESHOLD);
	filter->detail = (float)obs_data_get_double(settings, SETTING_DETAIL);
}

static void temporal_denoise_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, SETTING_STRENGTH, 0.85);
	obs_data_set_default_double(settings, SETTING_MOTION_THRESHOLD, 0.04);
	obs_data_set_default_double(settings, SETTING_DETAIL, 0.5);
}

static obs_properties_t *temporal_denoise_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_float_slider(props, SETTING_STRENGTH, obs_module_text("Strength"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(props, SETTING_MOTION_THRESHOLD,
					obs_module_text("MotionThreshold"), 0.0, 0.5, 0.005);
	obs_properties_add_float_slider(props, SETTING_DETAIL, obs_module_text("DetailPreservation"),
					0.0, 1.0, 0.01);

	UNUSED_PARAMETER(data);
	return props;
}

static void *temporal_denoise_create(obs_data_t *settings, obs_source_t *context)
{
	struct temporal_denoise_data *filter = bzalloc(sizeof(struct temporal_denoise_data));
	char *effect_path = obs_module_file("temporal-denoise.effect");

	filter->context = context;

	obs_enter_graphics();

	filter->input_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->history[0] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->history[1] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	filter->scene_stat = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	if (filter->effect) {
		filter->param_image = gs_effect_get_param_by_name(filter->effect, "image");
		filter->param_history = gs_effect_get_param_by_name(filter->effect, "history");
		filter->param_scene_stat = gs_effect_get_param_by_name(filter->effect, "scene_stat");
		filter->param_strength = gs_effect_get_param_by_name(filter->effect, "strength");
		filter->param_motion_threshold =
			gs_effect_get_param_by_name(filter->effect, "motion_threshold");
		filter->param_detail = gs_effect_get_param_by_name(filter->effect, "detail_amount");
		filter->param_history_valid =
			gs_effect_get_param_by_name(filter->effect, "history_valid");
		filter->param_scene_reset_threshold =
			gs_effect_get_param_by_name(filter->effect, "scene_reset_threshold");
		filter->param_texel_size = gs_effect_get_param_by_name(filter->effect, "texel_size");
	}

	obs_leave_graphics();

	bfree(effect_path);

	if (!filter->effect) {
		blog(LOG_ERROR, "[temporal-denoise] failed to load temporal-denoise.effect");
		temporal_denoise_update(filter, settings);
		return filter;
	}

	temporal_denoise_update(filter, settings);
	return filter;
}

static void temporal_denoise_destroy(void *data)
{
	struct temporal_denoise_data *filter = data;

	obs_enter_graphics();

	gs_effect_destroy(filter->effect);
	gs_texrender_destroy(filter->input_render);
	gs_texrender_destroy(filter->history[0]);
	gs_texrender_destroy(filter->history[1]);
	gs_texrender_destroy(filter->scene_stat);

	obs_leave_graphics();

	bfree(filter);
}

/* Render `source` into `render` with a plain alpha-copy blend. */
static bool render_source_to_texture(obs_source_t *source, gs_texrender_t *render, uint32_t width,
				     uint32_t height)
{
	gs_texrender_reset(render);
	if (!gs_texrender_begin(render, width, height))
		return false;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_video_render(source);
	gs_blend_state_pop();

	gs_texrender_end(render);
	return true;
}

/* Run one effect technique rendering a full-screen sprite into `render`. */
static void run_effect_pass(struct temporal_denoise_data *filter, const char *technique,
			    gs_texrender_t *render, uint32_t width, uint32_t height)
{
	gs_technique_t *tech = gs_effect_get_technique(filter->effect, technique);

	gs_texrender_reset(render);
	if (!gs_texrender_begin(render, width, height))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(NULL, 0, width, height);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_blend_state_pop();

	gs_texrender_end(render);
}

static void temporal_denoise_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct temporal_denoise_data *filter = data;
	obs_source_t *target = obs_filter_get_target(filter->context);

	if (!filter->effect || !target) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const uint32_t width = obs_source_get_base_width(target);
	const uint32_t height = obs_source_get_base_height(target);
	if (width == 0 || height == 0) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	/* Resolution change: drop history to avoid smearing. */
	if (width != filter->width || height != filter->height) {
		filter->width = width;
		filter->height = height;
		filter->history_valid = false;
	}

	/* Pass 1: render the incoming frame into our own texture. */
	if (!render_source_to_texture(target, filter->input_render, width, height)) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_texture_t *input_tex = gs_texrender_get_texture(filter->input_render);
	if (!input_tex) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_texture_t *history_tex = filter->history_valid
					    ? gs_texrender_get_texture(filter->history[filter->read_index])
					    : input_tex;

	struct vec2 texel_size;
	vec2_set(&texel_size, 1.0f / (float)width, 1.0f / (float)height);

	gs_effect_set_texture(filter->param_image, input_tex);
	gs_effect_set_texture(filter->param_history, history_tex);
	gs_effect_set_vec2(filter->param_texel_size, &texel_size);

	/* Pass 2: 1x1 scene-change statistic (stays on the GPU). */
	if (filter->history_valid) {
		run_effect_pass(filter, "SceneStat", filter->scene_stat, 1, 1);
		gs_effect_set_texture(filter->param_scene_stat,
				      gs_texrender_get_texture(filter->scene_stat));
	} else {
		/* Bind something valid; history_valid == 0 ignores it. */
		gs_effect_set_texture(filter->param_scene_stat, input_tex);
	}

	gs_effect_set_float(filter->param_strength, filter->strength);
	gs_effect_set_float(filter->param_motion_threshold, filter->motion_threshold);
	gs_effect_set_float(filter->param_detail, filter->detail);
	gs_effect_set_float(filter->param_history_valid, filter->history_valid ? 1.0f : 0.0f);
	gs_effect_set_float(filter->param_scene_reset_threshold, SCENE_RESET_THRESHOLD);

	/* Pass 3: denoise into the next history buffer (ping-pong). */
	const int write_index = filter->read_index ^ 1;
	run_effect_pass(filter, "Denoise", filter->history[write_index], width, height);

	gs_texture_t *output_tex = gs_texrender_get_texture(filter->history[write_index]);
	if (!output_tex) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	filter->read_index = write_index;
	filter->history_valid = true;

	/* Pass 4: draw the denoised frame. */
	gs_effect_set_texture(filter->param_image, output_tex);
	gs_technique_t *tech = gs_effect_get_technique(filter->effect, "Draw");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(output_tex, 0, width, height);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static struct obs_source_info temporal_denoise_filter_info = {
	.id = "temporal_denoise_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = temporal_denoise_get_name,
	.create = temporal_denoise_create,
	.destroy = temporal_denoise_destroy,
	.get_defaults = temporal_denoise_defaults,
	.get_properties = temporal_denoise_properties,
	.update = temporal_denoise_update,
	.video_render = temporal_denoise_video_render,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("temporal-denoise", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&temporal_denoise_filter_info);
	return true;
}

void obs_module_unload(void) {}
