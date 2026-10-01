# Why OmaWare patches LibVNCClient

OmaWare's console uses LibVNCClient 0.9.15 with two small fixes. Without them, the console breaks when a QEMU VM changes its screen size, which happens at almost every boot (firmware, bootloader, then the OS each set a different resolution). Distributions don't ship these fixes yet, so `scripts/build.sh` builds a private, patched copy of the library.

Both patches change only `src/libvncclient/rfbclient.c`. Upstream base: [LibVNCServer 0.9.15](https://github.com/LibVNC/libvncserver/tree/9b54b1ec32731bd23158ca014dc18014db4194c3), commit `9b54b1ec32731bd23158ca014dc18014db4194c3`.

## Fix 1: screens with ID 0 (upstream fix, backported)

**Symptom:** after a resize, the console stops with `Rect too large: 363x18 at (357,0)`.

**Cause:** when the server announces a new screen size, the client ignored screens whose ID is 0. QEMU uses ID 0, which the [RFB specification](https://github.com/rfbproto/rfbproto/blob/152107db63cd34b3536ad8ddf54a0cfc9017a9f9/rfbproto.rst) allows. So the client kept the old size and rejected drawing outside it.

**Fix:** [`libvncclient-zero-screen-id.patch`](../patches/libvncclient-zero-screen-id.patch) is upstream commit [041ea576](https://github.com/LibVNC/libvncserver/commit/041ea576c3dddd6c7169935aaf8889673024fbfc) by vin, committed by Christian Beier, unchanged. It removes the `screen.id != 0` condition; width and height are still validated. It's not in 0.9.15 or any distribution package yet.

## Fix 2: QEMU's keyboard capability message (OmaWare fix)

**Symptom:** occasionally, right at startup, `Rect too large: 720x400 at (0,0)`.

**Cause:** QEMU confirms its extended keyboard support with a message that carries its current screen size but no pixels. [QEMU](https://github.com/qemu/qemu/blob/v8.2.2/ui/vnc.c#L2085) can send it just before announcing a resize, so the size in it may be larger than the client's current screen. [LibVNCClient](https://github.com/LibVNC/libvncserver/blob/9b54b1ec32731bd23158ca014dc18014db4194c3/src/libvncclient/rfbclient.c#L2537) checks that size against the screen before recognizing the message, and rejects it.

**Fix:** [`libvncclient-qemu-key-ack.patch`](../patches/libvncclient-qemu-key-ack.patch) handles this message together with the other size-less messages, before the size check. Ordinary drawing is still checked. This is OmaWare's own patch and hasn't been submitted upstream yet; the current upstream code still has the problem.

## Testing

[`test-libvncclient-zero-screen.c`](../patches/test-libvncclient-zero-screen.c) sends real protocol bytes to the library through a socket pair and checks the result. CTest runs it as `vnc-resize-compatibility` against the library OmaWare is built with.

| Case | Unpatched 0.9.15 | Fix 1 only | Both fixes |
| --- | --- | --- | --- |
| Resize with screen ID 1, then draw at the new edge | Pass | Pass | Pass |
| Same with screen ID 0 | **Fail** | Pass | Pass |
| Screen ID 0 with an invalid zero width is rejected | Pass | Pass | Pass |
| Keyboard message larger than the current screen | **Fail** | **Fail** | Pass |
| Drawing outside the screen after that message is rejected | n/a | n/a | Pass |

With both fixes, repeated real QEMU boots, reconnects and console switches pass in the VM test suites.

## Checksums

`scripts/build.sh` refuses to build if a patch file differs from these:

| Patch | SHA-256 |
| --- | --- |
| `libvncclient-zero-screen-id.patch` | `76034e83a397bca4f9cca2bfdf8bcfcc0c94f891361e9bae30b90f0a6e1e585c` |
| `libvncclient-qemu-key-ack.patch` | `1c5eed6c9af481cc6de9082b9ffee4236c3dc1957fdb640951308bec7256da82` |

## One more pitfall: SASL

LibVNCClient's `rfbClient` struct has a different layout depending on whether the library was built with SASL. Building OmaWare against headers from one build and running it with the other corrupts memory. `build.sh` builds the library without SASL and compiles OmaWare against that same build's headers. Keep them together.

## Updating

When a LibVNCClient release includes both fixes, drop the patches, check that the regression test still passes against the new library, and rerun the VM test suites. Remove each patch as upstream adopts it; never apply it twice.

## License

LibVNCClient is GPL-2.0-or-later. Both patches keep that license and leave the original copyright headers untouched. The upstream license text is in [`patches/COPYING.LibVNCServer`](../patches/COPYING.LibVNCServer). The regression test is OmaWare's own code (GPL-3.0-or-later).
