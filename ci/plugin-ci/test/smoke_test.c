/*
 * Headless smoke test for an OBS plugin module.
 *
 * It starts libobs without a graphics/audio context (no X/Wayland needed),
 * opens and initializes the plugin shared object exactly like OBS would, and
 * verifies that:
 *   1. the module loads (obs_open_module / obs_init_module succeed),
 *   2. obs_module_ver() matches the running libobs API version,
 *   3. the reported plugin version matches the expected CI build version.
 *
 * Usage:
 *   smoke_test <plugin.so> <expected-version> [plugin-data-dir]
 */
#include <obs.h>
#include <obs-defs.h>
#include <obs-module.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define FAIL(...)                                                                                                       \
	do {                                                                                                           \
		fprintf(stderr, "smoke_test FAIL: " __VA_ARGS__);                                                       \
		fprintf(stderr, "\n");                                                                                   \
		return 1;                                                                                               \
	} while (0)

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <plugin.so> <expected-version> [plugin-data-dir]\n", argv[0]);
		return 2;
	}

	const char *plugin_path = argv[1];
	const char *expected_version = argv[2];
	const char *data_path = argc >= 4 ? argv[3] : "";

	/* ABI check via the module's own obs_module_ver export. obs_open_module
	 * performs the same comparison internally, but doing it here gives an
	 * explicit, diagnosable failure. */
	void *plugin_image = dlopen(plugin_path, RTLD_LAZY | RTLD_LOCAL);
	if (!plugin_image)
		FAIL("dlopen(%s) failed: %s", plugin_path, dlerror());

	typedef uint32_t (*module_ver_fn)(void);
	module_ver_fn module_ver = (module_ver_fn)dlsym(plugin_image, "obs_module_ver");
	if (!module_ver)
		FAIL("required export obs_module_ver not found in %s", plugin_path);

	const uint32_t module_api = module_ver();
	const uint32_t runtime_api = obs_get_version();
	if (module_api != runtime_api)
		FAIL("API version mismatch: plugin built against 0x%08x but runtime libobs is 0x%08x",
		     module_api, runtime_api);

	if (!obs_startup("en-US", NULL, NULL))
		FAIL("obs_startup() failed");

	int rc = 0;
	obs_module_t *module = NULL;

	int open_rc = obs_open_module(&module, plugin_path, data_path);
	if (open_rc != MODULE_SUCCESS)
		FAIL("obs_open_module(%s) returned %d", plugin_path, open_rc);

	if (!obs_init_module(module))
		FAIL("obs_init_module(%s) returned false", plugin_path);

	const char *reported_version = obs_get_module_version(module);
	if (!reported_version)
		FAIL("obs_get_module_version() returned NULL");

	if (strcmp(reported_version, expected_version) != 0)
		FAIL("version mismatch: expected %s, plugin reports %s", expected_version, reported_version);

	const char *module_name = obs_get_module_id(module);
	printf("smoke_test OK: module=%s path=%s version=%s libobs=%s api=0x%08x\n",
	       module_name ? module_name : "(unknown)", plugin_path, reported_version,
	       obs_get_version_string(), runtime_api);

	obs_shutdown();
	dlclose(plugin_image);
	return rc;
}
