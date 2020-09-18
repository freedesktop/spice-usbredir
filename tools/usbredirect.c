#include "config.h"
#include <stdio.h>
#include <stdbool.h>
#include <glib.h>
#include <gio/gio.h>
#include <libusb.h>
#include <usbredirhost.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#include <gio/gwin32inputstream.h>
#include <gio/gwin32outputstream.h>
#endif

struct redirect {
    struct {
        int vendor;
        int product;
    } device;
    char *addr;
    int port;

    struct usbredirhost *usbredirhost;
    GSocketConnection *connection;
    GThread *event_thread;
    int event_thread_run;
    int watch_server_id;

    GMainLoop *main_loop;
};

static bool
parse_opt_device(const char *device, int *vendor, int *product)
{
    if (!device) {
        g_warning("No device to redirect. For testing only\n");
        return true;
    }

    char **usbid = g_strsplit(device, ":", 2);
    if (usbid == NULL || usbid[0] == NULL || usbid[1] == NULL || usbid[2] != NULL) {
        g_strfreev(usbid);
        return false;
    }

    *vendor = g_ascii_strtoll(usbid[0], NULL, 16);
    *product = g_ascii_strtoll(usbid[1], NULL, 16);
    g_strfreev(usbid);

    if (*vendor <= 0 || *vendor > 0xffff || *product < 0 || *product > 0xffff) {
        g_printerr("Bad vendor:product values %04x:%04x", *vendor, *product);
        return false;
    }
    return true;
}

static bool
parse_opt_uri(const char *uri, char **adr, int *port)
{
    if (uri == NULL) {
        return false;
    }

    char **parts = g_strsplit(uri, ":", 2);
    if (parts == NULL || parts[0] == NULL || parts[1] == NULL || parts[2] != NULL) {
        g_printerr("Failed to parse '%s' - expected simplified uri scheme: host:port", uri);
        g_strfreev(parts);
        return false;
    }

    *adr = g_strdup(parts[0]);
    *port = g_ascii_strtoll(parts[1], NULL, 10);
    g_strfreev(parts);

    return true;
}

static struct redirect *
parse_opts(int *argc, char ***argv)
{
    char *device = NULL;
    char *remoteaddr = NULL;
    struct redirect *self = NULL;

    GOptionEntry entries[] = {
        { "device", 0, 0, G_OPTION_ARG_STRING, &device, "Local USB device to be redirected", NULL },
        { "to", 0, 0, G_OPTION_ARG_STRING, &remoteaddr, "Client URI to connect to", NULL },
        { NULL }
    };

    GError *err = NULL;
    GOptionContext *ctx = g_option_context_new(NULL);
    g_option_context_add_main_entries(ctx, entries, NULL);
    if (!g_option_context_parse(ctx, argc, argv, &err)) {
        g_printerr("Could not parse arguments: %s\n", err->message);
        g_printerr("%s", g_option_context_get_help(ctx, TRUE, NULL));
        g_clear_error(&err);
        goto end;
    }

    /* check options */

    if (!remoteaddr) {
        g_printerr("%s need to act as tcp client (-to)\n", *argv[0]);
        g_printerr("%s", g_option_context_get_help(ctx, TRUE, NULL));
        goto end;
    }

    self = g_new0(struct redirect, 1);
    if (!parse_opt_device(device, &self->device.vendor, &self->device.product)) {
        g_printerr("Failed to parse device: '%s' - expected: vendor:product\n", device);
        g_clear_pointer(&self, g_free);
        goto end;
    }

    if (!parse_opt_uri(remoteaddr, &self->addr, &self->port)) {
        g_printerr("Failed to parse uri '%s' - expected: addr:port", remoteaddr);
        g_clear_pointer(&self, g_free);
        goto end;
    }

end:
    if (self) {
        g_debug("Device: '%04x:%04x', client connect addr: '%s', port: %d\n",
                self->device.vendor,
                self->device.product,
                self->addr,
                self->port);
    }
    g_free(remoteaddr);
    g_free(device);
    g_option_context_free(ctx);
    return self;
}

static gpointer
thread_handle_libusb_events(gpointer user_data)
{
    struct redirect *self = (struct redirect *) user_data;

    int res = 0;
    const char *desc = "";
    while (g_atomic_int_get(&self->event_thread_run)) {
        res = libusb_handle_events(NULL);
        if (res && res != LIBUSB_ERROR_INTERRUPTED) {
            desc = libusb_strerror(res);
            g_warning("Error handling USB events: %s [%i]", desc, res);
            break;
        }
    }
    if (self->event_thread_run) {
        g_debug("%s: the thread aborted, %s(%d)", __FUNCTION__, desc, res);
    }
    return NULL;
}

static void
usbredir_log_cb(void *priv, int level, const char *msg)
{
    g_printerr("%s\n", msg);
}

static int
usbredir_read_cb(void *priv, uint8_t *data, int count)
{
    struct redirect *self = (struct redirect *) priv;
    GIOStream *iostream = G_IO_STREAM(self->connection);
    GError *err = NULL;

    GPollableInputStream *instream = G_POLLABLE_INPUT_STREAM(g_io_stream_get_input_stream(iostream));
    gssize nbytes = g_pollable_input_stream_read_nonblocking(instream,
            data,
            count,
            NULL,
            &err);
    if (nbytes <= 0) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            /* Try again later */
            nbytes = 0;
        } else {
            if (err != NULL) {
                g_warning("Failure at %s: %s", __func__, err->message);
            }
            g_main_loop_quit(self->main_loop);
        }
        g_clear_error(&err);
    }
    return nbytes;
}

static int
usbredir_write_cb(void *priv, uint8_t *data, int count)
{
    struct redirect *self = (struct redirect *) priv;
    GIOStream *iostream = G_IO_STREAM(self->connection);
    GError *err = NULL;

    GPollableOutputStream *outstream = G_POLLABLE_OUTPUT_STREAM(g_io_stream_get_output_stream(iostream));
    gssize nbytes = g_pollable_output_stream_write_nonblocking(outstream,
            data,
            count,
            NULL,
            &err);
    if (nbytes <= 0) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            /* Try again later */
            nbytes = 0;
        } else {
            if (err != NULL) {
                g_warning("Failure at %s: %s", __func__, err->message);
            }
            g_main_loop_quit(self->main_loop);
        }
        g_clear_error(&err);
    }
    return nbytes;
}

static void
usbredir_write_flush_cb(void *user_data)
{
    struct redirect *self = (struct redirect *) user_data;
    if (!self || !self->usbredirhost) {
        return;
    }

    int ret = usbredirhost_write_guest_data(self->usbredirhost);
    if (ret < 0) {
        g_critical("%s: Failed to write to guest", __func__);
        g_main_loop_quit(self->main_loop);
    }
}

static void
*usbredir_alloc_lock(void)
{
    GMutex *mutex;

    mutex = g_new0(GMutex, 1);
    g_mutex_init(mutex);

    return mutex;
}

static void
usbredir_free_lock(void *user_data)
{
    GMutex *mutex = user_data;

    g_mutex_clear(mutex);
    g_free(mutex);
}

static void
usbredir_lock_lock(void *user_data)
{
    GMutex *mutex = user_data;

    g_mutex_lock(mutex);
}

static void
usbredir_unlock_lock(void *user_data)
{
    GMutex *mutex = user_data;

    g_mutex_unlock(mutex);
}

static gboolean
connection_handle_io_cb(GIOChannel *source, GIOCondition condition, gpointer user_data)
{
    struct redirect *self = (struct redirect *) user_data;

    if (condition & G_IO_ERR || condition & G_IO_HUP) {
        g_warning("Connection: err=%d, hup=%d - exiting", (condition & G_IO_ERR), (condition & G_IO_HUP));
        goto end;
    }

    if (condition & G_IO_IN) {
        int ret = usbredirhost_read_guest_data(self->usbredirhost);
        if (ret < 0) {
            g_critical("%s: Failed to read guest", __func__);
            goto end;
        }
    }
    if (condition & G_IO_OUT) {
        int ret = usbredirhost_write_guest_data(self->usbredirhost);
        if (ret < 0) {
            g_critical("%s: Failed to write to guest", __func__);
            goto end;
        }
    }
    return G_SOURCE_CONTINUE;

end:
    g_main_loop_quit(self->main_loop);
    return G_SOURCE_REMOVE;
}

#ifdef G_OS_UNIX
static gboolean
signal_handler(gpointer user_data)
{
    struct redirect *self = (struct redirect *) user_data;
    g_main_loop_quit(self->main_loop);
    return G_SOURCE_REMOVE;
}
#endif

int
main(int argc, char *argv[])
{
    GError *err = NULL;
    struct redirect *self = parse_opts(&argc, &argv);
    if (!self) {
        /* specific issues logged in parse_opts() */
        return 1;
    }
    if (libusb_init(NULL)) {
        g_warning("Could not init libusb\n");
        goto err_init;
    }

#ifdef G_OS_WIN32
    /* WinUSB is the default by backwards compatibility so this is needed to
     * switch to USBDk backend. */
    libusb_set_option(NULL, LIBUSB_OPTION_USE_USBDK); 
#endif

#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, signal_handler, self);
    g_unix_signal_add(SIGHUP, signal_handler, self);
    g_unix_signal_add(SIGTERM, signal_handler, self);
#endif

    /* This is binary is not meant to support plugins so it is safe to pass
     * NULL as libusb_context here and all subsequent calls */
    libusb_device_handle *device_handle = libusb_open_device_with_vid_pid(NULL,
            self->device.vendor,
            self->device.product);
    if (!device_handle) {
        g_printerr("Failed to open device!\n");
        goto err_init;
    }

    /* As per doc below, we are not using hotplug so we must first call
     * libusb_open() and then we can start the event thread.
     *
     *      http://libusb.sourceforge.net/api-1.0/group__libusb__asyncio.html#eventthread
     *
     * The event thread is a must for Windows while on Unix we would ge okay
     * getting the fds and polling oursevelves. */
    g_atomic_int_set(&self->event_thread_run, TRUE);
    self->event_thread = g_thread_try_new("usbredirect-libusb-event-thread",
            thread_handle_libusb_events,
            self,
            &err);
    if (!self->event_thread) {
        g_warning("Error starting event thread: %s", err->message);
        libusb_close(device_handle);
        goto err_init;
    }

    self->usbredirhost = usbredirhost_open_full(NULL,
            device_handle,
            usbredir_log_cb,
            usbredir_read_cb,
            usbredir_write_cb,
            usbredir_write_flush_cb,
            usbredir_alloc_lock,
            usbredir_lock_lock,
            usbredir_unlock_lock,
            usbredir_free_lock,
            self,
            PACKAGE_STRING,
            usbredirparser_none,
            usbredirhost_fl_write_cb_owns_buffer);
    if (!self->usbredirhost) {
        g_warning("Error starting usbredirhost");
        goto err_init;
    }

    {
        /* Connect to a remote sever using usbredir to redirect the usb device */
        GSocketClient *client = g_socket_client_new();
        self->connection = g_socket_client_connect_to_host(client,
                self->addr,
                self->port, /* your port goes here */
                NULL,
                &err);
        g_object_unref(client);
        if (err != NULL) {
            g_warning("Failed to connect to the server: %s", err->message);
            goto end;
        }

        GSocket *connection_socket = g_socket_connection_get_socket(self->connection);
        int socket_fd = g_socket_get_fd(connection_socket);
        GIOChannel *io_channel =
#ifdef G_OS_UNIX
            g_io_channel_unix_new(socket_fd);
#else
            g_io_channel_win32_new_socket(socket_fd);
#endif
        self->watch_server_id = g_io_add_watch(io_channel,
                G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR,
                connection_handle_io_cb,
                self);
    }

    self->main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(self->main_loop);

    g_atomic_int_set(&self->event_thread_run, FALSE);
    if (self->event_thread) {
        libusb_interrupt_event_handler(NULL);
        g_thread_join(self->event_thread);
        self->event_thread = NULL;
    }

end:
    g_clear_pointer(&self->usbredirhost, usbredirhost_close);
    g_clear_pointer(&self->addr, g_free);
    g_clear_object(&self->connection);
    g_free(self);
err_init:
    libusb_exit(NULL);

    if (err != NULL) {
        g_error_free(err);
        return 1;
    }

    return 0;
}
