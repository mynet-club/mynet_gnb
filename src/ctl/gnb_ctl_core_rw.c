/*
   Copyright (C) gnbdev

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
//#include <limits.h>
//#include <stddef.h>
#include <inttypes.h>

#include "gnb_conf_type.h"
#include "gnb_node_type.h"
#include "gnb_address.h"
#include "gnb_binary.h"
#include "gnb_ctl_block.h"
#include "gnb_arg_list.h"


static void set_core_restart(gnb_ctl_block_t *ctl_block, char *data) {
	if ( '2' != *data ) {
		return;
	}
	ctl_block->core_zone->gnb_core_flag = 2;
	printf("core.flag=%u\n", ctl_block->core_zone->gnb_core_flag);
}


#if 0
core.log.file.path
core.log.file.std
core.log.file.debug
core.log.file.error
core.log.file.rotate

core.log.udp_addr4
core.log.udp_port4
core.log.udp_type

core.log.console_level
core.log.file_level
core.log.udp_level

core.log.verbose
core.log.trace

char log_file_name_std[PATH_MAX+NAME_MAX];
char log_file_name_debug[PATH_MAX+NAME_MAX];
char log_file_name_error[PATH_MAX+NAME_MAX];
#endif

static void set_log_ctl(gnb_ctl_block_t *ctl_block, char *key, char *data) {
	if ( !memcmp("core.log.file.path", key, sizeof("core.log.file.path")-1) ) {
		snprintf(ctl_block->conf_zone->conf_st.log_path, PATH_MAX, "%s", data);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_ROTATE;
		return;
	}
	#if 0
	if ( !memcmp("core.log.udp_addr4", key, sizeof("core.log.udp_addr4")-1) ) {
		;
	}
	if ( !memcmp("core.log.udp_port4", key, sizeof("core.log.udp_port4")-1) ) {
		;
	}
	#endif
	if ( !memcmp("core.log.udp_type", key, sizeof("core.log.udp_type")-1) ) {
		ctl_block->conf_zone->conf_st.log_udp_type = (uint8_t)strtoul(data, NULL, 10);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_SETUP;
		return;
	}
	if ( !memcmp("core.log.console_level", key, sizeof("core.log.console_level")-1) ) {
		ctl_block->conf_zone->conf_st.console_log_level = (uint8_t)strtoul(data, NULL, 10);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_SETUP;
		return;
	}
	if ( !memcmp("core.log.file_level", key, sizeof("core.log.file_level")-1) ) {
		ctl_block->conf_zone->conf_st.file_log_level = (uint8_t)strtoul(data, NULL, 10);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_SETUP;
		return;
	}
	if ( !memcmp("core.log.udp_level", key, sizeof("core.log.udp_level")-1) ) {
		ctl_block->conf_zone->conf_st.udp_log_level = (uint8_t)strtoul(data, NULL, 10);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_SETUP;
		return;
	}
	if ( !memcmp("core.log.verbose", key, sizeof("core.log.verbose")-1) ) {
		ctl_block->conf_zone->conf_st.log_verbose = (uint8_t)strtoul(data, NULL, 10);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_SETUP;
		return;
	}
	if ( !memcmp("core.log.trace", key, sizeof("core.log.trace")-1) ) {
		ctl_block->conf_zone->conf_st.log_trace = (uint8_t)strtoul(data, NULL, 10);
		ctl_block->core_zone->log_trigger = GNB_LOG_TRIGGER_SETUP;
		return;
	}
}

void gnb_ctl_core_write(gnb_ctl_block_t *ctl_block, gnb_arg_list_t *core_rw_arg_list) {
    int i;
	int num;
	char key[128+1];
	char data[128+1];
    for ( i=0; i < core_rw_arg_list->argc; i++ ) {
		num = sscanf(core_rw_arg_list->argv[i], "%128[^=]=%128s", key, data);
		if ( 2 != num ) {
			continue;
		}
		if ( !memcmp("core.flag", key, sizeof("core.flag")) ) {
			set_core_restart(ctl_block, data);
		}
		if ( !memcmp("core.log", key, sizeof("core.log")-1) ) {
			set_log_ctl(ctl_block, key, data);
		}
    }
}
