#include <sys/param.h>
#include <xen/hypervisor.h>

volatile shared_info_t *HYPERVISOR_shared_info __read_mostly;
paddr_t HYPERVISOR_shared_info_pa;
union start_info_union start_info_union __aligned(PAGE_SIZE);
struct hvm_start_info *hvm_start_info;

uint32_t hvm_start_paddr;

void init_start_info(void);
void
init_start_info(void)
{
	const char *cmd_line;
	if (vm_guest != VM_GUEST_XENPVH && vm_guest != VM_GUEST_GENPVH)
		return;
  
	hvm_start_info = (void *)((uintptr_t)hvm_start_paddr + KERNBASE);
  
	if (hvm_start_info->cmdline_paddr != 0) {
	    cmd_line =
		(void *)((uintptr_t)hvm_start_info->cmdline_paddr + KERNBASE);
	    strlcpy(xen_start_info.cmd_line, cmd_line,
		sizeof(xen_start_info.cmd_line));
	} else {
		xen_start_info.cmd_line[0] = '\0';
	}
	xen_start_info.flags = hvm_start_info->flags;
}
