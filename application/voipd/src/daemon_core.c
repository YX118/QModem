#define _POSIX_C_SOURCE 200809L

#include "daemon_core.h"
#include "media_serial.h"
#include "sip_activation.h"
#include "sip_gateway.h"

#include <errno.h>
#include <fcntl.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AT_ADAPTER "/usr/lib/qmodem_voip/at_daemon_adapter.sh"
#define SAFETY_HELPER "/usr/sbin/qmodem_voip_modem_safety"
#define SAFETY_JOURNAL "/var/lib/qmodem_voip/modem-safety.journal"
#define SIP_CONFIG QMODEM_VOIP_SIP_CONFIG
#define REGISTRAR_PIDFILE "/var/run/qmodem_voip_registrar.pid"
#define UCI_PROGRAM "/sbin/uci"
#define INIT_PROGRAM "/etc/init.d/qmodem_voip"
#define REGISTRAR_PROGRAM "/usr/bin/qmodem_voip_registrar"

struct qmodem_voip_context qmodem_voip_ctxt;

enum {
	PARAM_ENDPOINT,
	PARAM_NUMBER,
	PARAM_ORIGIN,
	PARAM_USERNAME,
	PARAM_PASSWORD,
	PARAM_MAX
};

enum {
	PARAM_CREDENTIAL_USERNAME,
	PARAM_CREDENTIAL_PASSWORD,
	PARAM_CREDENTIAL_MAX
};

static const struct blobmsg_policy action_policy[PARAM_MAX] = {
	[PARAM_ENDPOINT] = { .name = "endpoint", .type = BLOBMSG_TYPE_STRING },
	[PARAM_NUMBER] = { .name = "number", .type = BLOBMSG_TYPE_STRING },
	[PARAM_ORIGIN] = { .name = "origin", .type = BLOBMSG_TYPE_STRING },
	[PARAM_USERNAME] = { .name = "username", .type = BLOBMSG_TYPE_STRING },
	[PARAM_PASSWORD] = { .name = "password", .type = BLOBMSG_TYPE_STRING }
};

static const struct blobmsg_policy credential_policy[PARAM_CREDENTIAL_MAX] = {
	[PARAM_CREDENTIAL_USERNAME] = { .name = "username", .type = BLOBMSG_TYPE_STRING },
	[PARAM_CREDENTIAL_PASSWORD] = { .name = "password", .type = BLOBMSG_TYPE_STRING }
};

const struct blobmsg_policy qmodem_voip_media_token_policy[QMODEM_VOIP_MEDIA_TOKEN_MAX] = {
	[QMODEM_VOIP_MEDIA_SESSION_ID] = { .name = "session_id", .type = BLOBMSG_TYPE_STRING },
	[QMODEM_VOIP_MEDIA_CALL_REVISION] = { .name = "call_revision", .type = BLOBMSG_TYPE_UNSPEC },
	[QMODEM_VOIP_MEDIA_HTTPS_ORIGIN] = { .name = "https_origin", .type = BLOBMSG_TYPE_STRING }
};

const struct blobmsg_policy qmodem_voip_socket_session_policy[QMODEM_VOIP_SOCKET_SESSION_PARAM_MAX] = {
	[QMODEM_VOIP_SOCKET_SESSION_CALL_REVISION] = { .name = "call_revision",
		.type = BLOBMSG_TYPE_UNSPEC }
};

const struct blobmsg_policy qmodem_voip_rtp_policy[QMODEM_VOIP_RTP_PARAM_MAX] = {
	[QMODEM_VOIP_RTP_PARAM_ADDRESS] = { .name = "address", .type = BLOBMSG_TYPE_STRING },
	[QMODEM_VOIP_RTP_PARAM_PORT] = { .name = "port", .type = BLOBMSG_TYPE_INT32 },
	[QMODEM_VOIP_RTP_PARAM_PAYLOAD_TYPE] = { .name = "payload_type", .type = BLOBMSG_TYPE_INT32 },
	[QMODEM_VOIP_RTP_PARAM_SESSION_ID] = { .name = "session_id", .type = BLOBMSG_TYPE_UNSPEC }
};

static int wait_program(pid_t child)
{
	int status;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int run_at(const char *command)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		execl(AT_ADAPTER, AT_ADAPTER, "at", command, (char *)NULL);
		_exit(127);
	}
	return wait_program(child);
}

static int run_safety(const char *action, int quiet)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		if (quiet) {
			int null_fd = open("/dev/null", O_WRONLY);
			if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
			    dup2(null_fd, STDERR_FILENO) < 0)
				_exit(127);
			if (null_fd > STDERR_FILENO)
				close(null_fd);
		}
		execl(SAFETY_HELPER, SAFETY_HELPER, action, (char *)NULL);
		_exit(127);
	}
	return wait_program(child);
}

static int journal_enabled(void)
{
	char line[128];
	FILE *file = fopen(SAFETY_JOURNAL, "r");
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file))
		if (strcmp(line, "phase=enabled\n") == 0 || strcmp(line, "phase=enabled\r\n") == 0) {
			fclose(file);
			return 1;
		}
	fclose(file);
	return 0;
}

static int registrar_pid(pid_t *process)
{
	return qmodem_voip_sip_pidfile_identity(REGISTRAR_PIDFILE,
		REGISTRAR_PROGRAM, process);
}

static int registrar_live(void *opaque)
{
	pid_t process;
	(void)opaque;
	return registrar_pid(&process) == 0 &&
		(kill(process, 0) == 0 || errno == EPERM);
}

static int reload_registrar(void *opaque)
{
	pid_t process;
	(void)opaque;
	return registrar_pid(&process) == 0 && kill(process, SIGHUP) == 0 ? 0 : -1;
}

static int set_sip_enabled(int enabled)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		char *const arguments[] = { "uci", "set",
			enabled ? "qmodem_voip.sip.enabled=1" : "qmodem_voip.sip.enabled=0", NULL };
		execv(UCI_PROGRAM, arguments);
		_exit(127);
	}
	return wait_program(child);
}

static int commit_sip_config(void)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		char *const arguments[] = { "uci", "commit", "qmodem_voip", NULL };
		execv(UCI_PROGRAM, arguments);
		_exit(127);
	}
	return wait_program(child);
}

static int set_firewall_enabled(int enabled)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		char *const arguments[] = { "uci", "set",
			enabled ? "firewall.qmodem_voip.enabled=1" : "firewall.qmodem_voip.enabled=0", NULL };
		execv(UCI_PROGRAM, arguments);
		_exit(127);
	}
	return wait_program(child);
}

static int commit_firewall_config(void)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		char *const arguments[] = { "uci", "commit", "firewall", NULL };
		execv(UCI_PROGRAM, arguments);
		_exit(127);
	}
	return wait_program(child);
}

static int enable_sip(void *opaque)
{
	(void)opaque;
	if (set_sip_enabled(1) == 0 && set_firewall_enabled(1) == 0 &&
	    commit_sip_config() == 0 && commit_firewall_config() == 0)
		return 0;
	(void)set_sip_enabled(0);
	(void)set_firewall_enabled(0);
	(void)commit_sip_config();
	(void)commit_firewall_config();
	return -1;
}

static void disable_sip(void)
{
	if (set_sip_enabled(0) == 0 && set_firewall_enabled(0) == 0) {
		(void)commit_sip_config();
		(void)commit_firewall_config();
	}
}

static int schedule_registrar_start(void *opaque)
{
	char *const arguments[] = { "qmodem_voip", "reload", NULL };
	(void)opaque;
	return qmodem_voip_sip_run_program(INIT_PROGRAM, arguments);
}

void qmodem_voip_issue_at(const char *command, void *opaque)
{
	struct qmodem_voip_context *context = opaque;
	if (run_at(command) != 0)
		context->command_failed = 1;
}

void qmodem_voip_cancel_serial_prepare(void)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	if (app->media.backend != QMODEM_VOIP_MEDIA_BACKEND_SERIAL)
		return;
	if (atomic_load(&app->media.serial.reopen_pending) &&
	    qmodem_voip_serial_reopen(&app->media) != 0) {
		app->media.ready = 0;
		return;
	}
	qmodem_voip_serial_set_attached(&app->media, 0);
}

void qmodem_voip_add_redacted_status(struct blob_buf *buffer,
					const struct qmodem_voip_call *call)
{
	blobmsg_add_string(buffer, "state", qmodem_voip_state_name(call->state));
	blobmsg_add_string(buffer, "origin",
			   qmodem_voip_endpoint_name(call->origin));
	blobmsg_add_string(buffer, "endpoint",
			   qmodem_voip_endpoint_name(call->endpoint));
	blobmsg_add_u8(buffer, "number_present", call->number[0] != '\0');
	blobmsg_add_u8(buffer, "caller_id_withheld", call->caller_id_withheld);
	blobmsg_add_u8(buffer, "enabled", call->enabled);
	blobmsg_add_u64(buffer, "revision", call->revision);
	blobmsg_add_u64(buffer, "restart_epoch", call->restart_epoch);
	blobmsg_add_u64(buffer, "sequence", call->sequence);
	blobmsg_add_u64(buffer, "drop_count", call->drop_count);
	blobmsg_add_u8(buffer, "reconcile_pending", call->reconcile_pending);
	blobmsg_add_string(buffer, "answer_owner",
			   qmodem_voip_endpoint_name(call->answer_owner));
}

int qmodem_voip_reply_status(struct ubus_context *ubus,
			     struct ubus_request_data *request, int status,
			     const char *error, const char *message)
{
	struct blob_buf buffer = { 0 };
	blob_buf_init(&buffer, 0);
	blobmsg_add_string(&buffer, "status", status == 0 ? "ok" : "error");
	if (error)
		blobmsg_add_string(&buffer, "error", error);
	if (message)
		blobmsg_add_string(&buffer, "message", message);
	(void)ubus_send_reply(ubus, request, buffer.head);
	blob_buf_free(&buffer);
	return status;
}

int qmodem_voip_reply_ok_snapshot(struct ubus_context *ubus,
				  struct ubus_request_data *request)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	struct blob_buf buffer = { 0 };
	blob_buf_init(&buffer, 0);
	blobmsg_add_string(&buffer, "status", "ok");
	qmodem_voip_add_redacted_status(&buffer, &app->call);
	qmodem_voip_media_status(&buffer);
	(void)ubus_send_reply(ubus, request, buffer.head);
	blob_buf_free(&buffer);
	return UBUS_STATUS_OK;
}

static int parse_endpoint(struct blob_attr **parameters,
			  enum qmodem_voip_endpoint *endpoint,
			  int index)
{
	if (!parameters[index] ||
	    qmodem_voip_endpoint_parse(blobmsg_get_string(parameters[index]),
					endpoint) != 0)
		return -1;
	return 0;
}

static int action_call(struct ubus_context *ubus,
		       struct ubus_request_data *request,
		       struct blob_attr *message, const char *action)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	struct blob_attr *parameters[PARAM_MAX] = { 0 };
	enum qmodem_voip_endpoint endpoint;
	const char *number = NULL;
	int result;

	blobmsg_parse(action_policy, PARAM_MAX, parameters,
		      blob_data(message), blob_len(message));
	if (parse_endpoint(parameters, &endpoint, PARAM_ENDPOINT) != 0 ||
	    endpoint == QMODEM_VOIP_ENDPOINT_EXTERNAL_SIP)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_INVALID_ARGUMENT,
				    "invalid_endpoint", "endpoint is unsupported");
	app->command_failed = 0;
	if (strcmp(action, "originate") == 0) {
		if (!parameters[PARAM_NUMBER])
			return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_INVALID_ARGUMENT,
					    "invalid_number", "number is required");
		if (app->call.state != QMODEM_VOIP_IDLE)
			return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_NOT_SUPPORTED,
					    "busy", "another call or answer owner exists");
		if (!journal_enabled() || run_safety("recover", 1) != 0)
			return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_NOT_SUPPORTED,
				"media_not_ready", "modem PCM forwarding could not be armed");
		qmodem_voip_serial_prepare_call(&app->media);
		number = blobmsg_get_string(parameters[PARAM_NUMBER]);
		result = qmodem_voip_originate(&app->call, endpoint, number,
					       qmodem_voip_issue_at, app);
		if (result != 0)
			qmodem_voip_cancel_serial_prepare();
	} else if (strcmp(action, "answer") == 0) {
		if (!journal_enabled() || run_safety("recover", 1) != 0)
			return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_NOT_SUPPORTED,
				"media_not_ready", "modem PCM forwarding could not be armed");
		result = qmodem_voip_answer(&app->call, endpoint, qmodem_voip_issue_at, app);
	} else if (strcmp(action, "reject") == 0) {
		result = qmodem_voip_reject(&app->call, endpoint, qmodem_voip_issue_at, app);
	} else {
		result = qmodem_voip_hangup(&app->call, endpoint, qmodem_voip_issue_at, app);
	}
	if (app->command_failed) {
		qmodem_voip_cancel_serial_prepare();
		app->call.state = QMODEM_VOIP_FAULT;
		qmodem_voip_call_touch(&app->call);
		qmodem_voip_publish_event(&app->call, "fault", app);
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_UNKNOWN_ERROR,
				    "at_failed", "AT command failed");
	}
	if (result == -3)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_NOT_SUPPORTED,
				    "busy", "another call or answer owner exists");
	if (result == -2)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_INVALID_ARGUMENT,
				    "invalid_state", "call is not in this state");
	if (result != 0)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_INVALID_ARGUMENT,
				    strcmp(action, "originate") == 0 ? "invalid_number" : "invalid_endpoint",
				    "call request rejected");
	qmodem_voip_publish_event(&app->call, action, app);
	return qmodem_voip_reply_ok_snapshot(ubus, request);
}

static int action_method(struct ubus_context *ubus,
			 struct ubus_object *object,
			 struct ubus_request_data *request,
			 const char *method, struct blob_attr *message)
{
	(void)object;
	return action_call(ubus, request, message, method);
}

static int status_method(struct ubus_context *ubus, struct ubus_object *object,
			 struct ubus_request_data *request, const char *method,
			 struct blob_attr *message)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	struct blob_buf buffer = { 0 };
	(void)object;
	(void)method;
	(void)message;
	qmodem_voip_media_sync();
	blob_buf_init(&buffer, 0);
	qmodem_voip_add_redacted_status(&buffer, &app->call);
	qmodem_voip_media_status(&buffer);
	(void)ubus_send_reply(ubus, request, buffer.head);
	blob_buf_free(&buffer);
	return UBUS_STATUS_OK;
}

static int capabilities_method(struct ubus_context *ubus,
			       struct ubus_object *object,
			       struct ubus_request_data *request,
			       const char *method, struct blob_attr *message)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	struct blob_buf buffer = { 0 };
	int supported;
	(void)object;
	(void)method;
	(void)message;
	qmodem_voip_media_sync();
	supported = run_safety("probe", 1) == 0;
	blob_buf_init(&buffer, 0);
	blobmsg_add_string(&buffer, "status", "experimental");
	blobmsg_add_u8(&buffer, "supported", supported);
	blobmsg_add_string(&buffer, "support_state",
			   supported ? "supported" : "unsupported");
	blobmsg_add_string(&buffer, "reason",
			   supported ? "probe_passed" : "hardware_probe_failed");
	blobmsg_add_u8(&buffer, "enabled", app->call.enabled);
	blobmsg_add_string(&buffer, "hardware", "RM520N-GL");
	blobmsg_add_string(&buffer, "external_sip", "unsupported");
	qmodem_voip_media_status(&buffer);
	(void)ubus_send_reply(ubus, request, buffer.head);
	blob_buf_free(&buffer);
	return UBUS_STATUS_OK;
}

static int enable_method(struct ubus_context *ubus, struct ubus_object *object,
			 struct ubus_request_data *request, const char *method,
			 struct blob_attr *message)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	int result;
	(void)object;
	(void)method;
	(void)message;
	app->command_failed = 0;
	if (app->call.enabled)
		return qmodem_voip_reply_ok_snapshot(ubus, request);
	if (run_safety("enable", 0) != 0)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_NOT_SUPPORTED,
				    "unsupported", "unsupported or unsafe modem");
	qmodem_voip_call_set_enabled(&app->call, 1);
	if (qmodem_voip_media_engine_start(&app->media) != 0) {
		(void)run_safety("disable", 0);
		qmodem_voip_call_set_enabled(&app->call, 0);
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_NOT_SUPPORTED,
				    "media_not_ready", "selected modem media backend is unavailable");
	}
	app->command_failed = 0;
	result = qmodem_voip_start_recovery(&app->call, qmodem_voip_issue_at, app);
	if (result != 0 || app->command_failed) {
		app->call.state = QMODEM_VOIP_FAULT;
		qmodem_voip_call_touch(&app->call);
		qmodem_voip_publish_event(&app->call, "fault", app);
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_UNKNOWN_ERROR,
				    "at_failed", "startup recovery failed");
	}
	qmodem_voip_publish_event(&app->call, "enabled", app);
	return qmodem_voip_reply_ok_snapshot(ubus, request);
}

static int disable_method(struct ubus_context *ubus, struct ubus_object *object,
			  struct ubus_request_data *request, const char *method,
			  struct blob_attr *message)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	(void)object;
	(void)method;
	(void)message;
	app->command_failed = 0;
	if (app->call.enabled && run_safety("disable", 0) != 0)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_UNKNOWN_ERROR,
				    "restore_failed", "modem restore failed");
	qmodem_voip_call_set_enabled(&app->call, 0);
	qmodem_voip_browser_media_stop(&app->browser);
	qmodem_voip_rtp_release(&app->rtp);
	qmodem_voip_media_release(&app->media);
	qmodem_voip_publish_event(&app->call, "disabled", app);
	return qmodem_voip_reply_ok_snapshot(ubus, request);
}

static void session_access_result(struct ubus_request *request, int type,
				  struct blob_attr *message)
{
	int *allowed = request->priv;
	static const struct blobmsg_policy policy[] = {
		{ .name = "access", .type = BLOBMSG_TYPE_BOOL }
	};
	struct blob_attr *attributes[ARRAY_SIZE(policy)] = { 0 };
	(void)type;
	if (!allowed || !message)
		return;
	blobmsg_parse(policy, ARRAY_SIZE(policy), attributes, blob_data(message), blob_len(message));
	if (attributes[0] && blobmsg_get_bool(attributes[0]))
		*allowed = 1;
}

int qmodem_voip_session_is_authorized(const char *session_id)
{
	struct ubus_context *authorization_ubus;
	struct blob_buf buffer = { 0 };
	uint32_t object;
	int allowed = 0;
	if (!session_id)
		return 0;
	authorization_ubus = ubus_connect(NULL);
	if (!authorization_ubus ||
	    ubus_lookup_id(authorization_ubus, "session", &object) != 0) {
		if (authorization_ubus)
			ubus_free(authorization_ubus);
		return 0;
	}
	blob_buf_init(&buffer, 0);
	blobmsg_add_string(&buffer, "ubus_rpc_session", session_id);
	blobmsg_add_string(&buffer, "scope", "ubus");
	blobmsg_add_string(&buffer, "object", "qmodem_voip");
	blobmsg_add_string(&buffer, "function", "issue_media_token");
	if (ubus_invoke(authorization_ubus, object, "access", buffer.head, session_access_result,
		&allowed, 1000) != 0)
		allowed = 0;
	blob_buf_free(&buffer);
	ubus_free(authorization_ubus);
	return allowed;
}

static int set_sip_credentials_method(struct ubus_context *ubus,
				      struct ubus_object *object,
				      struct ubus_request_data *request,
				      const char *method, struct blob_attr *message)
{
	struct blob_attr *parameters[PARAM_CREDENTIAL_MAX] = { 0 };
	const char *username;
	const char *password;
	struct blob_buf buffer = { 0 };
	(void)object;
	(void)method;
	blobmsg_parse(credential_policy, PARAM_CREDENTIAL_MAX, parameters,
		      blob_data(message), blob_len(message));
	if (!parameters[PARAM_CREDENTIAL_USERNAME] || !parameters[PARAM_CREDENTIAL_PASSWORD])
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_INVALID_ARGUMENT,
				    "invalid_credentials", "username and password are required");
	username = blobmsg_get_string(parameters[PARAM_CREDENTIAL_USERNAME]);
	password = blobmsg_get_string(parameters[PARAM_CREDENTIAL_PASSWORD]);
	if (qmodem_voip_sip_write_credentials(SIP_CONFIG, username, password) != 0)
		return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_INVALID_ARGUMENT,
				    "invalid_credentials", "invalid SIP credentials");
	{
		const struct qmodem_voip_sip_activation_ops activation = {
			.enable = enable_sip,
			.reload = reload_registrar,
			.schedule_start = schedule_registrar_start
		};
		int live = registrar_live(NULL);
		if (qmodem_voip_sip_activate(&activation, NULL, live) < 0) {
			if (!live)
				disable_sip();
			return qmodem_voip_reply_status(ubus, request, UBUS_STATUS_UNKNOWN_ERROR,
					    "activation_failed", "SIP activation could not be scheduled");
		}
	}
	blob_buf_init(&buffer, 0);
	blobmsg_add_u8(&buffer, "configured", 1);
	blobmsg_add_string(&buffer, "username", username);
	blobmsg_add_u8(&buffer, "updated", 1);
	(void)ubus_send_reply(ubus, request, buffer.head);
	blob_buf_free(&buffer);
	return UBUS_STATUS_OK;
}

void qmodem_voip_publish_event(const struct qmodem_voip_call *call,
			       const char *event, void *opaque)
{
	struct qmodem_voip_context *app = opaque;
	struct blob_buf buffer = { 0 };
	(void)app;
	if (call->state == QMODEM_VOIP_ACTIVE &&
	    app->media.backend == QMODEM_VOIP_MEDIA_BACKEND_SERIAL &&
	    qmodem_voip_serial_reopen(&app->media) != 0) {
		app->call.state = QMODEM_VOIP_FAULT;
		qmodem_voip_call_touch(&app->call);
	}
	if ((call->state == QMODEM_VOIP_IDLE ||
	     call->state == QMODEM_VOIP_TERMINATING ||
	     call->state == QMODEM_VOIP_DISABLED ||
	     call->state == QMODEM_VOIP_FAULT) && app->rtp.active)
		qmodem_voip_rtp_release(&app->rtp);
	if ((call->state == QMODEM_VOIP_IDLE ||
	     call->state == QMODEM_VOIP_TERMINATING ||
	     call->state == QMODEM_VOIP_DISABLED ||
	     call->state == QMODEM_VOIP_FAULT) &&
	    app->media.backend == QMODEM_VOIP_MEDIA_BACKEND_SERIAL)
		qmodem_voip_serial_reset_stream(&app->media);
	qmodem_voip_media_sync();
	blob_buf_init(&buffer, 0);
	blobmsg_add_string(&buffer, "event", event);
	qmodem_voip_add_redacted_status(&buffer, call);
	(void)ubus_send_event(app->ubus, "qmodem_voip.call", buffer.head);
	blob_buf_free(&buffer);
}

int qmodem_voip_correlation_parse(const char *value)
{
	if (!value || strcmp(value, "response") == 0)
		return QMODEM_VOIP_CORR_RESPONSE;
	if (strcmp(value, "terminal") == 0)
		return QMODEM_VOIP_CORR_TERMINAL;
	if (strcmp(value, "ambiguous") == 0)
		return QMODEM_VOIP_CORR_AMBIGUOUS;
	return QMODEM_VOIP_CORR_IDLE;
}

void at_line_event(struct ubus_context *ubus,
		   struct ubus_event_handler *handler,
		   const char *type, struct blob_attr *message)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	static const struct blobmsg_policy policy[] = {
		{ .name = "restart_epoch", .type = BLOBMSG_TYPE_INT64 },
		{ .name = "sequence", .type = BLOBMSG_TYPE_INT64 },
		{ .name = "raw_line", .type = BLOBMSG_TYPE_STRING },
		{ .name = "correlation", .type = BLOBMSG_TYPE_STRING },
		{ .name = "command_id", .type = BLOBMSG_TYPE_INT64 },
		{ .name = "drop_count", .type = BLOBMSG_TYPE_INT64 }
	};
	struct blob_attr *values[ARRAY_SIZE(policy)] = { 0 };
	const char *correlation;
	uint64_t command_id = 0;
	(void)handler;
	(void)type;
	blobmsg_parse(policy, ARRAY_SIZE(policy), values, blob_data(message),
		      blob_len(message));
	if (!values[0] || !values[1] || !values[2])
		return;
	if (values[4])
		command_id = blobmsg_get_u64(values[4]);
	correlation = values[3] ? blobmsg_get_string(values[3]) : NULL;
	app->command_failed = 0;
	(void)qmodem_voip_line(&app->call, blobmsg_get_u64(values[0]),
			       blobmsg_get_u64(values[1]), blobmsg_get_string(values[2]),
			       qmodem_voip_correlation_parse(correlation), command_id,
			       values[5] ? blobmsg_get_u64(values[5]) : 0,
			       qmodem_voip_issue_at, qmodem_voip_publish_event, app);
	if (app->command_failed) {
		app->call.state = QMODEM_VOIP_FAULT;
		qmodem_voip_call_touch(&app->call);
		qmodem_voip_publish_event(&app->call, "fault", app);
	}
	(void)ubus;
}

void qmodem_voip_call_timer(struct uloop_timeout *timeout)
{
	struct qmodem_voip_context *app = &qmodem_voip_ctxt;
	(void)timeout;
	if (app->call.enabled &&
	    (app->call.state == QMODEM_VOIP_OUTGOING_SETUP ||
	     app->call.state == QMODEM_VOIP_EARLY_MEDIA)) {
		app->command_failed = 0;
		(void)qmodem_voip_poll_active(&app->call, qmodem_voip_issue_at, app);
		if (app->command_failed) {
			app->call.state = QMODEM_VOIP_FAULT;
			qmodem_voip_call_touch(&app->call);
			qmodem_voip_publish_event(&app->call, "fault", app);
		}
	} else if (app->call.enabled &&
		   (app->call.state == QMODEM_VOIP_ACTIVE ||
		    app->call.state == QMODEM_VOIP_TERMINATING) &&
		   !app->call.reconcile_pending) {
		app->command_failed = 0;
		(void)qmodem_voip_poll_active(&app->call, qmodem_voip_issue_at, app);
		if (app->command_failed) {
			app->call.reconcile_pending = 0;
			app->call.reconcile_command_id = 0;
		}
	}
	uloop_timeout_set(&app->call_timer, 1000);
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", status_method),
	UBUS_METHOD_NOARG("capabilities", capabilities_method),
	UBUS_METHOD_NOARG("enable", enable_method),
	UBUS_METHOD_NOARG("disable", disable_method),
	UBUS_METHOD("originate", action_method, action_policy),
	UBUS_METHOD("answer", action_method, action_policy),
	UBUS_METHOD("reject", action_method, action_policy),
	UBUS_METHOD("hangup", action_method, action_policy),
	UBUS_METHOD("set_sip_credentials", set_sip_credentials_method, credential_policy),
	UBUS_METHOD("issue_media_token", qmodem_voip_media_token_method, qmodem_voip_media_token_policy),
	UBUS_METHOD("issue_socket_session", qmodem_voip_socket_session_method, qmodem_voip_socket_session_policy),
	UBUS_METHOD("attach_rtp", qmodem_voip_rtp_attach_method, qmodem_voip_rtp_policy),
	UBUS_METHOD_NOARG("release_rtp", qmodem_voip_rtp_release_method)
};

struct ubus_object_type qmodem_voip_object_type =
	UBUS_OBJECT_TYPE("qmodem_voip", methods);