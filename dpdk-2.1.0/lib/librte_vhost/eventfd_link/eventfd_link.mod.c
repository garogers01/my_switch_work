#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x568fba06, "module_layout" },
	{ 0x364c8cd1, "misc_deregister" },
	{ 0x6633eeb3, "misc_register" },
	{ 0x738268ab, "fd_install" },
	{ 0x7baa5114, "filp_close" },
	{ 0xf233e2f8, "fput" },
	{ 0x9021c4eb, "current_task" },
	{ 0xf85dc997, "pid_task" },
	{ 0x556215a7, "find_vpid" },
	{ 0x77e2f33, "_copy_from_user" },
	{ 0xa1c76e0a, "_cond_resched" },
	{ 0xd52bf1ce, "_raw_spin_lock" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "4A97DF410894C6300A039A1");
