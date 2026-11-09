#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
struct vg_injector_child {
	void *handle;
	uint32_t pid, error;
};
typedef struct vg_injector_child (*vg_injector_launch)(void *context);
struct vg_injector_status {
	uint64_t id, creation;
	uint32_t target_pid, helper_pid, state, exit_code, error;
	bool compatibility, code_known, cancelled;
};
uint64_t vg_injector_acquire(uint32_t pid, uint64_t creation);
bool vg_injector_can_start(uint64_t lease);
bool vg_injector_start(uint64_t lease, bool compatibility, vg_injector_launch launch, void *context);
bool vg_injector_claim_direct(uint64_t lease);
void vg_injector_release(uint64_t lease);
struct vg_injector_status vg_injector_get_status(uint64_t lease);
void vg_injector_shutdown(void);
#ifdef __cplusplus
}
#endif
