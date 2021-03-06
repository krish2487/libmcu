#include "libmcu/system.h"
#include <assert.h>
#include "libmcu/compiler.h"

const char *system_get_version_string(void)
{
	return def2str(VERSION);
}

const char *system_get_build_date_string(void)
{
#if defined(BUILD_DATE)
	return BUILD_DATE;
#else
	return __DATE__;
#endif
}

const char * LIBMCU_WEAK system_get_serial_number_string(void)
{
	return "S/N";
}

const char * LIBMCU_WEAK system_get_reboot_reason_string(void)
{
	return "N/A";
}

void LIBMCU_WEAK system_reset_factory(void)
{
}

void LIBMCU_WEAK LIBMCU_NORETURN system_reboot(void)
{
	assert(0);
}

unsigned long LIBMCU_WEAK system_get_free_heap_bytes(void)
{
	return 0;
}

unsigned long LIBMCU_WEAK system_get_heap_watermark(void)
{
	return 0;
}

unsigned long LIBMCU_WEAK system_get_current_stack_watermark(void)
{
	return 0;
}

int LIBMCU_WEAK system_random(void)
{
	return 0;
}
