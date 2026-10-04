#include "stdafx.h"
#include "rpcs3.h"

LOG_CHANNEL(sys_log, "SYS");

int main(int argc, char** argv)
{
	const int exit_code = run_rpcs3(argc, argv);
	sys_log.notice("RPCS3 terminated with exit code %d", exit_code);
	return exit_code;
}
#include "Emu/system_config.h"
extern bool cfg_vertex_buffer_upload_mode_use_buffer_view()
{
	static const bool r = []
	{
		switch (g_cfg.video.vertex_buffer_upload_mode)
		{
		case vertex_buffer_upload_mode::buffer_view:
			return true;
		case vertex_buffer_upload_mode::buffer:
			return false;
		case vertex_buffer_upload_mode::_auto:
			return true;
		}
	}();
	return r;
}
