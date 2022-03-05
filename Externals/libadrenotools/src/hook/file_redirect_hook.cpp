#include "hook_impl.h"

__attribute__((visibility("default"))) void init_hook_param(const void *param) {
	init_file_redirect_hook_param(param);
}

__attribute__((visibility("default"))) FILE *fopen(const char *filename, const char *mode) {
	return hook_fopen(filename, mode);
}