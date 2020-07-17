/* Spa oFono backend
 *
 * Copyright © 2020 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/utils/type.h>

#include "defs.h"

struct spa_bt_backend {
	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	unsigned int filters_added:1;
};

#define OFONO_SERVICE "org.ofono"
#define OFONO_HF_AUDIO_MANAGER_INTERFACE OFONO_SERVICE ".HandsfreeAudioManager"
#define OFONO_HF_AUDIO_AGENT_INTERFACE OFONO_SERVICE ".HandsfreeAudioAgent"

#define OFONO_AUDIO_CLIENT	"/Profile/ofono"

#define OFONO_INTROSPECT_XML						    \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
	"<node>"                                                            \
	" <interface name=\"" OFONO_HF_AUDIO_AGENT_INTERFACE "\">"                 \
	"  <method name=\"Release\">"                                       \
	"  </method>"                                                       \
	"  <method name=\"NewConnection\">"                                 \
	"   <arg name=\"card\" direction=\"in\" type=\"o\"/>"             \
	"   <arg name=\"fd\" direction=\"in\" type=\"h\"/>"                 \
	"   <arg name=\"codec\" direction=\"in\" type=\"b\"/>"           \
	"  </method>"                                                       \
	" </interface>"                                                     \
	" <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
	"  <method name=\"Introspect\">"                                    \
	"   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
	"  </method>"                                                       \
	" </interface>"                                                     \
	"</node>"

#define OFONO_ERROR_INVALID_ARGUMENTS "org.ofono.Error.InvalidArguments"
#define OFONO_ERROR_IN_USE "org.ofono.Error.InUse"

static DBusHandlerResult ofono_audio_card_removed(struct spa_bt_backend *backend, const char *path)
{
	spa_log_debug(backend->log, "oFono card removed: %s", path);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult ofono_audio_card_found(struct spa_bt_backend *backend, char *path, DBusMessageIter *props_i)
{
	spa_log_debug(backend->log, "oFono new card: %s", path);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult ofono_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	DBusMessage *r;

	spa_log_warn(backend->log, "oFono release");

	r = dbus_message_new_error(m, OFONO_HF_AUDIO_AGENT_INTERFACE ".Error.NotImplemented",
                                            "Method not implemented");
	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult ofono_new_audio_connection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	const char *path;
	int fd;
	uint8_t codec;
	DBusMessage *r = NULL;

	if (dbus_message_get_args(m, NULL,
	                          DBUS_TYPE_OBJECT_PATH, &path,
	                          DBUS_TYPE_UNIX_FD, &fd,
	                          DBUS_TYPE_BYTE, &codec,
	                          DBUS_TYPE_INVALID) == FALSE) {
		r = dbus_message_new_error(m, OFONO_ERROR_INVALID_ARGUMENTS, "Invalid arguments in method call");
		goto fail;
	}

	spa_log_debug(backend->log, "NewConnection %s, fd %d codec %d", path, fd, codec);

fail:
	if (r) {
			dbus_connection_send(spa_dbus_connection_get(backend->dbus_connection), r, NULL);
			dbus_message_unref(r);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult ofono_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct spa_bt_backend *backend = userdata;
	const char *path, *interface, *member;
	DBusMessage *r;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(backend->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = OFONO_INTROSPECT_XML;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(backend->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_unref(r);
		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, OFONO_HF_AUDIO_AGENT_INTERFACE, "Release"))
		res = ofono_release(c, m, userdata);
	else if (dbus_message_is_method_call(m, OFONO_HF_AUDIO_AGENT_INTERFACE, "NewConnection"))
		res = ofono_new_audio_connection(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void ofono_getcards_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_backend *backend = user_data;
	DBusMessage *r;
	DBusMessageIter i, array_i, struct_i, props_i;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "Failed to get a list of handsfree audio cards from ofono: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

	if (!dbus_message_iter_init(r, &i) || strcmp(dbus_message_get_signature(r), "a(oa{sv})") != 0) {
		spa_log_error(backend->log, "Invalid arguments in GetCards() reply");
		goto finish;
	}

	dbus_message_iter_recurse(&i, &array_i);
	while (dbus_message_iter_get_arg_type(&array_i) != DBUS_TYPE_INVALID) {
			char *path;

			dbus_message_iter_recurse(&array_i, &struct_i);
			dbus_message_iter_get_basic(&struct_i, &path);
			dbus_message_iter_next(&struct_i);

			dbus_message_iter_recurse(&struct_i, &props_i);

			ofono_audio_card_found(backend, path, &props_i);

			dbus_message_iter_next(&array_i);
	}

finish:
	dbus_message_unref(r);
	dbus_pending_call_unref(pending);
}

static void ofono_register_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_backend *backend = user_data;
	DBusMessage *r;
	DBusMessage *m;
	DBusPendingCall *call;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, OFONO_ERROR_INVALID_ARGUMENTS)) {
		spa_log_warn(backend->log, "oFono invalid arguments");
		goto finish;
	}
	if (dbus_message_is_error(r, OFONO_ERROR_IN_USE)) {
		spa_log_warn(backend->log, "oFono already in use");
		goto finish;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(backend->log, "Error registering profile");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "RegisterProfile() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

	spa_log_debug(backend->log, "oFono registered");

	m = dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_HF_AUDIO_MANAGER_INTERFACE, "GetCards");
	if (m == NULL)
		goto finish;

	dbus_connection_send_with_reply(backend->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, ofono_getcards_reply, backend, NULL);
	dbus_message_unref(m);

finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
}

static int ofono_register(struct spa_bt_backend *backend)
{
	DBusMessage *m;
	const char *path = OFONO_AUDIO_CLIENT;
	uint8_t codecs[2];
	const uint8_t *pcodecs = codecs;
	int ncodecs = 0;
	DBusPendingCall *call;

	spa_log_debug(backend->log, "Registering oFono");

	m = dbus_message_new_method_call(OFONO_SERVICE, "/",
			OFONO_HF_AUDIO_MANAGER_INTERFACE, "Register");
	if (m == NULL)
		return -ENOMEM;

	codecs[ncodecs++] = HFP_AUDIO_CODEC_CVSD;

	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path,
                                          DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &pcodecs, ncodecs,
                                          DBUS_TYPE_INVALID);

	dbus_connection_send_with_reply(backend->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, ofono_register_reply, backend, NULL);
	dbus_message_unref(m);

	return 0;
}

static DBusHandlerResult ofono_filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct spa_bt_backend *backend = user_data;
	DBusError err;

	dbus_error_init(&err);

	if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;

		if (!dbus_message_get_args(m, &err,
		                           DBUS_TYPE_STRING, &name,
		                           DBUS_TYPE_STRING, &old_owner,
		                           DBUS_TYPE_STRING, &new_owner,
		                           DBUS_TYPE_INVALID)) {
				spa_log_error(backend->log, "Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
				goto fail;
		}

		if (strcmp(name, OFONO_SERVICE) == 0) {
				if (old_owner && *old_owner) {
						spa_log_debug(backend->log, "oFono disappeared");
						//ofono_bus_id_destroy(backend);
				}

				if (new_owner && *new_owner) {
						spa_log_debug(backend->log, "oFono appeared");
						ofono_register(backend);
				}
		} else {
			spa_log_debug(backend->log, "Name owner changed %s", dbus_message_get_path(m));
		}
	} else if (dbus_message_is_signal(m, OFONO_HF_AUDIO_MANAGER_INTERFACE, "CardAdded")) {
			char *p;
			DBusMessageIter arg_i, props_i;

			if (!dbus_message_iter_init(m, &arg_i) || strcmp(dbus_message_get_signature(m), "oa{sv}") != 0) {
					spa_log_error(backend->log, "Failed to parse org.ofono.HandsfreeAudioManager.CardAdded");
					goto fail;
			}

			dbus_message_iter_get_basic(&arg_i, &p);

			dbus_message_iter_next(&arg_i);
			spa_assert(dbus_message_iter_get_arg_type(&arg_i) == DBUS_TYPE_ARRAY);

			dbus_message_iter_recurse(&arg_i, &props_i);

			return ofono_audio_card_found(backend, p, &props_i);
	} else if (dbus_message_is_signal(m, OFONO_HF_AUDIO_MANAGER_INTERFACE, "CardRemoved")) {
			const char *p;

			if (!dbus_message_get_args(m, &err, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID)) {
					spa_log_error(backend->log, "Failed to parse org.ofono.HandsfreeAudioManager.CardRemoved: %s", err.message);
					goto fail;
			}

			return ofono_audio_card_removed(backend, p);
	}

	return DBUS_HANDLER_RESULT_HANDLED;

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void backend_ofono_add_filters(struct spa_bt_backend *backend)
{
	DBusError err;

	if (backend->filters_added)
		return;

	dbus_error_init(&err);

	if (!dbus_connection_add_filter(backend->conn, ofono_filter_cb, backend, NULL)) {
		spa_log_error(backend->log, "failed to add filter function");
		goto fail;
	}

	dbus_bus_add_match(backend->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" OFONO_SERVICE "'", &err);
	dbus_bus_add_match(backend->conn,
			"type='signal',sender='" OFONO_SERVICE "',"
			"interface='" OFONO_HF_AUDIO_MANAGER_INTERFACE "',member='CardAdded'", &err);
	dbus_bus_add_match(backend->conn,
			"type='signal',sender='" OFONO_SERVICE "',"
			"interface='" OFONO_HF_AUDIO_MANAGER_INTERFACE "',member='CardRemoved'", &err);

	backend->filters_added = true;

	return;

fail:
	dbus_error_free(&err);
}

void backend_ofono_free(struct spa_bt_backend *backend)
{
	dbus_connection_unregister_object_path(backend->conn, OFONO_AUDIO_CLIENT);

	free(backend);
}

struct spa_bt_backend *backend_ofono_new(struct spa_bt_monitor *monitor,
		const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_backend *backend;
	static const DBusObjectPathVTable vtable_profile = {
		.message_function = ofono_handler,
	};

	backend = calloc(1, sizeof(struct spa_bt_backend));
	if (backend == NULL)
		return NULL;

	backend->monitor = monitor;
	backend->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	backend->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	backend->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);

	backend->dbus_connection = spa_dbus_get_connection(backend->dbus, SPA_DBUS_TYPE_SYSTEM);
	if (backend->dbus_connection == NULL) {
		spa_log_error(backend->log, "no dbus connection");
    free(backend);
		return NULL;
	}
	backend->conn = spa_dbus_connection_get(backend->dbus_connection);

	if (!dbus_connection_register_object_path(backend->conn,
						  OFONO_AUDIO_CLIENT,
						  &vtable_profile, backend)) {
    free(backend);
		return NULL;
  }

  ofono_register(backend);

	return backend;
}
