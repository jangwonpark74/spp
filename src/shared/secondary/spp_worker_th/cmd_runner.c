/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2019 Nippon Telegraph and Telephone Corporation
 */

#include <unistd.h>
#include <string.h>

#include <rte_log.h>
#include <rte_branch_prediction.h>

#include "cmd_runner.h"
#include "vf_deps.h"
#include "mirror_deps.h"
#include "spp_port.h"
#include "shared/secondary/string_buffer.h"
#include "shared/secondary/json_helper.h"
#include "cmd_res_formatter.h"
#include "conn_spp_ctl.h"
#include "cmd_parser.h"

#define RTE_LOGTYPE_WK_CMD_RUNNER RTE_LOGTYPE_USER1

/* request message initial size */
#define CMD_REQ_BUF_INIT_SIZE 2048

enum cmd_res_code {
	CMD_SUCCESS = 0,
	CMD_FAILED,
	CMD_INVALID,
};

/* TODO(yasufum) move to another file for util funcs. */
/* Get client ID from global command params. */
static int
sppwk_get_client_id(void)
{
	struct startup_param *params;
	sppwk_get_mng_data(&params, NULL, NULL, NULL, NULL, NULL, NULL);
	return params->client_id;
}

/* Update classifier table with given action, add or del. */
static int
update_cls_table(enum sppwk_action wk_action,
		enum spp_classifier_type type __attribute__ ((unused)),
		int vid, const char *mac_str,
		const struct sppwk_port_idx *port)
{
	/**
	 * Use two types of mac addr in int64_t and uint64_t because first
	 * one is checked if converted value from string  is negative for error.
	 * If it is invalid, convert it to uint64_t.
	 */
	int64_t mac_int64;
	uint64_t mac_uint64;
	struct sppwk_port_info *port_info;

	RTE_LOG(DEBUG, WK_CMD_RUNNER, "Called __func__ with "
			"type `mac`, mac_addr `%s`, and port `%d:%d`.\n",
			mac_str, port->iface_type, port->iface_no);

	mac_int64 = sppwk_convert_mac_str_to_int64(mac_str);
	if (unlikely(mac_int64 == -1)) {
		RTE_LOG(ERR, WK_CMD_RUNNER, "Invalid MAC address `%s`.\n",
				mac_str);
		return SPP_RET_NG;
	}
	mac_uint64 = (uint64_t)mac_int64;

	port_info = get_sppwk_port(port->iface_type, port->iface_no);
	if (unlikely(port_info == NULL)) {
		RTE_LOG(ERR, WK_CMD_RUNNER, "Failed to get port %d:%d.\n",
				port->iface_type, port->iface_no);
		return SPP_RET_NG;
	}
	if (unlikely(port_info->iface_type == UNDEF)) {
		RTE_LOG(ERR, WK_CMD_RUNNER, "Port %d:%d doesn't exist.\n",
				port->iface_type, port->iface_no);
		return SPP_RET_NG;
	}

	if (wk_action == SPPWK_ACT_DEL) {
		if ((port_info->cls_attrs.vlantag.vid != 0) &&
				port_info->cls_attrs.vlantag.vid != vid) {
			RTE_LOG(ERR, WK_CMD_RUNNER,
					"Unexpected VLAN ID `%d`.\n", vid);
			return SPP_RET_NG;
		}
		if ((port_info->cls_attrs.mac_addr != 0) &&
				port_info->cls_attrs.mac_addr != mac_uint64) {
			RTE_LOG(ERR, WK_CMD_RUNNER, "Unexpected MAC %s.\n",
					mac_str);
			return SPP_RET_NG;
		}

		/* Initialize deleted attributes again. */
		port_info->cls_attrs.vlantag.vid = ETH_VLAN_ID_MAX;
		port_info->cls_attrs.mac_addr = 0;
		memset(port_info->cls_attrs.mac_addr_str, 0x00, STR_LEN_SHORT);
	} else if (wk_action == SPPWK_ACT_ADD) {
		if (unlikely(port_info->cls_attrs.vlantag.vid !=
				ETH_VLAN_ID_MAX)) {
			/* TODO(yasufum) why two vids are required in msg ? */
			RTE_LOG(ERR, WK_CMD_RUNNER, "Used port %d:%d, vid %d != %d.\n",
					port->iface_type, port->iface_no,
					port_info->cls_attrs.vlantag.vid, vid);
			return SPP_RET_NG;
		}
		if (unlikely(port_info->cls_attrs.mac_addr != 0)) {
			/* TODO(yasufum) why two macs are required in msg ? */
			RTE_LOG(ERR, WK_CMD_RUNNER, "Used port %d:%d, mac %s != %s.\n",
					port->iface_type, port->iface_no,
					port_info->cls_attrs.mac_addr_str,
					mac_str);
			return SPP_RET_NG;
		}

		/* Update attrs with validated params. */
		port_info->cls_attrs.vlantag.vid = vid;
		port_info->cls_attrs.mac_addr = mac_uint64;
		strcpy(port_info->cls_attrs.mac_addr_str, mac_str);
	}

	set_component_change_port(port_info, SPP_PORT_RXTX_TX);
	return SPP_RET_OK;
}

/* Assign worker thread or remove on specified lcore. */
/* TODO(yasufum) revise func name for removing the term `component`. */
static int
update_comp(enum sppwk_action wk_action, const char *name,
		unsigned int lcore_id, enum sppwk_worker_type wk_type)
{
	int ret;
	int ret_del;
	int comp_lcore_id = 0;
	unsigned int tmp_lcore_id = 0;
	struct sppwk_comp_info *comp_info = NULL;
	/* TODO(yasufum) revise `core` to be more specific. */
	struct core_info *core = NULL;
	struct core_mng_info *info = NULL;
	struct sppwk_comp_info *comp_info_base = NULL;
	/* TODO(yasufum) revise `core_info` which is same as struct name. */
	struct core_mng_info *core_info = NULL;
	int *change_core = NULL;
	int *change_component = NULL;

	sppwk_get_mng_data(NULL, NULL, &comp_info_base, &core_info,
				&change_core, &change_component, NULL);

	switch (wk_action) {
	case SPPWK_ACT_START:
		info = (core_info + lcore_id);
		if (info->status == SPP_CORE_UNUSE) {
			RTE_LOG(ERR, WK_CMD_RUNNER, "Core %d is not available because "
				"it is in SPP_CORE_UNUSE state.\n", lcore_id);
			return SPP_RET_NG;
		}

		comp_lcore_id = sppwk_get_lcore_id(name);
		if (comp_lcore_id >= 0) {
			RTE_LOG(ERR, WK_CMD_RUNNER, "Component name '%s' is already "
				"used.\n", name);
			return SPP_RET_NG;
		}

		comp_lcore_id = get_free_lcore_id();
		if (comp_lcore_id < 0) {
			RTE_LOG(ERR, WK_CMD_RUNNER, "Cannot assign component over the "
				"maximum number.\n");
			return SPP_RET_NG;
		}

		core = &info->core[info->upd_index];

		comp_info = (comp_info_base + comp_lcore_id);
		memset(comp_info, 0x00, sizeof(struct sppwk_comp_info));
		strcpy(comp_info->name, name);
		comp_info->wk_type = wk_type;
		comp_info->lcore_id = lcore_id;
		comp_info->comp_id = comp_lcore_id;

		core->id[core->num] = comp_lcore_id;
		core->num++;
		ret = SPP_RET_OK;
		tmp_lcore_id = lcore_id;
		*(change_component + comp_lcore_id) = 1;
		break;

	case SPPWK_ACT_STOP:
		comp_lcore_id = sppwk_get_lcore_id(name);
		if (comp_lcore_id < 0)
			return SPP_RET_OK;

		comp_info = (comp_info_base + comp_lcore_id);
		tmp_lcore_id = comp_info->lcore_id;
		memset(comp_info, 0x00, sizeof(struct sppwk_comp_info));

		info = (core_info + tmp_lcore_id);
		core = &info->core[info->upd_index];

		/**
		 * TODO(yasufum) check if this ifdef is simply removed by
		 * running other than spp_vf.
		 */
#ifdef SPP_VF_MODULE
		/* initialize classifier information */
		if (comp_info->wk_type == SPPWK_TYPE_CLS)
			init_classifier_info(comp_lcore_id);
#endif /* SPP_VF_MODULE */

		/* The latest lcore is released if worker thread is stopped. */
		ret_del = del_comp_info(comp_lcore_id, core->num, core->id);
		if (ret_del >= 0)
			core->num--;

		ret = SPP_RET_OK;
		*(change_component + comp_lcore_id) = 0;
		break;

	default:  /* Unexpected case. */
		ret = SPP_RET_NG;
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
	RTE_LOG(INFO, WK_CMD_RUNNER, "port count, port_type=%d,"
				" rx=%d, tx=%d\n", rxtx, num_rx, num_tx);
	if (rxtx == SPP_PORT_RXTX_RX)
		num_rx++;
	else
		num_tx++;
	/* Add rx or tx port appointed in port_type. */
	RTE_LOG(INFO, WK_CMD_RUNNER, "Num of ports after count up,"
				" port_type=%d, rx=%d, tx=%d\n",
				rxtx, num_rx, num_tx);
	switch (component_type) {
	case SPPWK_TYPE_FWD:
		if (num_rx > 1 || num_tx > 1)
			return SPP_RET_NG;
		break;

	case SPPWK_TYPE_MRG:
		if (num_tx > 1)
			return SPP_RET_NG;
		break;

	case SPPWK_TYPE_CLS:
		if (num_rx > 1)
			return SPP_RET_NG;
		break;

	case SPPWK_TYPE_MIR:
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
static int
update_port(enum sppwk_action wk_action,
		const struct sppwk_port_idx *port,
		enum spp_port_rxtx rxtx,
		const char *name,
		const struct spp_port_ability *ability)
{
	int ret = SPP_RET_NG;
	int port_idx;
	int ret_del = -1;
	int comp_lcore_id = 0;
	int cnt = 0;
	struct sppwk_comp_info *comp_info = NULL;
	struct sppwk_port_info *port_info = NULL;
	int *nof_ports = NULL;
	struct sppwk_port_info **ports = NULL;
	struct sppwk_comp_info *comp_info_base = NULL;
	int *change_component = NULL;

	comp_lcore_id = sppwk_get_lcore_id(name);
	if (comp_lcore_id < 0) {
		RTE_LOG(ERR, WK_CMD_RUNNER, "Unknown component by port command. "
				"(component = %s)\n", name);
		return SPP_RET_NG;
	}
	sppwk_get_mng_data(NULL, NULL,
			&comp_info_base, NULL, NULL, &change_component, NULL);
	comp_info = (comp_info_base + comp_lcore_id);
	port_info = get_sppwk_port(port->iface_type, port->iface_no);
	if (rxtx == SPP_PORT_RXTX_RX) {
		nof_ports = &comp_info->nof_rx;
		ports = comp_info->rx_ports;
	} else {
		nof_ports = &comp_info->nof_tx;
		ports = comp_info->tx_ports;
	}

	switch (wk_action) {
	case SPPWK_ACT_ADD:
		/* Check if over the maximum num of ports of component. */
		if (check_port_count(comp_info->wk_type, rxtx,
				comp_info->nof_rx,
				comp_info->nof_tx) != SPP_RET_OK)
			return SPP_RET_NG;

		/* Check if the port_info is included in array `ports`. */
		port_idx = get_idx_port_info(port_info, *nof_ports, ports);
		if (port_idx >= SPP_RET_OK) {
			/* registered */
			if (ability->ops == SPPWK_PORT_ABL_OPS_ADD_VLANTAG) {
				while ((cnt < SPP_PORT_ABILITY_MAX) &&
					    (port_info->ability[cnt].ops !=
					    SPPWK_PORT_ABL_OPS_ADD_VLANTAG))
					cnt++;
				if (cnt >= SPP_PORT_ABILITY_MAX) {
					RTE_LOG(ERR, WK_CMD_RUNNER, "update VLAN tag "
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

		if (*nof_ports >= RTE_MAX_ETHPORTS) {
			RTE_LOG(ERR, WK_CMD_RUNNER, "Cannot assign port over the "
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
				RTE_LOG(ERR, WK_CMD_RUNNER,
						"No space of port ability.\n");
				return SPP_RET_NG;
			}
			memcpy(&port_info->ability[cnt], ability,
					sizeof(struct spp_port_ability));
		}

		port_info->iface_type = port->iface_type;
		ports[*nof_ports] = port_info;
		(*nof_ports)++;

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

		ret_del = delete_port_info(port_info, *nof_ports, ports);
		if (ret_del == 0)
			(*nof_ports)--; /* If deleted, decrement number. */

		ret = SPP_RET_OK;
		break;

	default:  /* This case cannot be happend without invlid wk_action. */
		return SPP_RET_NG;
	}

	*(change_component + comp_lcore_id) = 1;
	return ret;
}

/* Activate temporarily stored command. */
static int
flush_cmd(void)
{
	int ret;
	struct cancel_backup_info *backup_info = NULL;

	sppwk_get_mng_data(NULL, NULL, NULL,
				NULL, NULL, NULL, &backup_info);

	ret = update_port_info();
	if (ret < SPP_RET_OK)
		return ret;

	/* TODO(yasufum) confirm why no returned value. */
	update_lcore_info();

	/* TODO(yasufum) confirm why no checking for returned value. */
	ret = update_comp_info();

	backup_mng_info(backup_info);
	return ret;
}

/* Iterate classifier_table to create response to status command */
#ifdef SPP_VF_MODULE
static int
_add_classifier_table(
		struct spp_iterate_classifier_table_params *params)
{
	int ret;

	ret = add_classifier_table_val(params);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, WK_CMD_RUNNER, "Cannot iterate classifier_mac_table.\n");
		return SPP_RET_NG;
	}

	return SPP_RET_OK;
}
#endif /* SPP_VF_MODULE */

/* Execute one command. */
static int
exec_one_cmd(const struct sppwk_cmd_attrs *cmd)
{
	int ret;

	RTE_LOG(INFO, WK_CMD_RUNNER, "Exec `%s` cmd.\n",
			sppwk_cmd_type_str(cmd->type));

	switch (cmd->type) {
	case SPPWK_CMDTYPE_CLS_MAC:
	case SPPWK_CMDTYPE_CLS_VLAN:
		ret = update_cls_table(cmd->spec.cls_table.wk_action,
				cmd->spec.cls_table.type,
				cmd->spec.cls_table.vid,
				cmd->spec.cls_table.mac,
				&cmd->spec.cls_table.port);
		if (ret == 0) {
			RTE_LOG(INFO, WK_CMD_RUNNER, "Exec flush.\n");
			ret = flush_cmd();
		}
		break;

	case SPPWK_CMDTYPE_WORKER:
		ret = update_comp(
				cmd->spec.comp.wk_action,
				cmd->spec.comp.name,
				cmd->spec.comp.core,
				cmd->spec.comp.wk_type);
		if (ret == 0) {
			RTE_LOG(INFO, WK_CMD_RUNNER, "Exec flush.\n");
			ret = flush_cmd();
		}
		break;

	case SPPWK_CMDTYPE_PORT:
		RTE_LOG(INFO, WK_CMD_RUNNER, "with action `%s`.\n",
				sppwk_action_str(cmd->spec.port.wk_action));
		ret = update_port(cmd->spec.port.wk_action,
				&cmd->spec.port.port, cmd->spec.port.rxtx,
				cmd->spec.port.name, &cmd->spec.port.ability);
		if (ret == 0) {
			RTE_LOG(INFO, WK_CMD_RUNNER, "Exec flush.\n");
			ret = flush_cmd();
		}
		break;

	default:
		/* Do nothing. */
		ret = SPP_RET_OK;
		break;
	}

	return ret;
}

/* Get error message of parsing from given wk_err_msg object. */
static const char *
get_parse_err_msg(
		const struct sppwk_parse_err_msg *wk_err_msg,
		char *message)
{
	switch (wk_err_msg->code) {
	case SPPWK_PARSE_WRONG_FORMAT:
		sprintf(message, "Wrong message format");
		break;

	case SPPWK_PARSE_UNKNOWN_CMD:
		/* TODO(yasufum) Fix compile err if space exists before "(" */
		sprintf(message, "Unknown command(%s)", wk_err_msg->details);
		break;

	case SPPWK_PARSE_NO_PARAM:
		sprintf(message, "No or insufficient number of params (%s)",
				wk_err_msg->msg);
		break;

	case SPPWK_PARSE_INVALID_TYPE:
		sprintf(message, "Invalid value type (%s)",
				wk_err_msg->msg);
		break;

	case SPPWK_PARSE_INVALID_VALUE:
		sprintf(message, "Invalid value (%s)", wk_err_msg->msg);
		break;

	default:
		sprintf(message, "Failed to parse with unexpected reason");
		break;
	}

	return message;
}

/* Setup cmd_result with given code and message. */
static inline void
set_cmd_result(struct cmd_result *cmd_res,
		int code, const char *error_messege)
{
	cmd_res->code = code;
	/**
	 * TODO(yasufum) confirm these string "success", "error" or "invalid"
	 * should be fixed or not because this no meaning short message is
	 * obvious from code and nouse actually.
	 */
	switch (code) {
	case CMD_SUCCESS:
		strcpy(cmd_res->result, "success");
		memset(cmd_res->err_msg, 0x00, CMD_ERR_MSG_LEN);
		break;
	case CMD_FAILED:
		strcpy(cmd_res->result, "error");
		strcpy(cmd_res->err_msg, error_messege);
		break;
	case CMD_INVALID:
	default:
		strcpy(cmd_res->result, "invalid");
		memset(cmd_res->err_msg, 0x00, CMD_ERR_MSG_LEN);
		break;
	}
}

/* Setup error message of parsing for requested command. */
static void
prepare_parse_err_msg(struct cmd_result *results,
		const struct sppwk_cmd_req *request,
		const struct sppwk_parse_err_msg *wk_err_msg)
{
	int i;
	const char *tmp_buff;
	char error_messege[CMD_ERR_MSG_LEN];

	for (i = 0; i < request->nof_cmds; i++) {
		if (wk_err_msg->code == 0)
			set_cmd_result(&results[i], CMD_SUCCESS, "");
		else
			set_cmd_result(&results[i], CMD_INVALID, "");
	}

	if (wk_err_msg->code != 0) {
		tmp_buff = get_parse_err_msg(wk_err_msg, error_messege);
		set_cmd_result(&results[request->nof_valid_cmds],
				CMD_FAILED, tmp_buff);
	}
}

/* Add entry of client ID to a response in JSON. */
static int
add_client_id(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	return append_json_int_value(output, name, sppwk_get_client_id());
}

/* Add entry of port to a response in JSON such as "phy:0". */
static int
add_interface(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	int ret = SPP_RET_NG;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, WK_CMD_RUNNER,
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

	ret = append_json_array_brackets(output, name, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

#ifdef SPP_VF_MODULE
/**
 * Add entries of classifier table in JSON. Before iterating the entries,
 * this function calls several nested functions.
 *   add_classifier_table()  // This function.
 *     -> _add_classifier_table()  // Wrapper and doesn't almost nothing.
 *       -> add_classifier_table_val()  // Setup data and call iterator.
 *         -> iterate_adding_mac_entry()
 */
static int
add_classifier_table(const char *name, char **output,
		void *tmp __attribute__ ((unused)))
{
	int ret = SPP_RET_NG;
	struct spp_iterate_classifier_table_params itr_params;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, WK_CMD_RUNNER,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = %s)\n",
				name);
		return SPP_RET_NG;
	}

	itr_params.output = tmp_buff;
	itr_params.element_proc = append_classifier_element_value;

	ret = _add_classifier_table(&itr_params);
	if (unlikely(ret != SPP_RET_OK)) {
		spp_strbuf_free(itr_params.output);
		return SPP_RET_NG;
	}

	ret = append_json_array_brackets(output, name, itr_params.output);
	spp_strbuf_free(itr_params.output);
	return ret;
}
#endif /* SPP_VF_MODULE */

/**
 * List of combination of tag and operator function. It is used to assemble
 * a result of command in JSON like as following.
 *
 *     {
 *         "client-id": 1,
 *         "ports": ["phy:0", "phy:1", "vhost:0", "ring:0"],
 *         "components": [
 *             {
 *                 "core": 2,
 *                 ...
 */
struct cmd_response response_info_list[] = {
	{ "client-id", add_client_id },
	{ "phy", add_interface },
	{ "vhost", add_interface },
	{ "ring", add_interface },
	{ "master-lcore", add_master_lcore},
	{ "core", add_core},
#ifdef SPP_VF_MODULE
	{ "classifier_table", add_classifier_table},
#endif /* SPP_VF_MODULE */
	{ "", NULL }
};

/* append a list of status information for JSON format. */
static int
append_info_value(const char *name, char **output)
{
	int ret = SPP_RET_NG;
	char *tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, WK_CMD_RUNNER,
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

	ret = append_json_block_brackets(output, name, tmp_buff);
	spp_strbuf_free(tmp_buff);
	return ret;
}

/* send response for decode error */
static void
send_decode_error_response(int *sock,
		const struct sppwk_cmd_req *request,
		struct cmd_result *cmd_results)
{
	int ret = SPP_RET_NG;
	char *msg, *tmp_buff;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		/* TODO(yasufum) refactor no meaning err msg */
		RTE_LOG(ERR, WK_CMD_RUNNER, "allocate error. "
				"(name = decode_error_response)\n");
		return;
	}

	/* create & append result array */
	ret = append_command_results_value("results", &tmp_buff,
			request->nof_cmds, cmd_results);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(tmp_buff);
		RTE_LOG(ERR, WK_CMD_RUNNER,
				"Failed to make command result response.\n");
		return;
	}

	msg = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(msg == NULL)) {
		spp_strbuf_free(tmp_buff);
		/* TODO(yasufum) refactor no meaning err msg */
		RTE_LOG(ERR, WK_CMD_RUNNER, "allocate error. "
				"(name = decode_error_response)\n");
		return;
	}
	ret = append_json_block_brackets(&msg, "", tmp_buff);
	spp_strbuf_free(tmp_buff);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(msg);
		RTE_LOG(ERR, WK_CMD_RUNNER,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}

	RTE_LOG(DEBUG, WK_CMD_RUNNER,
			"Make command response (decode error). "
			"response_str=\n%s\n", msg);

	/* send response to requester */
	ret = send_ctl_msg(sock, msg, strlen(msg));
	if (unlikely(ret != SPP_RET_OK)) {
		RTE_LOG(ERR, WK_CMD_RUNNER,
				"Failed to send decode error response.\n");
		/* not return */
	}

	spp_strbuf_free(msg);
}

/* Send the result of command to spp-ctl. */
static void
send_result_spp_ctl(int *sock,
		const struct sppwk_cmd_req *request,
		struct cmd_result *cmd_results)
{
	int ret = SPP_RET_NG;
	char *msg, *tmp_buff;
	tmp_buff = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(tmp_buff == NULL)) {
		RTE_LOG(ERR, WK_CMD_RUNNER,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}

	/* create & append result array */
	ret = append_command_results_value("results", &tmp_buff,
			request->nof_cmds, cmd_results);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(tmp_buff);
		RTE_LOG(ERR, WK_CMD_RUNNER,
				"Failed to make command result response.\n");
		return;
	}

	/* append client id information value */
	if (request->is_requested_client_id) {
		ret = add_client_id("client_id", &tmp_buff, NULL);
		if (unlikely(ret < SPP_RET_OK)) {
			spp_strbuf_free(tmp_buff);
			RTE_LOG(ERR, WK_CMD_RUNNER, "Failed to make "
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
			RTE_LOG(ERR, WK_CMD_RUNNER,
					"Failed to make status response.\n");
			return;
		}
	}

	msg = spp_strbuf_allocate(CMD_RES_BUF_INIT_SIZE);
	if (unlikely(msg == NULL)) {
		spp_strbuf_free(tmp_buff);
		RTE_LOG(ERR, WK_CMD_RUNNER,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}
	ret = append_json_block_brackets(&msg, "", tmp_buff);
	spp_strbuf_free(tmp_buff);
	if (unlikely(ret < SPP_RET_OK)) {
		spp_strbuf_free(msg);
		RTE_LOG(ERR, WK_CMD_RUNNER,
				/* TODO(yasufum) refactor no meaning err msg */
				"allocate error. (name = result_response)\n");
		return;
	}

	RTE_LOG(DEBUG, WK_CMD_RUNNER,
			"Make command response (command result). "
			"response_str=\n%s\n", msg);

	/* send response to requester */
	ret = send_ctl_msg(sock, msg, strlen(msg));
	if (unlikely(ret != SPP_RET_OK)) {
		RTE_LOG(ERR, WK_CMD_RUNNER,
			"Failed to send command result response.\n");
		/* not return */
	}

	spp_strbuf_free(msg);
}

/* Execute series of commands. */
static int
exec_cmds(int *sock, const char *req_str, size_t req_str_len)
{
	int ret = SPP_RET_NG;
	int i;

	struct sppwk_cmd_req cmd_req;
	struct sppwk_parse_err_msg wk_err_msg;
	struct cmd_result cmd_results[SPPWK_MAX_CMDS];

	memset(&cmd_req, 0, sizeof(struct sppwk_cmd_req));
	memset(&wk_err_msg, 0, sizeof(struct sppwk_parse_err_msg));
	memset(cmd_results, 0, sizeof(cmd_results));

	/* Parse request message. */
	RTE_LOG(DEBUG, WK_CMD_RUNNER, "Parse cmds, %.*s\n",
			(int)req_str_len, req_str);
	ret = sppwk_parse_req(&cmd_req, req_str, req_str_len, &wk_err_msg);

	if (unlikely(ret != SPP_RET_OK)) {
		/* Setup and send error response. */
		prepare_parse_err_msg(cmd_results, &cmd_req, &wk_err_msg);
		send_decode_error_response(sock, &cmd_req, cmd_results);
		RTE_LOG(DEBUG, WK_CMD_RUNNER, "Failed to parse cmds.\n");
		return SPP_RET_OK;
	}

	RTE_LOG(DEBUG, WK_CMD_RUNNER,
			"Num of cmds is %d, and valid cmds is %d\n",
			cmd_req.nof_cmds, cmd_req.nof_valid_cmds);

	/* execute commands */
	for (i = 0; i < cmd_req.nof_cmds; ++i) {
		ret = exec_one_cmd(cmd_req.commands + i);
		if (unlikely(ret != SPP_RET_OK)) {
			set_cmd_result(&cmd_results[i], CMD_FAILED,
					"error occur");
			/* Does not execute remaining commands */
			for (++i; i < cmd_req.nof_cmds; ++i)
				set_cmd_result(&cmd_results[i],
					CMD_INVALID, "");
			break;
		}

		set_cmd_result(&cmd_results[i], CMD_SUCCESS, "");
	}

	/* Exec exit command. */
	if (cmd_req.is_requested_exit) {
		set_cmd_result(&cmd_results[0], CMD_SUCCESS, "");
		send_result_spp_ctl(sock, &cmd_req, cmd_results);
		RTE_LOG(INFO, WK_CMD_RUNNER,
				"Process is terminated with exit cmd.\n");
		return SPP_RET_NG;
	}

	/* Send response to spp-ctl. */
	send_result_spp_ctl(sock, &cmd_req, cmd_results);

	RTE_LOG(DEBUG, WK_CMD_RUNNER, "End command request processing.\n");

	return SPP_RET_OK;
}

/* Setup connection for accepting commands from spp-ctl. */
int
sppwk_cmd_runner_conn(const char *ctl_ipaddr, int ctl_port)
{
	return conn_spp_ctl_init(ctl_ipaddr, ctl_port);
}

/* Run command sent from spp-ctl. */
int
sppwk_run_cmd(void)
{
	int ret;
	int msg_ret;

	static int sock = -1;
	static char *msgbuf;

	if (unlikely(msgbuf == NULL)) {
		msgbuf = spp_strbuf_allocate(CMD_REQ_BUF_INIT_SIZE);
		if (unlikely(msgbuf == NULL)) {
			RTE_LOG(ERR, WK_CMD_RUNNER,
					"Cannot allocate memory "
					"for receive data(init).\n");
			return SPP_RET_NG;
		}
	}

	ret = conn_spp_ctl(&sock);

	if (unlikely(ret != SPP_RET_OK))
		return SPP_RET_OK;

	msg_ret = recv_ctl_msg(&sock, &msgbuf);
	if (unlikely(msg_ret <= 0)) {
		if (likely(msg_ret == 0))
			return SPP_RET_OK;
		else if (unlikely(msg_ret == SPP_CONNERR_TEMPORARY))
			return SPP_RET_OK;
		else
			return SPP_RET_NG;
	}

	ret = exec_cmds(&sock, msgbuf, msg_ret);
	spp_strbuf_remove_front(msgbuf, msg_ret);

	return ret;
}
