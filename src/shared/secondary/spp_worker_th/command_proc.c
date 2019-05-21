/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2019 Nippon Telegraph and Telephone Corporation
 */

#include <unistd.h>
#include <string.h>

#include <rte_log.h>
#include <rte_branch_prediction.h>

#include "vf_deps.h"
#include "mirror_deps.h"
#include "spp_port.h"
#include "string_buffer.h"

#include "command_conn.h"
#include "cmd_parser.h"
#include "command_proc.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER2

/* request message initial size */
#define CMD_RES_ERR_MSG_SIZE  128
#define CMD_TAG_APPEND_SIZE   16
#define CMD_REQ_BUF_INIT_SIZE 2048
#define CMD_RES_BUF_INIT_SIZE 2048

#define COMMAND_RESP_LIST_EMPTY { "", NULL }

#define JSON_COMMA                ", "
#define JSON_APPEND_COMMA(flg)    ((flg)?JSON_COMMA:"")
#define JSON_APPEND_VALUE(format) "%s\"%s\": "format
#define JSON_APPEND_ARRAY         "%s\"%s\": [ %s ]"
#define JSON_APPEND_BLOCK         "%s\"%s\": { %s }"
#define JSON_APPEND_BLOCK_NONAME  "%s%s{ %s }"

/* command execution result code */
enum command_result_code {
	CRES_SUCCESS = 0,
	CRES_FAILURE,
	CRES_INVALID,
};

/* command execution result information */
struct command_result {
	/* Response code */
	int code;

	/* Response message */
	char result[SPPWK_NAME_BUFSZ];

	/* Detailed response message */
	char error_message[CMD_RES_ERR_MSG_SIZE];
};

/* command response list control structure */
struct command_response_list {
	/* Tag name */
	char tag_name[SPPWK_NAME_BUFSZ];

	/* Pointer to handling function */
	int (*func)(const char *name, char **output, void *tmp);
};

/**
 * List of worker process type. The order of items should be same as the order
 * of enum `secondary_type` in spp_proc.h.
 */
/* TODO(yasufum) rename `secondary_type` to `sppwk_proc_type`. */
const char *SPPWK_PROC_TYPE_LIST[] = {
	"none",
	"vf",
	"mirror",
	"",  /* termination */
};

/**
 * List of port abilities. The order of items should be same as the order of
 * enum `spp_port_ability_type` in spp_vf.h.
 */
const char *PORT_ABILITY_STAT_LIST[] = {
	"none",
	"add",
	"del",
	"",  /* termination */
};

/**
 * List of classifier type. The order of items should be same as the order of
 * enum `spp_classifier_type` defined in spp_proc.h.
 */
/* TODO(yasufum) fix similar var in cmd_parser.c */
const char *CLS_TYPE_A_LIST[] = {
	"none",
	"mac",
	"vlan",
	"",  /* termination */
};

/* get client id */
static int
spp_get_client_id(void)
{
	struct startup_param *startup_param;

	spp_get_mng_data_addr(&startup_param,
			NULL, NULL, NULL, NULL, NULL, NULL);
	return startup_param->client_id;
}

/* get process type */
static int
spp_get_process_type(void)
{
	struct startup_param *startup_param;

	spp_get_mng_data_addr(&startup_param,
			NULL, NULL, NULL, NULL, NULL, NULL);
	return startup_param->secondary_type;
}

/* Check if port has been flushed. */
static int
spp_check_flush_port(enum port_type iface_type, int iface_no)
{
	struct sppwk_port_info *port = get_sppwk_port(iface_type, iface_no);
	return port->ethdev_port_id >= 0;
}

/* Update classifier table with given action, add or del. */
static int
spp_update_classifier_table(
		enum sppwk_action wk_action,
		enum spp_classifier_type type __attribute__ ((unused)),
		int vid,
		const char *mac_addr_str,
		const struct sppwk_port_idx *port)
{
	struct sppwk_port_info *port_info = NULL;
	int64_t ret_mac = 0;
	uint64_t mac_addr = 0;

	RTE_LOG(DEBUG, APP, "update_classifier_table "
			"( type = mac, mac addr = %s, port = %d:%d )\n",
			mac_addr_str, port->iface_type, port->iface_no);

	ret_mac = spp_change_mac_str_to_int64(mac_addr_str);
	if (unlikely(ret_mac == -1)) {
		RTE_LOG(ERR, APP, "MAC address format error. ( mac = %s )\n",
			mac_addr_str);
		return SPP_RET_NG;
	}
	mac_addr = (uint64_t)ret_mac;

	port_info = get_sppwk_port(port->iface_type, port->iface_no);
	if (unlikely(port_info == NULL)) {
		RTE_LOG(ERR, APP, "No port. ( port = %d:%d )\n",
				port->iface_type, port->iface_no);
		return SPP_RET_NG;
	}
	if (unlikely(port_info->iface_type == UNDEF)) {
		RTE_LOG(ERR, APP, "Port not added. ( port = %d:%d )\n",
				port->iface_type, port->iface_no);
		return SPP_RET_NG;
	}

	if (wk_action == SPPWK_ACT_DEL) {
		/* Delete */
		if ((port_info->cls_attrs.vlantag.vid != 0) &&
				unlikely(port_info->cls_attrs.vlantag.vid !=
				vid)) {
			RTE_LOG(ERR, APP, "VLAN ID is different. "
					"( vid = %d )\n", vid);
			return SPP_RET_NG;
		}
		if ((port_info->cls_attrs.mac_addr != 0) &&
			unlikely(port_info->cls_attrs.mac_addr !=
					mac_addr)) {
			RTE_LOG(ERR, APP, "MAC address is different. "
					"( mac = %s )\n", mac_addr_str);
			return SPP_RET_NG;
		}

		port_info->cls_attrs.vlantag.vid = ETH_VLAN_ID_MAX;
		port_info->cls_attrs.mac_addr    = 0;
		memset(port_info->cls_attrs.mac_addr_str, 0x00,
							SPP_MIN_STR_LEN);

	} else if (wk_action == SPPWK_ACT_ADD) {
		/* Setting */
		if (unlikely(port_info->cls_attrs.vlantag.vid !=
				ETH_VLAN_ID_MAX)) {
			RTE_LOG(ERR, APP, "Port in used. "
					"( port = %d:%d, vlan = %d != %d )\n",
					port->iface_type, port->iface_no,
					port_info->cls_attrs.vlantag.vid, vid);
			return SPP_RET_NG;
		}
		if (unlikely(port_info->cls_attrs.mac_addr != 0)) {
			RTE_LOG(ERR, APP, "Port in used. "
					"( port = %d:%d, mac = %s != %s )\n",
					port->iface_type, port->iface_no,
					port_info->cls_attrs.mac_addr_str,
					mac_addr_str);
			return SPP_RET_NG;
		}

		port_info->cls_attrs.vlantag.vid = vid;
		port_info->cls_attrs.mac_addr    = mac_addr;
		strcpy(port_info->cls_attrs.mac_addr_str, mac_addr_str);
	}

	set_component_change_port(port_info, SPP_PORT_RXTX_TX);
	return SPP_RET_OK;
}

/* Assign worker thread or remove on specified lcore. */
static int
spp_update_component(
		enum sppwk_action wk_action,
		const char *name,
		unsigned int lcore_id,
		enum spp_component_type type)
{
	int ret = SPP_RET_NG;
	int ret_del = -1;
	int component_id = 0;
	unsigned int tmp_lcore_id = 0;
	struct spp_component_info *comp_info = NULL;
	struct core_info *core = NULL;
	struct core_mng_info *info = NULL;
	struct spp_component_info *comp_info_base = NULL;
	struct core_mng_info *core_info = NULL;
	int *change_core = NULL;
	int *change_component = NULL;

	spp_get_mng_data_addr(NULL, NULL, &comp_info_base, &core_info,
				&change_core, &change_component, NULL);

	switch (wk_action) {
	case SPPWK_ACT_START:
		info = (core_info + lcore_id);
		if (info->status == SPP_CORE_UNUSE) {
			RTE_LOG(ERR, APP, "Core %d is not available because "
				"it is in SPP_CORE_UNUSE state.\n", lcore_id);
			return SPP_RET_NG;
		}

		component_id = spp_get_component_id(name);
		if (component_id >= 0) {
			RTE_LOG(ERR, APP, "Component name '%s' is already "
				"used.\n", name);
			return SPP_RET_NG;
		}

		component_id = get_free_component();
		if (component_id < 0) {
			RTE_LOG(ERR, APP, "Cannot assign component over the "
				"maximum number.\n");
			return SPP_RET_NG;
		}

		core = &info->core[info->upd_index];

		comp_info = (comp_info_base + component_id);
		memset(comp_info, 0x00, sizeof(struct spp_component_info));
		strcpy(comp_info->name, name);
		comp_info->type		= type;
		comp_info->lcore_id	= lcore_id;
		comp_info->component_id	= component_id;

		core->id[core->num] = component_id;
		core->num++;
		ret = SPP_RET_OK;
		tmp_lcore_id = lcore_id;
		*(change_component + component_id) = 1;
		break;

	case SPPWK_ACT_STOP:
		component_id = spp_get_component_id(name);
		if (component_id < 0)
			return SPP_RET_OK;

		comp_info = (comp_info_base + component_id);
		tmp_lcore_id = comp_info->lcore_id;
		memset(comp_info, 0x00, sizeof(struct spp_component_info));

		info = (core_info + tmp_lcore_id);
		core = &info->core[info->upd_index];

#ifdef SPP_VF_MODULE
		/* initialize classifier information */
		if (comp_info->type == SPP_COMPONENT_CLASSIFIER_MAC)
			init_classifier_info(component_id);
#endif /* SPP_VF_MODULE */

		ret_del = del_component_info(component_id,
				core->num, core->id);
		if (ret_del >= 0)
			/* If deleted, decrement number. */
			core->num--;

		ret = SPP_RET_OK;
		*(change_component + component_id) = 0;
		break;

	default:
		break;
	}

	*(change_core + tmp_lcore_id) = 1;
	return ret;
}

/* Check if over the maximum num of rx and tx ports of component. */
static int
check_port_count(int component_type, enum spp_port_rxtx rxtx, int num_rx,
								int num_tx)
{
	RTE_LOG(INFO, SPP_COMMAND_PROC, "port count, port_type=%d,"
				" rx=%d, tx=%d\n", rxtx, num_rx, num_tx);
	if (rxtx == SPP_PORT_RXTX_RX)
		num_rx++;
	else
		num_tx++;
	/* Add rx or tx port appointed in port_type. */
	RTE_LOG(INFO, SPP_COMMAND_PROC, "Num of ports after count up,"
				" port_type=%d, rx=%d, tx=%d\n",
				rxtx, num_rx, num_tx);
	switch (component_type) {
	case SPP_COMPONENT_FORWARD:
		if (num_rx > 1 || num_tx > 1)
			return SPP_RET_NG;
		break;

	case SPP_COMPONENT_MERGE:
		if (num_tx > 1)
			return SPP_RET_NG;
		break;

	case SPP_COMPONENT_CLASSIFIER_MAC:
		if (num_rx > 1)
			return SPP_RET_NG;
		break;

	case SPP_COMPONENT_MIRROR:
		if (num_rx > 1 || num_tx > 2)
			return SPP_RET_NG;
		break;

	default:
		/* Illegal component type. */
		return SPP_RET_NG;
	}

	return SPP_RET_OK;
}

/* Port add or del to execute it */
/**
 * TODO(Ogasawara) The name `action` should be revised to be more
 * appropriate one.
 */
static int
spp_update_port(enum sppwk_action wk_action,
		const struct sppwk_port_idx *port,
		enum spp_port_rxtx rxtx,
		const char *name,
		const struct spp_port_ability *ability)
{
	int ret = SPP_RET_NG;
	int ret_check = -1;
	int ret_del = -1;
	int component_id = 0;
	int cnt = 0;
	struct spp_component_info *comp_info = NULL;
	struct sppwk_port_info *port_info = NULL;
	int *num = NULL;
	struct sppwk_port_info **ports = NULL;
	struct spp_component_info *comp_info_base = NULL;
	int *change_component = NULL;

	component_id = spp_get_component_id(name);
	if (component_id < 0) {
		RTE_LOG(ERR, APP, "Unknown component by port command. "
				"(component = %s)\n", name);
		return SPP_RET_NG;
	}
	spp_get_mng_data_addr(NULL, NULL,
			&comp_info_base, NULL, NULL, &change_component, NULL);
	comp_info = (comp_info_base + component_id);
	port_info = get_sppwk_port(port->iface_type, port->iface_no);
	if (rxtx == SPP_PORT_RXTX_RX) {
		num = &comp_info->num_rx_port;
		ports = comp_info->rx_ports;
	} else {
		num = &comp_info->num_tx_port;
		ports = comp_info->tx_ports;
	}

	switch (wk_action) {
	case SPPWK_ACT_ADD:
		/* Check if over the maximum num of ports of component. */
		if (check_port_count(comp_info->type, rxtx,
				comp_info->num_rx_port,
				comp_info->num_tx_port) != SPP_RET_OK)
			return SPP_RET_NG;

		ret_check = check_port_element(port_info, *num, ports);
		/* Check whether a port has been already registered. */
		if (ret_check >= SPP_RET_OK) {
			/* registered */
			if (ability->ops == SPPWK_PORT_ABL_OPS_ADD_VLANTAG) {
				while ((cnt < SPP_PORT_ABILITY_MAX) &&
					    (port_info->ability[cnt].ops !=
					    SPPWK_PORT_ABL_OPS_ADD_VLANTAG))
					cnt++;
				if (cnt >= SPP_PORT_ABILITY_MAX) {
					RTE_LOG(ERR, APP, "update VLAN tag "
						"Non-registratio\n");
					return SPP_RET_NG;
				}
				memcpy(&port_info->ability[cnt], ability,
					sizeof(struct spp_port_ability));

				ret = SPP_RET_OK;
				break;
			}
			return SPP_RET_OK;
		}

		if (*num >= RTE_MAX_ETHPORTS) {
			RTE_LOG(ERR, APP, "Cannot assign port over the "
				"maximum number.\n");
			return SPP_RET_NG;
		}

		if (ability->ops != SPPWK_PORT_ABL_OPS_NONE) {
			while ((cnt < SPP_PORT_ABILITY_MAX) &&
					(port_info->ability[cnt].ops !=
					SPPWK_PORT_ABL_OPS_NONE)) {
				cnt++;
			}
			if (cnt >= SPP_PORT_ABILITY_MAX) {
				RTE_LOG(ERR, APP,
						"No space of port ability.\n");
				return SPP_RET_NG;
			}
			memcpy(&port_info->ability[cnt], ability,
					sizeof(struct spp_port_ability));
		}

		port_info->iface_type = port->iface_type;
		ports[*num] = port_info;
		(*num)++;

		ret = SPP_RET_OK;
		break;

	case SPPWK_ACT_DEL:
		for (cnt = 0; cnt < SPP_PORT_ABILITY_MAX; cnt++) {
			if (port_info->ability[cnt].ops ==
					SPPWK_PORT_ABL_OPS_NONE)
				continue;

			if (port_info->ability[cnt].rxtx == rxtx)
				memset(&port_info->ability[cnt], 0x00,
					sizeof(struct spp_port_ability));
		}

		ret_del = get_del_port_element(port_info, *num, ports);
		if (ret_del == 0)
			(*num)--; /* If deleted, decrement number. */

		ret = SPP_RET_OK;
		break;

	default:
		return SPP_RET_NG;
	}

	*(change_component + component_id) = 1;
	return ret;
}

/* Flush command to execute it */
static int
spp_flush(void)
{
	int ret = SPP_RET_NG;
	struct cancel_backup_info *backup_info = NULL;

	spp_get_mng_data_addr(NULL, NULL, NULL,
				NULL, NULL, NULL, &backup_info);

	/* Initial setting of each interface. */
	ret = flush_port();
	if (ret < SPP_RET_OK)
		return ret;

	/* Flush of core index. */
	flush_core();

	/* Flush of component */
	ret = flush_component();

	backup_mng_info(backup_info);
	return ret;
}

/* Iterate core information to create response to status command */
static int
spp_iterate_core_info(struct spp_iterate_core_params *params)
{
	int ret;
	int lcore_id, cnt;
	struct core_info *core = NULL;
	struct spp_component_info *comp_info_base = NULL;
	struct spp_component_info *comp_info = NULL;

	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (spp_get_core_status(lcore_id) == SPP_CORE_UNUSE)
			continue;

		core = get_core_info(lcore_id);
		if (core->num == 0) {
			ret = (*params->element_proc)(
				params, lcore_id,
				"", SPP_TYPE_UNUSE_STR,
				0, NULL, 0, NULL);
			if (unlikely(ret != 0)) {
				RTE_LOG(ERR, APP, "Cannot iterate core "
						"information. "
						"(core = %d, type = %d)\n",
						lcore_id, SPP_COMPONENT_UNUSE);
				return SPP_RET_NG;
			}
			continue;
		}

		for (cnt = 0; cnt < core->num; cnt++) {
			spp_get_mng_data_addr(NULL, NULL, &comp_info_base,
							NULL, NULL, NULL, NULL);
			comp_info = (comp_info_base + core->id[cnt]);
#ifdef SPP_VF_MODULE
			if (comp_info->type == SPP_COMPONENT_CLASSIFIER_MAC) {
				ret = spp_classifier_get_component_status(
						lcore_id,
						core->id[cnt],
						params);
			} else {
				ret = spp_forward_get_component_status(
						lcore_id,
						core->id[cnt],
						params);
			}
#endif /* SPP_VF_MODULE */
#ifdef SPP_MIRROR_MODULE
			ret = spp_mirror_get_component_status(
						lcore_id,
						core->id[cnt],
						params);
#endif /* SPP_MIRROR_MODULE */
			if (unlikely(ret != 0)) {
				RTE_LOG(ERR, APP, "Cannot iterate core "
						"information. "
						"(core = %d, type = %d)\n",
						lcore_id, comp_info->type);
				return SPP_RET_NG;
			}
		}
	}

	return SPP_RET_OK;
}

/* Iterate classifier_table to create response to status command */
#ifdef SPP_VF_MODULE
static int
spp_iterate_classifier_table(
		struct spp_iterate_classifier_table_params *params)
{
	int ret;

	ret = spp_classifier_mac_iterate_table(params);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, APP, "Cannot iterate classifier_mac_table.\n");
		return SPP_RET_NG;
	}

	return SPP_RET_OK;
}
#endif /* SPP_VF_MODULE */

/**
 * Get consistent port ID of rte ethdev from resource UID such as `phy:0`.
 * It returns a port ID, or error code if it's failed to.
 */
static int
sppwk_get_ethdev_port_id(enum port_type iface_type, int iface_no)
{
	struct iface_info *iface_info = NULL;

	spp_get_mng_data_addr(NULL, &iface_info,
				NULL, NULL, NULL, NULL, NULL);
	switch (iface_type) {
	case PHY:
		return iface_info->nic[iface_no].ethdev_port_id;
	case RING:
		return iface_info->ring[iface_no].ethdev_port_id;
	case VHOST:
		return iface_info->vhost[iface_no].ethdev_port_id;
	default:
		return SPP_RET_NG;
	}
}

/* append a comma for JSON format */
static int
append_json_comma(char **output)
{
	*output = spp_strbuf_append(*output, JSON_COMMA, strlen(JSON_COMMA));
	if (unlikely(*output == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"JSON's comma failed to add.\n");
		return SPP_RET_NG;
	}

	return SPP_RET_OK;
}

/* append data of unsigned integral type for JSON format */
static int
append_json_uint_value(const char *name, char **output, unsigned int value)
{
	int len = strlen(*output);
	/* extend the buffer */
	*output = spp_strbuf_append(*output, "",
			strlen(name) + CMD_TAG_APPEND_SIZE*2);
	if (unlikely(*output == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"JSON's numeric format failed to add. "
				"(name = %s, uint = %u)\n", name, value);
		return SPP_RET_NG;
	}

	sprintf(&(*output)[len], JSON_APPEND_VALUE("%u"),
			JSON_APPEND_COMMA(len), name, value);
	return SPP_RET_OK;
}

/* append data of integral type for JSON format */
static int
append_json_int_value(const char *name, char **output, int value)
{
	int len = strlen(*output);
	/* extend the buffer */
	*output = spp_strbuf_append(*output, "",
			strlen(name) + CMD_TAG_APPEND_SIZE*2);
	if (unlikely(*output == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"JSON's numeric format failed to add. "
				"(name = %s, int = %d)\n", name, value);
		return SPP_RET_NG;
	}

	sprintf(&(*output)[len], JSON_APPEND_VALUE("%d"),
			JSON_APPEND_COMMA(len), name, value);
	return SPP_RET_OK;
}

/* append data of string type for JSON format */
static int
append_json_str_value(const char *name, char **output, const char *str)
{
	int len = strlen(*output);
	/* extend the buffer */
	*output = spp_strbuf_append(*output, "",
			strlen(name) + strlen(str) + CMD_TAG_APPEND_SIZE);
	if (unlikely(*output == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"JSON's string format failed to add. "
				"(name = %s, str = %s)\n", name, str);
		return SPP_RET_NG;
	}

	sprintf(&(*output)[len], JSON_APPEND_VALUE("\"%s\""),
			JSON_APPEND_COMMA(len), name, str);
	return SPP_RET_OK;
}

/* append brackets of the array for JSON format */
static int
append_json_array_brackets(const char *name, char **output, const char *str)
{
	int len = strlen(*output);
	/* extend the buffer */
	*output = spp_strbuf_append(*output, "",
			strlen(name) + strlen(str) + CMD_TAG_APPEND_SIZE);
	if (unlikely(*output == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"JSON's square bracket failed to add. "
				"(name = %s, str = %s)\n", name, str);
		return SPP_RET_NG;
	}

	sprintf(&(*output)[len], JSON_APPEND_ARRAY,
			JSON_APPEND_COMMA(len), name, str);
	return SPP_RET_OK;
}

/* append brackets of the blocks for JSON format */
static int
append_json_block_brackets(const char *name, char **output, const char *str)
{
	int len = strlen(*output);
	/* extend the buffer */
	*output = spp_strbuf_append(*output, "",
			strlen(name) + strlen(str) + CMD_TAG_APPEND_SIZE);
	if (unlikely(*output == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"JSON's curly bracket failed to add. "
				"(name = %s, str = %s)\n", name, str);
		return SPP_RET_NG;
	}

	if (name[0] == '\0')
		sprintf(&(*output)[len], JSON_APPEND_BLOCK_NONAME,
				JSON_APPEND_COMMA(len), name, str);
	else
		sprintf(&(*output)[len], JSON_APPEND_BLOCK,
				JSON_APPEND_COMMA(len), name, str);
	return SPP_RET_OK;
}

/* execute one command */
static int
execute_command(const struct spp_command *command)
{
	int ret = SPP_RET_OK;

	switch (command->type) {
	case SPPWK_CMDTYPE_CLS_MAC:
	case SPPWK_CMDTYPE_CLS_VLAN:
		RTE_LOG(INFO, SPP_COMMAND_PROC,
				"Execute classifier_table command.\n");
		ret = spp_update_classifier_table(
				command->spec.cls_table.wk_action,
				command->spec.cls_table.type,
				command->spec.cls_table.vid,
				command->spec.cls_table.mac,
				&command->spec.cls_table.port);
		if (ret == 0) {
			RTE_LOG(INFO, SPP_COMMAND_PROC,
					"Execute flush.\n");
			ret = spp_flush();
		}
		break;

	case SPPWK_CMDTYPE_WORKER:
		RTE_LOG(INFO, SPP_COMMAND_PROC,
				"Execute component command.\n");
		ret = spp_update_component(
				command->spec.comp.wk_action,
				command->spec.comp.name,
				command->spec.comp.core,
				command->spec.comp.type);
		if (ret == 0) {
			RTE_LOG(INFO, SPP_COMMAND_PROC,
					"Execute flush.\n");
			ret = spp_flush();
		}
		break;

	case SPPWK_CMDTYPE_PORT:
		RTE_LOG(INFO, SPP_COMMAND_PROC,
				"Execute port command. (act = %d)\n",
				command->spec.port.wk_action);
		ret = spp_update_port(
				command->spec.port.wk_action,
				&command->spec.port.port,
				command->spec.port.rxtx,
				command->spec.port.name,
				&command->spec.port.ability);
		if (ret == 0) {
			RTE_LOG(INFO, SPP_COMMAND_PROC,
					"Execute flush.\n");
			ret = spp_flush();
		}
		break;

	default:
		RTE_LOG(INFO, SPP_COMMAND_PROC,
				"Execute other command. type=%d\n",
				command->type);
		/* nothing to do here */
		break;
	}

	return ret;
}

/* Fill err_msg obj with given error message. */
static const char *
make_decode_error_message(
		const struct sppwk_parse_err_msg *err_msg,
		char *message)
{
	switch (err_msg->code) {
	case SPPWK_PARSE_WRONG_FORMAT:
		sprintf(message, "Wrong message format");
		break;

	case SPPWK_PARSE_UNKNOWN_CMD:
		/* TODO(yasufum) Fix compile err if space exists before "(" */
		sprintf(message, "Unknown command(%s)", err_msg->details);
		break;

	case SPPWK_PARSE_NO_PARAM:
		sprintf(message, "No or insufficient number of params (%s)",
				err_msg->msg);
		break;

	case SPPWK_PARSE_INVALID_TYPE:
		sprintf(message, "Invalid value type (%s)",
				err_msg->msg);
		break;

	case SPPWK_PARSE_INVALID_VALUE:
		sprintf(message, "Invalid value (%s)", err_msg->msg);
		break;

	default:
		sprintf(message, "Failed to parse with unexpected reason");
		break;
	}

	return message;
}

/* set the command result */
static inline void
set_command_results(struct command_result *result,
		int code, const char *error_messege)
{
	result->code = code;
	switch (code) {
	case CRES_SUCCESS:
		strcpy(result->result, "success");
		memset(result->error_message, 0x00, CMD_RES_ERR_MSG_SIZE);
		break;
	case CRES_FAILURE:
		strcpy(result->result, "error");
		strcpy(result->error_message, error_messege);
		break;
	case CRES_INVALID: /* FALLTHROUGH */
	default:
		strcpy(result->result, "invalid");
		memset(result->error_message, 0x00, CMD_RES_ERR_MSG_SIZE);
		break;
	}
}

/* set decode error to command result */
static void
set_decode_error_to_results(struct command_result *results,
		const struct sppwk_cmd_req *request,
		const struct sppwk_parse_err_msg *err_msg)
{
	int i;
	const char *tmp_buff;
	char error_messege[CMD_RES_ERR_MSG_SIZE];

	for (i = 0; i < request->num_command; i++) {
		if (err_msg->code == 0)
			set_command_results(&results[i], CRES_SUCCESS, "");
		else
			set_command_results(&results[i], CRES_INVALID, "");
	}

	if (err_msg->code != 0) {
		tmp_buff = make_decode_error_message(err_msg,
				error_messege);
		set_command_results(&results[request->num_valid_command],
				CRES_FAILURE, tmp_buff);
	}
}

/* append a command result for JSON format */
static int
append_result_value(const char *name, char **output, void *tmp)
{
	const struct command_result *result = tmp;
	return append_json_str_value(name, output, result->result);
}

/* append error details for JSON format */
static int
append_error_details_value(const char *name, char **output, void *tmp)
{
	int ret = SPP_RET_NG;
	const struct command_result *result = tmp;
	char *tmp_buff;
	/* string is empty, except for errors */
	if (result->error_message[0] == '\0')
		return SPP_RET_OK;

	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	ret = append_json_str_value("message", &tmp_buff,
			result->error_message);
	if (unlikely(ret < 0)) {
		spp_strbuf_free(tmp_buff);
		return SPP_RET_NG;
	}

	ret = append_json_block_brackets(name, output, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/* append a client id for JSON format */
static int
append_client_id_value(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	return append_json_int_value(name, output, spp_get_client_id());
}

/* append a list of interface numbers */
static int
append_interface_array(char **output, const enum port_type type)
{
	int i, port_cnt = 0;
	char tmp_str[CMD_TAG_APPEND_SIZE];

	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (!spp_check_flush_port(type, i))
			continue;

		sprintf(tmp_str, "%s%d", JSON_APPEND_COMMA(port_cnt), i);

		*output = spp_strbuf_append(*output, tmp_str, strlen(tmp_str));
		if (unlikely(*output == NULL)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Interface number failed to add. "
					"(type = %d)\n", type);
			return SPP_RET_NG;
		}

		port_cnt++;
	}

	return SPP_RET_OK;
}

/* append a secondary process type for JSON format */
static int
append_process_type_value(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	return append_json_str_value(name, output,
			SPPWK_PROC_TYPE_LIST[spp_get_process_type()]);
}

/* append a list of interface numbers for JSON format */
static int
append_interface_value(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	int ret = SPP_RET_NG;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	if (strcmp(name, SPP_IFTYPE_NIC_STR) == 0)
		ret = append_interface_array(&tmp_buff, PHY);

	else if (strcmp(name, SPP_IFTYPE_VHOST_STR) == 0)
		ret = append_interface_array(&tmp_buff, VHOST);

	else if (strcmp(name, SPP_IFTYPE_RING_STR) == 0)
		ret = append_interface_array(&tmp_buff, RING);

	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(tmp_buff);
		return SPP_RET_NG;
	}

	ret = append_json_array_brackets(name, output, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/* append a value of vlan for JSON format */
static int
append_vlan_value(char **output, const int ope, const int vid, const int pcp)
{
	int ret = SPP_RET_OK;
	ret = append_json_str_value("operation", output,
			PORT_ABILITY_STAT_LIST[ope]);
	if (unlikely(ret < SPP_RET_OK))
		return SPP_RET_NG;

	ret = append_json_int_value("id", output, vid);
	if (unlikely(ret < 0))
		return SPP_RET_NG;

	ret = append_json_int_value("pcp", output, pcp);
	if (unlikely(ret < 0))
		return SPP_RET_NG;

	return SPP_RET_OK;
}

/* append a block of vlan for JSON format */
static int
append_vlan_block(const char *name, char **output,
		const int port_id, const enum spp_port_rxtx rxtx)
{
	int ret = SPP_RET_NG;
	int i = 0;
	struct spp_port_ability *info = NULL;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	spp_port_ability_get_info(port_id, rxtx, &info);
	for (i = 0; i < SPP_PORT_ABILITY_MAX; i++) {
		switch (info[i].ops) {
		case SPPWK_PORT_ABL_OPS_ADD_VLANTAG:
		case SPPWK_PORT_ABL_OPS_DEL_VLANTAG:
			ret = append_vlan_value(&tmp_buff, info[i].ops,
					info[i].data.vlantag.vid,
					info[i].data.vlantag.pcp);
			if (unlikely(ret < SPP_RET_OK))
				return SPP_RET_NG;

			/*
			 * Change counter to "maximum+1" for exit the loop.
			 * An if statement after loop termination is false
			 * by "maximum+1 ".
			 */
			i = SPP_PORT_ABILITY_MAX + 1;
			break;
		default:
			/* not used */
			break;
		}
	}
	if (i == SPP_PORT_ABILITY_MAX) {
		ret = append_vlan_value(&tmp_buff, SPPWK_PORT_ABL_OPS_NONE,
				0, 0);
		if (unlikely(ret < SPP_RET_OK))
			return SPP_RET_NG;
	}

	ret = append_json_block_brackets(name, output, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/* append a block of port numbers for JSON format */
static int
append_port_block(char **output, const struct sppwk_port_idx *port,
		const enum spp_port_rxtx rxtx)
{
	int ret = SPP_RET_NG;
	char port_str[CMD_TAG_APPEND_SIZE];
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = port_block)\n");
		return SPP_RET_NG;
	}

	spp_format_port_string(port_str, port->iface_type, port->iface_no);
	ret = append_json_str_value("port", &tmp_buff, port_str);
	if (unlikely(ret < SPP_RET_OK))
		return SPP_RET_NG;

	ret = append_vlan_block("vlan", &tmp_buff,
			sppwk_get_ethdev_port_id(
				port->iface_type, port->iface_no),
			rxtx);
	if (unlikely(ret < SPP_RET_OK))
		return SPP_RET_NG;

	ret = append_json_block_brackets("", output, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/* append a list of port numbers for JSON format */
static int
append_port_array(const char *name, char **output, const int num,
		const struct sppwk_port_idx *ports,
		const enum spp_port_rxtx rxtx)
{
	int ret = SPP_RET_NG;
	int i = 0;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	for (i = 0; i < num; i++) {
		ret = append_port_block(&tmp_buff, &ports[i], rxtx);
		if (unlikely(ret < SPP_RET_OK))
			return SPP_RET_NG;
	}

	ret = append_json_array_brackets(name, output, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/**
 * TODO(yasufum) add usages called from `append_core_value` or refactor
 * confusing function names.
 */
/* append one element of core information for JSON format */
static int
append_core_element_value(
		struct spp_iterate_core_params *params,
		const unsigned int lcore_id,
		const char *name, const char *type,
		const int num_rx, const struct sppwk_port_idx *rx_ports,
		const int num_tx, const struct sppwk_port_idx *tx_ports)
{
	int ret = SPP_RET_NG;
	int unuse_flg = 0;
	char *buff, *tmp_buff;
	buff = params->output;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		/* TODO(yasufum) refactor no meaning err msg */
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"allocate error. (name = %s)\n",
				name);
		return ret;
	}

	/* there is unnecessary data when "unuse" by type */
	unuse_flg = strcmp(type, SPP_TYPE_UNUSE_STR);

	/**
	 * TODO(yasufum) change ambiguous "core" to more specific one such as
	 * "worker-lcores" or "slave-lcores".
	 */
	ret = append_json_uint_value("core", &tmp_buff, lcore_id);
	if (unlikely(ret < SPP_RET_OK))
		return ret;

	if (unuse_flg) {
		ret = append_json_str_value("name", &tmp_buff, name);
		if (unlikely(ret < 0))
			return ret;
	}

	ret = append_json_str_value("type", &tmp_buff, type);
	if (unlikely(ret < SPP_RET_OK))
		return ret;

	if (unuse_flg) {
		ret = append_port_array("rx_port", &tmp_buff,
				num_rx, rx_ports, SPP_PORT_RXTX_RX);
		if (unlikely(ret < 0))
			return ret;

		ret = append_port_array("tx_port", &tmp_buff,
				num_tx, tx_ports, SPP_PORT_RXTX_TX);
		if (unlikely(ret < SPP_RET_OK))
			return ret;
	}

	ret = append_json_block_brackets("", &buff, tmp_buff);
	spp_strbuf_free(tmp_buff);
	params->output = buff;
	return ret;
}

/* append master lcore in JSON format */
static int
append_master_lcore_value(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	int ret = SPP_RET_NG;
	ret = append_json_int_value(name, output, rte_get_master_lcore());
	return ret;
}

/* append a list of core information for JSON format */
static int
append_core_value(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	int ret = SPP_RET_NG;
	struct spp_iterate_core_params itr_params;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	itr_params.output = tmp_buff;
	itr_params.element_proc = append_core_element_value;

	ret = spp_iterate_core_info(&itr_params);
	if (unlikely(ret != SPP_RET_OK)) {
		spp_strbuf_free(itr_params.output);
		return SPP_RET_NG;
	}

	ret = append_json_array_brackets(name, output, itr_params.output);
	spp_strbuf_free(itr_params.output);
	return ret;
}

/* append one element of classifier table for JSON format */
#ifdef SPP_VF_MODULE
static int
append_classifier_element_value(
		struct spp_iterate_classifier_table_params *params,
		enum spp_classifier_type type,
		int vid, const char *mac,
		const struct sppwk_port_idx *port)
{
	int ret = SPP_RET_NG;
	char *buff, *tmp_buff;
	char port_str[CMD_TAG_APPEND_SIZE];
	char value_str[SPP_MIN_STR_LEN];
	buff = params->output;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = classifier_table)\n");
		return ret;
	}

	spp_format_port_string(port_str, port->iface_type, port->iface_no);

	ret = append_json_str_value("type", &tmp_buff,
			CLS_TYPE_A_LIST[type]);
	if (unlikely(ret < SPP_RET_OK))
		return ret;

	memset(value_str, 0x00, SPP_MIN_STR_LEN);
	switch (type) {
	case SPP_CLASSIFIER_TYPE_MAC:
		sprintf(value_str, "%s", mac);
		break;
	case SPP_CLASSIFIER_TYPE_VLAN:
		sprintf(value_str, "%d/%s", vid, mac);
		break;
	default:
		/* not used */
		break;
	}

	ret = append_json_str_value("value", &tmp_buff, value_str);
	if (unlikely(ret < 0))
		return ret;

	ret = append_json_str_value("port", &tmp_buff, port_str);
	if (unlikely(ret < SPP_RET_OK))
		return ret;

	ret = append_json_block_brackets("", &buff, tmp_buff);
	spp_strbuf_free(tmp_buff);
	params->output = buff;
	return ret;
}
#endif /* SPP_VF_MODULE */

/* append a list of classifier table for JSON format */
#ifdef SPP_VF_MODULE
static int
append_classifier_table_value(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	int ret = SPP_RET_NG;
	struct spp_iterate_classifier_table_params itr_params;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	itr_params.output = tmp_buff;
	itr_params.element_proc = append_classifier_element_value;

	ret = spp_iterate_classifier_table(&itr_params);
	if (unlikely(ret != SPP_RET_OK)) {
		spp_strbuf_free(itr_params.output);
		return SPP_RET_NG;
	}

	ret = append_json_array_brackets(name, output, itr_params.output);
	spp_strbuf_free(itr_params.output);
	return ret;
}
#endif /* SPP_VF_MODULE */
/* append string of command response list for JSON format */
static int
append_response_list_value(char **output,
		struct command_response_list *list,
		void *tmp)
{
	int ret = SPP_RET_NG;
	int i;
	char *tmp_buff;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = response_list)\n");
		return SPP_RET_NG;
	}

	for (i = 0; list[i].tag_name[0] != '\0'; i++) {
		tmp_buff[0] = '\0';
		ret = list[i].func(list[i].tag_name, &tmp_buff, tmp);
		if (unlikely(ret < SPP_RET_OK)) {
			spp_strbuf_free(tmp_buff);
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Failed to get reply string. "
					"(tag = %s)\n", list[i].tag_name);
			return SPP_RET_NG;
		}

		if (tmp_buff[0] == '\0')
			continue;

		if ((*output)[0] != '\0') {
			ret = append_json_comma(output);
			if (unlikely(ret < SPP_RET_OK)) {
				spp_strbuf_free(tmp_buff);
				RTE_LOG(ERR, SPP_COMMAND_PROC,
						"Failed to add commas. "
						"(tag = %s)\n",
						list[i].tag_name);
				return SPP_RET_NG;
			}
		}

		*output = spp_strbuf_append(*output, tmp_buff,
				strlen(tmp_buff));
		if (unlikely(*output == NULL)) {
			spp_strbuf_free(tmp_buff);
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Failed to add reply string. "
					"(tag = %s)\n",
					list[i].tag_name);
			return SPP_RET_NG;
		}
	}

	spp_strbuf_free(tmp_buff);
	return SPP_RET_OK;
}

/* termination constant of command response list */
#define COMMAND_RESP_TAG_LIST_EMPTY { "", NULL }

/* command response result string list */
struct command_response_list response_result_list[] = {
	{ "result",        append_result_value },
	{ "error_details", append_error_details_value },
	COMMAND_RESP_TAG_LIST_EMPTY
};

/**
 * TODO(yasufum) Add desc why it is needed and how to be used. At least, func
 * name is not appropriate because not for reponse, but name of funcs returns
 * response.
 */
/* command response status information string list */
struct command_response_list response_info_list[] = {
	{ "client-id",        append_client_id_value },
	{ "phy",              append_interface_value },
	{ "vhost",            append_interface_value },
	{ "ring",             append_interface_value },
	{ "master-lcore",     append_master_lcore_value },
	{ "core",             append_core_value },
#ifdef SPP_VF_MODULE
	{ "classifier_table", append_classifier_table_value },
#endif /* SPP_VF_MODULE */
	COMMAND_RESP_TAG_LIST_EMPTY
};

/* append a list of command results for JSON format. */
static int
append_command_results_value(const char *name, char **output,
		int num, struct command_result *results)
{
	int ret = SPP_RET_NG;
	int i;
	char *tmp_buff1, *tmp_buff2;
	tmp_buff1 = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff1 == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s, buff=1)\n",
				name);
		return SPP_RET_NG;
	}

	tmp_buff2 = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff2 == NULL)) {
		spp_strbuf_free(tmp_buff1);
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s, buff=2)\n",
				name);
		return SPP_RET_NG;
	}

	for (i = 0; i < num; i++) {
		tmp_buff1[0] = '\0';
		ret = append_response_list_value(&tmp_buff1,
				response_result_list, &results[i]);
		if (unlikely(ret < 0)) {
			spp_strbuf_free(tmp_buff1);
			spp_strbuf_free(tmp_buff2);
			return SPP_RET_NG;
		}

		ret = append_json_block_brackets("", &tmp_buff2, tmp_buff1);
		if (unlikely(ret < 0)) {
			spp_strbuf_free(tmp_buff1);
			spp_strbuf_free(tmp_buff2);
			return SPP_RET_NG;
		}

	}

	ret = append_json_array_brackets(name, output, tmp_buff2);
	spp_strbuf_free(tmp_buff1);
	spp_strbuf_free(tmp_buff2);
	return ret;
}

/* append a list of status information for JSON format. */
static int
append_info_value(const char *name, char **output)
{
	int ret = SPP_RET_NG;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	ret = append_response_list_value(&tmp_buff,
			response_info_list, NULL);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(tmp_buff);
		return SPP_RET_NG;
	}

	ret = append_json_block_brackets(name, output, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/* send response for decode error */
static void
send_decode_error_response(int *sock,
		const struct sppwk_cmd_req *request,
		struct command_result *command_results)
{
	int ret = SPP_RET_NG;
	char *msg, *tmp_buff;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		/* TODO(yasufum) refactor no meaning err msg */
		RTE_LOG(ERR, SPP_COMMAND_PROC, "allocate error. "
				"(name = decode_error_response)\n");
		return;
	}

	/* create & append result array */
	ret = append_command_results_value("results", &tmp_buff,
			request->num_command, command_results);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(tmp_buff);
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Failed to make command result response.\n");
		return;
	}

	msg = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(msg == NULL)) {
		spp_strbuf_free(tmp_buff);
		/* TODO(yasufum) refactor no meaning err msg */
		RTE_LOG(ERR, SPP_COMMAND_PROC, "allocate error. "
				"(name = decode_error_response)\n");
		return;
	}
	ret = append_json_block_brackets("", &msg, tmp_buff);
	spp_strbuf_free(tmp_buff);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(msg);
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}

	RTE_LOG(DEBUG, SPP_COMMAND_PROC,
			"Make command response (decode error). "
			"response_str=\n%s\n", msg);

	/* send response to requester */
	ret = spp_send_message(sock, msg, strlen(msg));
	if (unlikely(ret != SPP_RET_OK)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Failed to send decode error response.\n");
		/* not return */
	}

	spp_strbuf_free(msg);
}

/* send response for command execution result */
static void
send_command_result_response(int *sock,
		const struct sppwk_cmd_req *request,
		struct command_result *command_results)
{
	int ret = SPP_RET_NG;
	char *msg, *tmp_buff;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}

	/* create & append result array */
	ret = append_command_results_value("results", &tmp_buff,
			request->num_command, command_results);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(tmp_buff);
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Failed to make command result response.\n");
		return;
	}

	/* append client id information value */
	if (request->is_requested_client_id) {
		ret = append_client_id_value("client_id", &tmp_buff, NULL);
		if (unlikely(ret < SPP_RET_OK)) {
			spp_strbuf_free(tmp_buff);
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make "
					"client id response.\n");
			return;
		}
		ret = append_process_type_value("process_type",
							&tmp_buff, NULL);
	}

	/* append info value */
	if (request->is_requested_status) {
		ret = append_info_value("info", &tmp_buff);
		if (unlikely(ret < SPP_RET_OK)) {
			spp_strbuf_free(tmp_buff);
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Failed to make status response.\n");
			return;
		}
	}

	msg = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(msg == NULL)) {
		spp_strbuf_free(tmp_buff);
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}
	ret = append_json_block_brackets("", &msg, tmp_buff);
	spp_strbuf_free(tmp_buff);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(msg);
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}

	RTE_LOG(DEBUG, SPP_COMMAND_PROC,
			"Make command response (command result). "
			"response_str=\n%s\n", msg);

	/* send response to requester */
	ret = spp_send_message(sock, msg, strlen(msg));
	if (unlikely(ret != SPP_RET_OK)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
			"Failed to send command result response.\n");
		/* not return */
	}

	spp_strbuf_free(msg);
}

/* process command request from no-null-terminated string */
static int
process_request(int *sock, const char *request_str, size_t request_str_len)
{
	int ret = SPP_RET_NG;
	int i;

	struct sppwk_cmd_req request;
	struct sppwk_parse_err_msg wk_err_msg;
	struct command_result command_results[SPPWK_MAX_CMDS];

	memset(&request, 0, sizeof(struct sppwk_cmd_req));
	memset(&wk_err_msg, 0, sizeof(struct sppwk_parse_err_msg));
	memset(command_results, 0, sizeof(command_results));

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Start command request processing. "
			"request_str=\n%.*s\n",
			(int)request_str_len, request_str);

	/* decode request message */
	ret = sppwk_parse_req(
			&request, request_str, request_str_len, &wk_err_msg);
	if (unlikely(ret != SPP_RET_OK)) {
		/* send error response */
		set_decode_error_to_results(command_results, &request,
				&wk_err_msg);
		send_decode_error_response(sock, &request, command_results);
		RTE_LOG(DEBUG, SPP_COMMAND_PROC,
				"End command request processing.\n");
		return SPP_RET_OK;
	}

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Command request is valid. "
			"num_command=%d, num_valid_command=%d\n",
			request.num_command, request.num_valid_command);

	/* execute commands */
	for (i = 0; i < request.num_command ; ++i) {
		ret = execute_command(request.commands + i);
		if (unlikely(ret != SPP_RET_OK)) {
			set_command_results(&command_results[i], CRES_FAILURE,
					"error occur");

			/* not execute remaining commands */
			for (++i; i < request.num_command ; ++i)
				set_command_results(&command_results[i],
					CRES_INVALID, "");

			break;
		}

		set_command_results(&command_results[i], CRES_SUCCESS, "");
	}

	if (request.is_requested_exit) {
		/* Terminated by process exit command.                       */
		/* Other route is normal end because it responds to command. */
		set_command_results(&command_results[0], CRES_SUCCESS, "");
		send_command_result_response(sock, &request, command_results);
		RTE_LOG(INFO, SPP_COMMAND_PROC,
				"Terminate process for exit.\n");
		return SPP_RET_NG;
	}

	/* send response */
	send_command_result_response(sock, &request, command_results);

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "End command request processing.\n");

	return SPP_RET_OK;
}

/* initialize command processor. */
int
spp_command_proc_init(const char *controller_ip, int controller_port)
{
	return spp_command_conn_init(controller_ip, controller_port);
}

/* process command from controller. */
int
spp_command_proc_do(void)
{
	int ret = SPP_RET_NG;
	int msg_ret = -1;

	static int sock = -1;
	static char *msgbuf;

	if (unlikely(msgbuf == NULL)) {
		msgbuf = spp_strbuf_allocate(CMD_REQ_BUF_INIT_SIZE);
		if (unlikely(msgbuf == NULL)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Cannot allocate memory "
					"for receive data(init).\n");
			return SPP_RET_NG;
		}
	}

	ret = spp_connect_to_controller(&sock);

	if (unlikely(ret != SPP_RET_OK))
		return SPP_RET_OK;

	msg_ret = spp_receive_message(&sock, &msgbuf);
	if (unlikely(msg_ret <= 0)) {
		if (likely(msg_ret == 0))
			return SPP_RET_OK;
		else if (unlikely(msg_ret == SPP_CONNERR_TEMPORARY))
			return SPP_RET_OK;
		else
			return SPP_RET_NG;
	}

	ret = process_request(&sock, msgbuf, msg_ret);
	spp_strbuf_remove_front(msgbuf, msg_ret);

	return ret;
}
