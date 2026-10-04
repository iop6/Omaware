// SPDX-License-Identifier: GPL-3.0-or-later
#include <rfb/rfbclient.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static void clipboard(rfbClient *client, const char *text, int length) {
    (void)client;
    (void)text;
    (void)length;
}

int main(void) {
    rfbClient *client = rfbGetClient(8, 3, 4);
    if (!client) return EXIT_FAILURE;

    // LibVNCClient 0.9.15 conditionally includes SASL fields in its PUBLIC
    // struct. Same version/SONAME is insufficient: its generated rfbconfig.h
    // must match the loaded library. Check a library-initialized field beyond
    // those conditional members before installing our callbacks.
    if (client->connectTimeout != DEFAULT_CONNECT_TIMEOUT || client->readTimeout != DEFAULT_READ_TIMEOUT) {
        fprintf(stderr,
                "LibVNCClient header/library layout mismatch: "
                "timeout defaults %u/%u, expected %u/%u. "
                "Rebuild with the headers from the shipped library.\n",
                client->connectTimeout, client->readTimeout, (unsigned)DEFAULT_CONNECT_TIMEOUT,
                (unsigned)DEFAULT_READ_TIMEOUT);
        rfbClientCleanup(client);
        return EXIT_FAILURE;
    }

    // Exercise the same late-struct writes as VncWorker::start. Run this probe
    // under ASan as well: defaults alone are not a general ABI compatibility proof.
    client->GotXCutTextUTF8 = clipboard;
    client->readTimeout = 3;
    printf("VNC client layout smoke test passed: sizeof=%zu, UTF8 offset=%zu\n", sizeof(*client),
            offsetof(rfbClient, GotXCutTextUTF8));
    rfbClientCleanup(client);
    return EXIT_SUCCESS;
}
