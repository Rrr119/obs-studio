#include <obs-module.h>

#include "plugin-version.h"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
	return PLUGIN_NAME;
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Minimal OBS plugin used by the CI build pipeline";
}

MODULE_EXPORT const char *obs_module_author(void)
{
	return "Plugin CI Maintainers";
}

MODULE_EXPORT const char *obs_module_version(void)
{
	return PLUGIN_VERSION_STRING;
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[%s] loaded, plugin version %s, built against libobs %u",
	     PLUGIN_NAME, PLUGIN_VERSION_STRING, LIBOBS_API_VER);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[%s] unloaded", PLUGIN_NAME);
}
