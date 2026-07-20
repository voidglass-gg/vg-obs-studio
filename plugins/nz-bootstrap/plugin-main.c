#include <obs-module.h>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void)
{
	return "NZ OBS bootstrap module";
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[nz-bootstrap] module loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[nz-bootstrap] module unloaded");
}
