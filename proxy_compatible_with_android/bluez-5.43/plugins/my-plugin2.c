/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "src/plugin.h"
#include "src/log.h"

////


#include <ctype.h>              // is_digit, is_alpha, to_lower
#include <stdbool.h>            // bool
#include <stdlib.h>             // realloc
#include <unistd.h>
#include <sys/socket.h>
#include <glib.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/sdp.h"
#include "lib/mgmt.h"
#include "lib/bluetooth.h"
#include "src/adapter.h"
#include "src/shared/mgmt.h"
#include "src/eir.h"            // EIR_*, eir_data
#include "src/shared/mainloop.h"
#include "src/shared/io.h"

//#include "src/shared/mgmt.h"
//--------------------------------------------------------------------------
// Throw event
//--------------------------------------------------------------------------
struct mgmt {
	int ref_count;
	int fd;
	bool close_on_unref;
	struct io *io;
	bool writer_active;
	struct queue *request_queue;
	struct queue *reply_queue;
	struct queue *pending_list;
	struct queue *notify_list;
	unsigned int next_request_id;
	unsigned int next_notify_id;
	bool need_notify_cleanup;
	bool in_notify;
	void *buf;
	uint16_t len;
	mgmt_debug_func_t debug_callback;
	mgmt_destroy_func_t debug_destroy;
	void *debug_data;
};
struct io {
	int ref_count;
	int fd;
	uint32_t events;
	bool close_on_destroy;
	io_callback_func_t read_callback;
	io_destroy_func_t read_destroy;
	void *read_data;
	io_callback_func_t write_callback;
	io_destroy_func_t write_destroy;
	void *write_data;
	io_callback_func_t disconnect_callback;
	io_destroy_func_t disconnect_destroy;
	void *disconnect_data;
};
struct command_test_data {
	uint16_t opcode;
	uint16_t index;
	uint16_t length;
	const void *param;
	const void *cmd_data;
	uint16_t cmd_size;
	const void *rsp_data;
	uint16_t rsp_size;
	uint8_t rsp_status;
};
//--------------------------------------------------------------------------
// Copied from adapter.c
//--------------------------------------------------------------------------

struct btd_adapter {
	int ref_count;

	uint16_t dev_id;
	struct mgmt *mgmt;

	bdaddr_t bdaddr;		/* controller Bluetooth address */
	uint32_t dev_class;		/* controller class of device */
	char *name;			/* controller device name */
	char *short_name;		/* controller short name */
	uint32_t supported_settings;	/* controller supported settings */
	uint32_t current_settings;	/* current controller settings */

	char *path;			/* adapter object path */
	uint8_t major_class;		/* configured major class */
	uint8_t minor_class;		/* configured minor class */
	char *system_name;		/* configured system name */
	char *modalias;			/* device id (modalias) */
	bool stored_discoverable;	/* stored discoverable mode */
	uint32_t discoverable_timeout;	/* discoverable time(sec) */
	uint32_t pairable_timeout;	/* pairable time(sec) */

	char *current_alias;		/* current adapter name alias */
	char *stored_alias;		/* stored adapter name alias */

	bool discovering;		/* discovering property state */
	bool filtered_discovery;	/* we are doing filtered discovery */
	bool no_scan_restart_delay;	/* when this flag is set, restart scan
					 * without delay */
	uint8_t discovery_type;		/* current active discovery type */
	uint8_t discovery_enable;	/* discovery enabled/disabled */
	bool discovery_suspended;	/* discovery has been suspended */
	GSList *discovery_list;		/* list of discovery clients */
	GSList *set_filter_list;	/* list of clients that specified
					 * filter, but don't scan yet
					 */
	/* current discovery filter, if any */
	struct mgmt_cp_start_service_discovery *current_discovery_filter;

	GSList *discovery_found;	/* list of found devices */
	guint discovery_idle_timeout;	/* timeout between discovery runs */
	guint passive_scan_timeout;	/* timeout between passive scans */
	guint temp_devices_timeout;	/* timeout for temporary devices */

	guint pairable_timeout_id;	/* pairable timeout id */
	guint auth_idle_id;		/* Pending authorization dequeue */
	GQueue *auths;			/* Ongoing and pending auths */
	bool pincode_requested;		/* PIN requested during last bonding */
	GSList *connections;		/* Connected devices */
	GSList *devices;		/* Devices structure pointers */
	GSList *connect_list;		/* Devices to connect when found */
	struct btd_device *connect_le;	/* LE device waiting to be connected */
	sdp_list_t *services;		/* Services associated to adapter */

	struct btd_gatt_database *database;
	struct btd_advertising *adv_manager;

	gboolean initialized;

	GSList *pin_callbacks;
	GSList *msd_callbacks;

	GSList *drivers;
	GSList *profiles;

	struct oob_handler *oob_handler;

	unsigned int load_ltks_id;
	guint load_ltks_timeout;

	unsigned int confirm_name_id;
	guint confirm_name_timeout;

	unsigned int pair_device_id;
	guint pair_device_timeout;

	unsigned int db_id;		/* Service event handler for GATT db */

	bool is_default;		/* true if adapter is default one */
};

//--------------------------------------------------------------------------
// EIR crafting
//--------------------------------------------------------------------------

static void to_eir_GSList_cb_write_uint128(gpointer item, gpointer pbuffer) {
    // TODO only write if the item is uint128_t

    // Read the string. Each consecutive pair of character corresponds to a byte.
    // bt_string2uuid(*(uuid_t **) pbuffer, item);
    uint128_t u128;
    char    * pc = (char *) item;
    uint8_t * pu = (uint8_t *) &u128;
    while (*pc) {
        // Convert the hexadecimal character (char c) into an hex digit (uint8_t n).
        uint8_t extracted, n = 0;
        for (extracted = 0; extracted < 2 && *pc; pc++) {
            char c = *pc;
            if (isdigit(c)) {
                if (c - '0') n += (c - '0') << (4 * (1 - extracted));
                extracted++;
            } else if (isalpha(c)) {
                if (c - '0') n += (tolower(c) - 'a' + 10) << (4 * (1 - extracted));
                extracted++;
            } // Other characters like '-' are ignored.
        }

        // Write n and move 1 byte forward.
        *pu++ = n;
    }

    // u128 is the big endian representation, so we now put the bytes in the right order.
    // http://stackoverflow.com/questions/8004790/how-to-change-byte-order-of-128-bit-number
    // We could also use *(uint32_t **) pbuffer (see bluetooth.h)
    *(*(uint32_t **) pbuffer)++ = ntohl(((uint32_t *) &u128)[3]);
    *(*(uint32_t **) pbuffer)++ = ntohl(((uint32_t *) &u128)[2]);
    *(*(uint32_t **) pbuffer)++ = ntohl(((uint32_t *) &u128)[1]);
    *(*(uint32_t **) pbuffer)++ = ntohl(((uint32_t *) &u128)[0]);

    // TODO: can we have heterogeneous services (uint16,32,128) in the same list?
    // I suspect that the answer is NO.
    // If we need to pass heterogeneous services, we just sort them by size, and make a list of services per uid size.
}

/*
void to_eir_uint16_write(gpointer item, gpointer pbuffer) {
    // In practice, item is a uint16_t *, pbuffer is an uint8_t **.
    printf("to_eir_uint16_write: begin: @ = %p\n", * (uint8_t **) pbuffer);
    size_t len = sizeof(uint8_t) + sizeof(uint32_t);    // The field contains the type, and the uint16_t.
    *(*(uint8_t **) pbuffer)++ = (uint8_t) len;         // Write size and move 1 byte forward.
    *(*(uint8_t **) pbuffer)++ = EIR_UUID16_SOME;       // Write type and move 1 byte forward.
    memcpy(*pbuffer, item, len);                        // Write value.
    *(uint8_t **) pbuffer += len;                       // Move len byte forward.
    printf("to_eir_uint16_write: end  : @ = %p\n", * (uint8_t **) pbuffer);
}
*/

static void to_eir_string_write(gpointer item, gpointer pbuffer) {
    // In practice, item is a char *, pbuffer is an uint8_t **.
    size_t len = strlen(item);                    // The field contains the type, the string null terminated.
    *(*(uint8_t **) pbuffer)++ = (uint8_t) len + 1;   // Write size and move 1 byte forward. Do not forget the type.
    *(*(uint8_t **) pbuffer)++ = EIR_NAME_SHORT;  // Write type and move 1 byte forward.
    memcpy(*(uint8_t **) pbuffer, item, len);     // Write the string and its ending '\0'.
    *(uint8_t **) pbuffer += len;                 // Move at the end of the string.
}

/**
 * Convert a eir_data structure into a an EIR, parsable by eir_parse.
 * @param eir_data The eir_data structure.
 * @param buffer The output buffer.
 * @param buffer_size The size of the buffer.
 *
 * Example 1: pre-allocated buffer (eventually reallocated if too small)/
 *
 * char buffer[100];
 * size_t size;
 * to_eir(eir_data, &buffer, &size);
 *
 * Example 2: unallocated buffer.
 * size_t size;
 * char * buffer = NULL;
 * to_eir(eir_data, &buffer, &size);
 * free(buffer);
 */

static gpointer to_eir(const struct eir_data * eir_data, gpointer buffer, uint16_t *pbuffer_size) {
    uint16_t eir_size = 0;      // type imposed by mgmt_ev_device_found
    gpointer ret = buffer;
    uint8_t num_services = 0;

    // Pre-requisites:
    //
    // An EIR is made of a sequence of fields.
    //
    // Each field is made as follow:
    // - 1 byte for the size s (uint8_t)
    // - 1 byte for the type   (uint8_t)
    // - (s-1) bytes for the data. Indeed s = data_len + sizeof(size) = data_len + 1
    //
    // If a field start at @, the next field start at @ + s + 1 (because we did not count the type in s).
    //
    // Knowing the length of the whole EIR is sufficient to deduce when to stop to read the EIR buffer.
    // Its ends should match the end of the last field.
    // The order in which the fields appear seems to be irrelevant.
    // Indeed, the type suffices to identify which part of eir_data must be set.
    //
    // Approach:
    //
    // 1) Pass over eir_data to determine the size of the field we have to write.
    // 2) Allocate the buffer to build the EIR.
    // 3) Write the buffer.

    // 1) Determine the size to allocate.
    // For each member of the eir_data structure, we allocated the adequate number of bytes.

    if (eir_data->name) {
        eir_size += 2;                      // size, type
        eir_size += strlen(eir_data->name); // value
    }

    if (eir_data->services) {
        num_services = (uint8_t) g_slist_length(eir_data->services);
        eir_size += 2;                      // size, type
        eir_size += num_services * 128/8;   // values
    }

    // 2) Allocation
    // Now we can allocate/reallocate the buffer to write the EIR into it.

    if (buffer) {
        printf("pre allocated buffer\n");
        if (eir_size > *pbuffer_size) {
            printf("enlarge buffer %zu > %zu\n", eir_size, *pbuffer_size);
            // Enlarge my buffer.
            ret = buffer = realloc(buffer, eir_size);
            *pbuffer_size = eir_size;
        }
    } else {
        // Allocate the buffer
        ret = buffer = malloc(eir_size);
        if (!buffer) {
            return NULL; // Not enough memory
        }
        *pbuffer_size = eir_size;
    }

    // 3) Second pass: write into the buffer the EIR.

    if (eir_data->name) {
        const uint8_t *begin_field = buffer; // DEBUG
        to_eir_string_write(eir_data->name, &buffer);
    }

    if (eir_data->services) {
        const uint8_t *begin_field = buffer; // DEBUG
        // TODO: I assume that we only have services of 128 bits. In pratice I think we should
        // make a list of services of 16 bits, another one of 32 bits, and a last one of 128 bits.
        // Each groups leads to a list. Under my hypothesis I only have one list to write.

        // byte0: size of the array (in bytes)
        // byte1: type of the cell
        printf("num_services = %zu\n", (size_t) num_services);
        *(uint8_t *) buffer++ = num_services * 128/8 + 1; // Size: 128 bits per services + 1 byte for the type.
        *(uint8_t *) buffer++ = EIR_UUID128_ALL;          // Type: our cell is an uint128_t

        // next bytes: the uint128_t value(s)
        g_slist_foreach(eir_data->services, to_eir_GSList_cb_write_uint128, &buffer);

        // to use bt_uuid2string, we have to prepare a struct uuid_t (see src/uuid-helper.c)
        printf("s:%d t:%d v:'%p'\n", begin_field[0], begin_field[1], begin_field+2); // DEBUG
    }

    return ret;
}


//--------------------------------------------------------------------------
// Plugin
//--------------------------------------------------------------------------
struct context {
	GMainLoop *main_loop;
	int fd;
	struct mgmt *mgmt_client;
	guint server_source;
	GList *handler_list;
};

enum action {
	ACTION_PASSED,
	ACTION_IGNORE,
	ACTION_RESPOND,
};

struct handler {
	const void *cmd_data;
	uint16_t cmd_size;
	const void *rsp_data;
	uint16_t rsp_size;
	uint8_t rsp_status;
	bool match_prefix;
	enum action action;
};

static void mgmt_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	g_print("%s%s\n", prefix, str);
}

static void context_quit(struct context *context)
{
	g_main_loop_quit(context->main_loop);
}

static void check_actions(struct context *context, int fd,
					const void *data, uint16_t size)
{
	GList *list;

	for (list = g_list_first(context->handler_list); list;
						list = g_list_next(list)) {
		struct handler *handler = list->data;
		int ret;

		if (handler->match_prefix) {
			if (size < handler->cmd_size)
				continue;
		} else {
			if (size != handler->cmd_size)
				continue;
		}

		if (memcmp(data, handler->cmd_data, handler->cmd_size))
			continue;

		switch (handler->action) {
		case ACTION_PASSED:
			context_quit(context);
			return;
		case ACTION_RESPOND:
			ret = write(fd, handler->rsp_data, handler->rsp_size);
			g_assert(ret >= 0);
			return;
		case ACTION_IGNORE:
			return;
		}
	}

	g_test_message("Command not handled\n");
	g_assert_not_reached();
}

static gboolean server_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct context *context = user_data;
	unsigned char buf[512];
	ssize_t result;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	result = read(fd, buf, sizeof(buf));
	if (result < 0)
		return FALSE;

	check_actions(context, fd, buf, result);

	return TRUE;
}

static struct context *create_context(void)
{
	struct context *context = g_new0(struct context, 1);
	GIOChannel *channel;
	int err, sv[2];

	context->main_loop = g_main_loop_new(NULL, FALSE);
	g_assert(context->main_loop);

	err = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	g_assert(err == 0);

	context->fd = sv[0];
	channel = g_io_channel_unix_new(sv[0]);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	context->server_source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				server_handler, context);
	g_assert(context->server_source > 0);

	g_io_channel_unref(channel);

	context->mgmt_client = mgmt_new(sv[1]);
	g_assert(context->mgmt_client);

	if (g_test_verbose() == TRUE)
		mgmt_set_debug(context->mgmt_client,
					mgmt_debug, "mgmt: ", NULL);

	mgmt_set_close_on_unref(context->mgmt_client, true);

	return context;
}

static void execute_context(struct context *context)
{
	g_main_loop_run(context->main_loop);

	g_list_free_full(context->handler_list, g_free);

	g_source_remove(context->server_source);

	mgmt_unref(context->mgmt_client);

	g_main_loop_unref(context->main_loop);

	g_free(context);
}

static void event_cb(uint16_t index, uint16_t length, const void *param,
							void *user_data)
{
	DBG("NATALYA: INSIDE EVENT CB");
//	struct btd_adapter *adapter = user_data;
	struct context *context = user_data;
	context_quit(context);

}

static void raise_event(gconstpointer data, struct btd_adapter *adapter)
{
	const struct command_test_data *test = data;
	struct context *context = create_context();

	DBG("RAISE EVENT, MGMT_EV_DEVICE_FOUND");

//	mgmt_register(adapter->mgmt, test->opcode, adapter->dev_id,
//						event_cb, adapter, NULL);

	mgmt_register(context->mgmt_client, test->opcode, test->index,
							event_cb, context, NULL);

	g_assert_cmpint(write(context->fd, test->cmd_data, test->cmd_size), ==,
									test->cmd_size);

	execute_context(context);
//	write(adapter->mgmt->io->fd, test->cmd_data, test->cmd_size);
	DBG("END WRITE EVENT, MGMT_EV_DEVICE_FOUND");
//	g_main_loop_run(adapter->mgmt->);

}

static void command_completed(
    uint16_t index,
    uint16_t length,
    const void *param,          // struct mgmt_ev_device_found *, see lib/mgmt.h
    void *user_data)
{
	const struct mgmt_ev_cmd_complete * command = param;
    struct btd_adapter * adapter = user_data;
	DBG("COMMAND COMPLETED, opcode %d", command->opcode);

	if(command->opcode == MGMT_OP_START_DISCOVERY || command->opcode == MGMT_OP_START_SERVICE_DISCOVERY)
	{
		DBG("COMMAND COMPLETED, MGMT_OP_START_DISCOVERY");

	    // Craft an eir_data, use to build the EIR.
	    struct eir_data eir_data;
	    memset(&eir_data, 0, sizeof(eir_data));
	    eir_data.services = g_slist_append(eir_data.services, "0000ffe5-0000-1000-8000-00805f9b34fb");
	    //eir_data.services = g_slist_append(eir_data.services, "12345678-9abc-def0-0fed-cba987654321");
	    eir_data.name = "Natalya-FAKE-LED";

	    gpointer eir = NULL;
	    uint16_t eir_len;
	    eir = to_eir(&eir_data, eir, &eir_len);
	    DBG("NATALYA: eir_len = %d\n", eir_len);
	    DBG("NATALYA: eir     = %p\n", eir);

	    size_t ev_fake_device_len = sizeof(struct mgmt_ev_device_found) + eir_len;
	    DBG("NATALYA: ev_fake_device_len = %zu\n", ev_fake_device_len);
	    struct mgmt_ev_device_found * ev_fake_device = malloc(ev_fake_device_len);
	    memset(ev_fake_device, 0, sizeof(ev_fake_device));
	    str2ba("C4:D9:87:C3:30:E3", &(ev_fake_device->addr.bdaddr));
	    ev_fake_device->addr.type = BDADDR_LE_PUBLIC; // see lib/bluetooth.h
	    ev_fake_device->rssi = -55;

	    ev_fake_device->eir_len = eir_len;
	    //memcpy(&(ev_fake_device->eir), eir, eir_len);
	    unsigned i;
	    uint8_t * pc = eir;
	    for (i = 0; i < eir_len; ++i) {
	 //       DBG("NATALYA: %p [%d] <-- %p [%02x]", ev_fake_device->eir + i, i, pc, *pc);
	        ev_fake_device->eir[i] = *pc++;
	    }


	    struct command_test_data event_device_found = {
	        .opcode   = MGMT_EV_DEVICE_FOUND,
	        .index    = adapter->dev_id, // TODO hardcodé
	        .cmd_data = ev_fake_device,
	        .cmd_size = ev_fake_device_len,
	    };

		raise_event(&event_device_found, adapter);

	    free(eir);
	    //free(ev_fake_device); // TO REMOVE, see natalya_destroy
	}
}

static const char device_found_valid[] = { 0x00, 0x00, 0x01, 0x01, 0xaa, 0x00,
		0x01, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x02, 0x01,
		0x06, 0x0d, 0xff, 0x80, 0x01, 0x02, 0x15, 0x12, 0x34, 0x80,
		0x91, 0xd0, 0xf2, 0xbb, 0xc5, 0x03, 0x02, 0x0f, 0x18 };

static const unsigned char event_index_added[] =
				{ 0x04, 0x00, 0x01, 0x00, 0x00, 0x00 };

static void natalya_callback(
    uint16_t index,
    uint16_t length,
    const void *param,          // struct mgmt_ev_device_found *, see lib/mgmt.h
    void *user_data
) {
    const struct mgmt_ev_discovering * ev = param;
    struct btd_adapter * adapter = user_data;
	DBG("NATALYA: CALLBACK i = %d l = %d p = %p u = %p", index, length, ev, user_data);

    // Craft an eir_data, use to build the EIR.
    struct eir_data eir_data;
    memset(&eir_data, 0, sizeof(eir_data));
    eir_data.services = g_slist_append(eir_data.services, "0000ffe5-0000-1000-8000-00805f9b34fb");
    //eir_data.services = g_slist_append(eir_data.services, "12345678-9abc-def0-0fed-cba987654321");
    eir_data.name = "Natalya-FAKE-LED";

    gpointer eir = NULL;
    uint16_t eir_len;
    eir = to_eir(&eir_data, eir, &eir_len);
    DBG("NATALYA: eir_len = %d\n", eir_len);
    DBG("NATALYA: eir     = %p\n", eir);

    size_t ev_fake_device_len = sizeof(struct mgmt_ev_device_found) + eir_len;
    DBG("NATALYA: ev_fake_device_len = %zu\n", ev_fake_device_len);
    struct mgmt_ev_device_found * ev_fake_device = malloc(ev_fake_device_len);
    memset(ev_fake_device, 0, sizeof(ev_fake_device));
    str2ba("C4:D9:87:C3:30:E3", &(ev_fake_device->addr.bdaddr));
    ev_fake_device->addr.type = BDADDR_LE_PUBLIC; // see lib/bluetooth.h
    ev_fake_device->rssi = -55;

    ev_fake_device->eir_len = eir_len;
    //memcpy(&(ev_fake_device->eir), eir, eir_len);
    unsigned i;
    uint8_t * pc = eir;
    for (i = 0; i < eir_len; ++i) {
 //       DBG("NATALYA: %p [%d] <-- %p [%02x]", ev_fake_device->eir + i, i, pc, *pc);
        ev_fake_device->eir[i] = *pc++;
    }


    struct command_test_data event_device_found = {
        .opcode   = MGMT_EV_INDEX_ADDED,//MGMT_EV_DEVICE_FOUND,
        .index    = adapter->dev_id, // TODO hardcodé
        .cmd_data = event_index_added,//ev_fake_device,
        .cmd_size = sizeof(event_index_added),//ev_fake_device_len,
    };

	raise_event(&event_device_found, adapter);

    free(eir);
    //free(ev_fake_device); // TO REMOVE, see natalya_destroy
}

void natalya_destroy(void * user_data) {
    DBG("NATALYA destroy %p", user_data);
    // TODO free(ev_fake_device);
}

static int my_driver_probe(struct btd_adapter *adapter)
{
    // We require mgmt because we want to tweak some events, so btd_register_* functions do not suffice.
	DBG("NATALYA: MY_DRIVER_PROBE!");
    DBG("NATALYA: hci%d", adapter->dev_id);

    // Whenever a discovery is launched, we must run our callback.
	mgmt_register(
        adapter->mgmt,
        MGMT_EV_DISCOVERING,    // event
        adapter->dev_id,        // index
        natalya_callback,       // callback
        adapter,                // user_data
        NULL //natalya_destroy         // mgmt_destroy_func_t
    );
/*
	mgmt_register(adapter->mgmt,
			MGMT_EV_CMD_COMPLETE,
			adapter->dev_id,        // index
			command_completed,       // callback
			adapter,                // user_data
			NULL //natalya_destroy         // mgmt_destroy_func_t
	);
*/
	return 0;
}

static void my_driver_remove(struct btd_adapter *adapter)
{
	DBG("NATALYA: MY DRIVER REMOVE!");
}

static struct btd_adapter_driver my_plugin_driver = {
	.name = "My driver",
	.probe = my_driver_probe,
	.remove = my_driver_remove,
};

static int my_plugin2_init(void)
{
	DBG("NATALYA: MY_PLUGIN_INIT");
	btd_register_adapter_driver(&my_plugin_driver);
}

static void my_plugin2_exit(void)
{
	DBG("NATALYA: MY_PLUGIN_EXIT");
	btd_unregister_adapter_driver(&my_plugin_driver);
}

BLUETOOTH_PLUGIN_DEFINE(my_plugin2, VERSION,
		BLUETOOTH_PLUGIN_PRIORITY_LOW, my_plugin2_init, my_plugin2_exit)
