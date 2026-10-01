// SPDX-License-Identifier: GPL-3.0-or-later
// Omaware regression fixture: exercise the real decoder through a local socket.
#include <rfb/rfbclient.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static rfbBool allocate(rfbClient *client)
{
    free(client->frameBuffer);
    client->frameBuffer = calloc((size_t)client->width * client->height, 4);
    return client->frameBuffer != NULL;
}

static void put16(uint8_t *buffer, unsigned value)
{
    buffer[0] = value >> 8;
    buffer[1] = value;
}

static void put32(uint8_t *buffer, uint32_t value)
{
    put16(buffer, value >> 16);
    put16(buffer + 2, value);
}

static int check(unsigned id, unsigned screen_width)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return 0;
    rfbClient *client = rfbGetClient(8, 3, 4);
    if (!client) { close(sockets[0]); close(sockets[1]); return 0; }
    client->sock = sockets[0];
    client->width = 640;
    client->height = 480;
    client->canHandleNewFBSize = TRUE;
    client->MallocFrameBuffer = allocate;
    client->readTimeout = 1;
    if (!allocate(client)) {
        rfbClientCleanup(client);
        close(sockets[1]);
        return 0;
    }

    // FramebufferUpdate: one ExtendedDesktopSize rectangle, one screen.
    uint8_t update[36] = {0};
    put16(update + 2, 1);
    put16(update + 8, 720);
    put16(update + 10, 400);
    put32(update + 12, rfbEncodingExtDesktopSize);
    update[16] = 1;
    put32(update + 20, id);
    put16(update + 28, screen_width);
    put16(update + 30, 400);
    int ok = write(sockets[1], update, sizeof update) == sizeof update;
    ok = ok && WaitForMessage(client, 1000000) > 0;
    ok = ok && HandleRFBServerMessage(client);
    int expected_width = screen_width ? 720 : 640;
    int expected_height = screen_width ? 400 : 480;
    ok = ok && client->width == expected_width && client->height == expected_height;

    if (ok && screen_width) {
        // A separate update draws into the area outside the old 640px width.
        uint8_t pixel[20] = {0};
        put16(pixel + 2, 1);
        put16(pixel + 4, 719);
        put16(pixel + 6, 399);
        put16(pixel + 8, 1);
        put16(pixel + 10, 1);
        pixel[16] = 0xff;
        ok = write(sockets[1], pixel, sizeof pixel) == sizeof pixel;
        ok = ok && WaitForMessage(client, 1000000) > 0;
        ok = ok && HandleRFBServerMessage(client);
        ok = ok && client->frameBuffer[((size_t)399 * 720 + 719) * 4] == 0xff;
    }

    printf("screen id=%u width=%u: framebuffer=%dx%d expected=%dx%d %s\n",
           id, screen_width, client->width, client->height,
           expected_width, expected_height, ok ? "PASS" : "FAIL");
    free(client->frameBuffer);
    client->frameBuffer = NULL;
    rfbClientCleanup(client);
    close(sockets[1]);
    return ok;
}

static int check_extended_key_ack(void)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return 0;
    rfbClient *client = rfbGetClient(8, 3, 4);
    if (!client) { close(sockets[0]); close(sockets[1]); return 0; }
    client->sock = sockets[0];
    client->width = 640;
    client->height = 480;
    client->readTimeout = 1;
    client->MallocFrameBuffer = allocate;
    if (!allocate(client)) {
        rfbClientCleanup(client);
        close(sockets[1]);
        return 0;
    }

    // QEMU can advertise a new surface's dimensions on its key-event ACK
    // before sending the resize notification. This rectangle has no pixels.
    uint8_t ack[16] = {0};
    put16(ack + 2, 1);
    put16(ack + 8, 720);
    put16(ack + 10, 400);
    put32(ack + 12, rfbEncodingQemuExtendedKeyEvent);
    int ok = write(sockets[1], ack, sizeof ack) == sizeof ack;
    ok = ok && WaitForMessage(client, 1000000) > 0;
    ok = ok && HandleRFBServerMessage(client);
    ok = ok && SupportsClient2Server(client, rfbQemuEvent);
    ok = ok && client->width == 640 && client->height == 480;
    printf("pseudo key ACK 720x400 with framebuffer640x480: %s\n", ok ? "PASS" : "FAIL");

    if (ok) {
        // The pseudo-rectangle exemption must not weaken real pixel bounds.
        uint8_t raw[16] = {0};
        put16(raw + 2, 1);
        put16(raw + 4, 719);
        put16(raw + 6, 399);
        put16(raw + 8, 1);
        put16(raw + 10, 1);
        ok = write(sockets[1], raw, sizeof raw) == sizeof raw;
        ok = ok && WaitForMessage(client, 1000000) > 0;
        ok = ok && !HandleRFBServerMessage(client);
        printf("oversized real pixel rectangle rejected: %s\n", ok ? "PASS" : "FAIL");
    }
    free(client->frameBuffer);
    client->frameBuffer = NULL;
    rfbClientCleanup(client);
    close(sockets[1]);
    return ok;
}

int main(void)
{
    int control = check(1, 720);
    int regression = check(0, 720);
    int invalid = check(0, 0);
    int pseudo = check_extended_key_ack();
    return control && regression && invalid && pseudo ? EXIT_SUCCESS : EXIT_FAILURE;
}
