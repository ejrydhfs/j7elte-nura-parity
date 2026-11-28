#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dbus/dbus.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/shot.ppm";
    DBusError err; dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!c) { fprintf(stderr, "bus: %s\n", err.message); return 1; }
    int pfd[2];
    if (pipe(pfd) < 0) { perror("pipe"); return 1; }

    DBusMessage *m = dbus_message_new_method_call("org.kde.KWin",
        "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2",
        "CaptureScreen");
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(m, &it);
    const char *screen = argc > 2 ? argv[2] : "DSI-1";
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &screen);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    dbus_message_iter_close_container(&it, &arr);
    int wfd = pfd[1];
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UNIX_FD, &wfd);

    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 15000, &err);
    close(pfd[1]);
    if (!r) { fprintf(stderr, "call: %s\n", err.message); return 1; }
    DBusMessageIter ri;
    dbus_message_iter_init(r, &ri);
    int w = -1, h = -1, stride = -1, fmt = -1;
    if (dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_ARRAY) {
        DBusMessageIter a, e, kv;
        dbus_message_iter_recurse(&ri, &a);
        while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_recurse(&a, &e);
            const char *key = NULL;
            dbus_message_iter_get_basic(&e, &key);
            dbus_message_iter_next(&e);
            dbus_message_iter_recurse(&e, &kv);
            if (key && strcmp(key, "width") == 0) dbus_message_iter_get_basic(&kv, &w);
            else if (key && strcmp(key, "height") == 0) dbus_message_iter_get_basic(&kv, &h);
            else if (key && strcmp(key, "stride") == 0) dbus_message_iter_get_basic(&kv, &stride);
            else if (key && strcmp(key, "format") == 0) dbus_message_iter_get_basic(&kv, &fmt);
            dbus_message_iter_next(&a);
        }
    }
    fprintf(stderr, "w=%d h=%d stride=%d format=%d\n", w, h, stride, fmt);
    if (w <= 0 || h <= 0) return 1;
    if (stride <= 0) stride = w * 4;
    size_t size = (size_t)stride * h;
    unsigned char *buf = malloc(size);
    size_t off = 0;
    for (;;) {
        ssize_t n = read(pfd[0], buf + off, size - off);
        if (n <= 0) break;
        off += n;
        if (off >= size) break;
    }
    fprintf(stderr, "read %zu of %zu\n", off, size);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        unsigned char *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char px[3];
            if (fmt == 16) { px[0]=row[x*4+0]; px[1]=row[x*4+1]; px[2]=row[x*4+2]; } /* RGBX8888 */
            else           { px[0]=row[x*4+2]; px[1]=row[x*4+1]; px[2]=row[x*4+0]; } /* BGRA */
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
    return 0;
}
