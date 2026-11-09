# BusyBox — Alpine edge patch set adoption

Port report for the migration from a hand-maintained patch set (4 inline
seds + 5 Debian CVE backports) to the Alpine edge patch set, while
preserving the gateway's curated applet selection.

**Ported:** 2026-04-17, on BusyBox 1.37.0 with 24 patches (17 Alpine + 3 CVE
supplements + 4 Lexra).
**Current:** 2026-09-03, BusyBox **1.38.0** with **19 patches** (15 Alpine + 4
Lexra). Upstream absorbed the three CVE supplements and four of the seventeen
Alpine patches; Alpine contributed two new `ash` fixes. See the review log.

---

## Upstream review log

Because the patch set tracks Alpine edge, "is our BusyBox stale" is answered by
comparing version and patch list against aports, not against upstream release
announcements. Reviews are recorded here so the next one starts from a date.

| Reviewed | Alpine edge then | Verdict |
|---|---|---|
| 2026-09-03 | 1.38.0-r4, 40 patches | **Bump to 1.38.0** — done the same day. Upstream moved; the port had pinned 1.37.0 four weeks before 1.38.0 was released. |
| 2026-11-09 | 1.38.0-r8, 42 patches | **No change.** Upstream still at 1.38.0; none of Alpine's four commits since r4 touches an applet or option this build ships. |

Detail for 2026-11-09. Alpine's four commits on `main/busybox` since r4, each
checked against `busybox.config`:

- r5, `0041-less-Support-the-T-option` — `less` accepts and ignores `-T`, for
  mandoc. `CONFIG_LESS` is not set here.
- r6, `0042-modprobe-Check-for-ELF-header-to-determine-if-module` — `modprobe`
  checks for an ELF header before asking the kernel to decompress a module.
  `CONFIG_MODPROBE` and `CONFIG_INSMOD` are not set, and the kernel is built
  without module support (`# CONFIG_MODULES is not set`).
- r7, `CONFIG_CTTYHACK=y` in Alpine's `busyboxconfig` — a configuration choice,
  not a fix; the curated applet set here is left as it is.
- r8, help-text fix inside Alpine's `0021-udhcpc-set-default-discover-retries-to-5`
  (the `-t` and `-T` defaults were swapped in the usage string). 0021 is among the
  patches not imported (see below); `S10network` passes `-T 3` explicitly.

No new CVE backport and no new `ash` fix, so the 15 imported patches are still
Alpine's current set for what this build ships.

Detail for 2026-09-03. BusyBox 1.38.0 was released upstream on 2026-05-13 and
Alpine moved to it on 2026-05-19 — a month after this port pinned 1.37.0, which
at the time was what Alpine shipped. Four of the seventeen patches imported here
are absent from Alpine's 1.38.0 set, upstream having absorbed them: the tar
CVE-2025-46394 masking fix, `ash-reject-unknown-long-options`, and both
`lineedit` fixes. Alpine added two `ash` fixes after this port that exist in no
1.37.0 tree: `ash-clear-bb_got_signal-for-non-interactive-shell`, backported
from OpenWRT on 2026-06-14, and `ash-fix-out-of-bounds-read-in-ifsbreakup` on
2026-07-22 — a genuine stack over-read, where `ifsfree()` is skipped because
`argstr()` longjmps out of `expandarg()` on an expansion error, leaving stale
IFS split offsets that a later, shorter expansion then trusts. Note this is an
upstream robustness fix in the expansion path and not a return of the historical
ash fault class on this platform, which was root-caused elsewhere, in
`local_flush_tlb_all()`. Everything else new in Alpine's set falls under the
exclusions already listed below: Alpine infrastructure, applet install paths,
testsuite, and one x86 workaround.

Unlike the toolchain, a BusyBox bump does not re-lay the kernel text, so it costs
no performance bench. The work is the configuration: `build_busybox.sh` resolves
a new version's Kconfig with `yes "" | make oldconfig` and rewrites
`busybox.config` in place, which accepts every new option at its default without
asking — so the applet set must be diffed before and after, not assumed.

---

## Rationale

Same philosophy as the toolchain migration (GCC / binutils / musl moved
to Alpine edge in v3.0.0): inherit Alpine's curated patch stream — CVE
backports, musl-first fixes, hardening defaults — without depending on
Alpine infrastructure (`utmps`, external `ssl_client`, multi-variant
builds). Drop the locally maintained patches whose fixes are now carried
upstream (or by Alpine), keep only what remains truly gateway-specific.

---

## Patches imported from Alpine (15)

Alpine ships **40 patches** for BusyBox 1.38.0. We keep the subset that
is portable to our target (MIPS big-endian, musl, crosstool-NG build)
and does not require Alpine-specific infrastructure.

| # | File (renumbered) | Upstream origin | Why we took it |
|---|-------------------|-----------------|----------------|
| 001 | alpine-awk-fix-handling-of-literal-backslashes-in-replaceme | upstream bug fix | awk replacement string regression |
| 002 | alpine-hexdump-fix-regression-with-n4-e-u | upstream bug fix | hexdump `-n4 -e '"%u"'` behavior regression |
| 003 | alpine-mount-fix-parsing-proc-mounts-with-long-lines | upstream bug fix | `mount` crashed on `/proc/mounts` lines > 256 chars |
| 004 | alpine-tar-fix-TOCTOU-symlink-race-condition | security | `O_NOFOLLOW` for regular file writes during tar extraction |
| 005 | alpine-tunctl-fix-segfault-on-ioctl-failure | upstream bug fix | NULL fmt-string deref on ioctl() error path |
| 006 | alpine-wget-add-header-Accept | hardening/compat | some CDNs reject requests without an `Accept:` header |
| 007 | alpine-libbb-sockaddr2str-ensure-only-printable-characters | hardening | defense in depth against terminal-escape injection |
| 008 | alpine-nslookup-sanitize-all-printed-strings-with-printable | hardening | same as 007, applied in nslookup |
| 009 | alpine-ping-make-ping-work-without-root-privileges | hardening | `IPPROTO_ICMP DGRAM` socket → no SUID root required |
| 010 | alpine-find-fix-xdev-depth-and-delete | upstream bug fix | `find -xdev -depth -delete` crossed filesystem boundaries |
| 011 | alpine-awk.c-fix-CVE-2023-42366-bug-15874 | CVE-2023-42366 | awk OOB write on crafted input (heap corruption) |
| 012 | alpine-syslogd-fix-wrong-OPT_locallog-flag-detection | upstream bug fix | `-l` flag (local log) was never detected |
| 013 | alpine-Fix-CVE-2024-58251-sanitize-process-names-when-calli | CVE-2024-58251 | netstat printed non-printable chars from `/proc/PID/comm` (Alpine renamed this patch at 1.38.0; same fix as the former 004) |
| 014 | alpine-ash-clear-bb_got_signal-for-non-interactive-shell | upstream bug fix | new at 1.38.0 — backported by Alpine from OpenWRT on 2026-06-14 |
| 015 | alpine-ash-fix-out-of-bounds-read-in-ifsbreakup | security | new at 1.38.0 — stack over-read: `ifsfree()` is skipped when `argstr()` longjmps out of `expandarg()`, leaving stale IFS split offsets a later, shorter expansion then trusts |

Four patches carried at 1.37.0 are gone, absorbed by upstream 1.38.0 and
absent from Alpine's set: the tar CVE-2025-46394 masking fix,
`ash-reject-unknown-long-options`, and both `lineedit` fixes.

Naming: Alpine filenames are kept as-is (minus the `0001-…0035-`
prefix, which is Alpine-internal ordering). Our `001-015` prefix
reflects the application order, and preserves Alpine's relative order.

---

## Alpine patches NOT imported (25 of 40)

Evaluated and dropped — they either target infrastructure we don't ship or
change behaviour in ways that would surprise users of the curated rootfs. The
filenames are Alpine's own at 1.38.0; the criteria are unchanged since the
2026-04-17 port, and re-applying them to the newer set is what keeps this list
a decision record rather than an inventory.

### Architecture-specific (1)

| Alpine file | Why dropped |
|-------------|-------------|
| 0030-Hackfix-to-disable-HW-acceleration-for-MD5-SHA1-on-x | x86-only; our target is big-endian MIPS |

### Alpine-infra dependencies (3)

| Alpine file | Why dropped |
|-------------|-------------|
| 0001-ash-exec-busybox.static | requires the separate `busybox.static` subpackage |
| 0012-Avoid-redefined-warnings-when-buiding-with-utmps | requires `libutmps`/`utmps-dev`, not shipped on the gateway |
| 0017-properly-fix-wget-https-support | relies on the external `ssl_client` binary; we don't ship it |

### Testsuite-only (4)

The gateway build doesn't run `make test`; these only fix test-runner quirks on
Alpine's CI.

| Alpine file | Why dropped |
|-------------|-------------|
| 0028-tests-fix-tarball-creation | testsuite only |
| 0029-tests-musl-doesn-t-seem-to-recognize-UTC0-as-a-timez | testsuite only |
| 0034-awk-Mark-test-for-handling-of-start-of-word-pattern- | testsuite only |
| 0037-Skip-additional-hexdump-test-on-big-endian-systems | testsuite only (even though we ARE big-endian, we don't run tests) |

### Applet install-path changes (4)

Alpine ships a different filesystem layout than our rootfs; relocating applets
would confuse scripts and muscle memory.

| Alpine file | Why dropped |
|-------------|-------------|
| 0003-blkdiscard-ship-link-to-sbin-instead-of-usr-bin | we don't ship blkdiscard |
| 0015-nologin-Install-applet-to-sbin-instead-of-usr-sbin | our layout uses `/sbin` already via busybox.config |
| 0020-app-location-for-cpio-vi-and-lspci | installs to `/sbin` on Alpine; we don't ship lspci at all |
| 0039-Install-lsblk-to-bin-instead-of-usr-bin | we don't ship lsblk — see the configuration section |

### Applet behaviour / policy changes (13)

Alpine's defaults diverge from the gateway's curated UX. These either change
user-visible behaviour, or assume adduser/passwd setups that don't match our
`/etc/passwd` and init scripts.

| Alpine file | Why dropped |
|-------------|-------------|
| 0005-init-add-support-for-separate-reboot-action | our init layout is different (userdata S9x scripts) |
| 0010-adduser-default-to-sbin-nologin-as-shell-for-system- | our /etc/passwd uses `/bin/false` for system users |
| 0011-ash-add-built-in-BB_ASH_VERSION-variable | no consumer of that variable in our scripts |
| 0014-modinfo-add-k-option-for-kernel-version | we don't ship modinfo on the gateway |
| 0016-pgrep-add-support-for-matching-against-UID-and-RUID | minor feature; our scripts don't need it |
| 0018-fsck-resolve-LABEL-.-UUID-.-spec-to-device | fsck not used on JFFS2/SquashFS |
| 0021-udhcpc-set-default-discover-retries-to-5 | our udhcpc config already sets retries explicitly |
| 0023-fbsplash-support-console-switching | no framebuffer on the gateway |
| 0024-fbsplash-support-image-and-bar-alignment-and-positio | no framebuffer |
| 0025-depmod-support-generating-kmod-binary-index-files | we use in-tree modules only |
| 0026-Add-flag-for-not-following-symlinks-when-recursing | diff-only feature, not needed |
| 0027-udhcpc-Don-t-background-if-n-is-given | our scripts don't use `udhcpc -n` |
| 0031-umount-Implement-O-option-to-unmount-by-mount-option | niche feature, not used |

---

## CVE supplements — retired at 1.38.0

The three path-traversal patches carried with an `800-` prefix on 1.37.0 are
gone. They were upstream BusyBox commits backported into 1.37.0, and 1.38.0
ships them: `archival/Config.src` declares `FEATURE_PATH_TRAVERSAL_PROTECTION`,
`archival/libarchive/unsafe_prefix.c` provides both `skip_unsafe_prefix()` and
`strip_unsafe_prefix()`, and `get_header_tar.c` guards the link-target strip
with `if (file_header->link_target && !S_ISLNK(file_header->mode))` — the exact
post-image of the former 802.

Verified three ways rather than assumed: 800 and 802 report "Reversed (or
previously applied) patch detected" against a clean 1.38.0 tree, 802
reverse-applies cleanly, and the region 801 rewrote is present upstream with a
further refinement on top (a `FEATURE_TAR_LONG_OPTIONS` split that keeps cpio's
`free()` of `file_header->name` valid). Nothing was dropped for convenience.

| Retired | CVE | Where it lives now |
|---|---|---|
| 800-CVE-2023-39810-path-traversal-protection | CVE-2023-39810 | upstream `archival/Config.src` |
| 801-CVE-2026-26157-tar-hardlink-path-traversal | CVE-2026-26157 | upstream `unsafe_prefix.c` + `data_extract_all.c` |
| 802-CVE-2026-26158-fix-symlink-target-stripping | CVE-2026-26158 | upstream `get_header_tar.c` |

---

## Lexra platform patches (4)

The 4 previously inline `sed` edits in `build_busybox.sh` have been
converted to proper `.patch` files (numbered 900-903), generated via
`diff -u` against a clean upstream copy. Converting them yields better
debuggability (we know exactly when a patch fails to apply after a
version bump) and symmetry with our toolchain patch stack.

| # | File | Target | Purpose |
|---|------|--------|---------|
| 900 | Lexra-off_t-size-check | `include/libbb.h` | comments out `BUG_off_t_size_is_misdetected` — musl MIPS mis-detects `sizeof(off_t)` vs `sizeof(uoff_t)` at compile time; the struct is a compile-time assertion, not a real bug. |
| 901 | Lexra-PAGE_SIZE-fallback | `scripts/generate_BUFSIZ.sh` | forces `PAGE_SIZE=1000` then `=4096` fallbacks. During cross-compilation `getconf PAGESIZE` queries the *host* machine, not the target — we need to force a sensible value. |
| 902 | Lexra-jffs2-fcntl-lock | `libbb/update_passwd.c` | silently ignore `fcntl(F_SETLK)` failures. JFFS2 does not implement file locking; the default warning spams once per `passwd`/`adduser`/`addgroup` invocation. |
| 903 | Lexra-usage-write-fortify | `applets/usage.c` | check the return value of `write()`. GCC 15 + `-D_FORTIFY_SOURCE=2` (Alpine's default, enabled by our toolchain patch 004-alpine) rejects unchecked `write()` with `warning: ignoring return value`. |

900-903 are applied **last**: they are platform adaptations, not bug
fixes, and should sit on top of the upstream + Alpine stack. All four applied
to 1.38.0 unchanged, which is the payoff for having converted them from inline
`sed` edits into real patches — a `sed` would have silently matched nothing.

---

## `build_busybox.sh` refactor

- 285 → 226 lines.
- Removed: 4 inline `sed` blocks (lines 117-164 of the previous version).
  Each one now lives as a standalone, reviewable `.patch` file.
- Replaced: two-phase "inline then security" patch logic, with one
  alphabetical loop over `patches/*.patch`.
- Added: hard-fail on patch application error (the previous version
  only printed a warning on failure, so a silently-broken patch could
  ship a half-patched source tree). We now `exit 1` with the last 10
  lines of patch output on any failure.
- Unchanged: tarball download, toolchain auto-detection, menuconfig
  flow, double-make (for `COMMON_BUFSIZE` optimization), install to
  `${ROOTFS_DIR}`, applet count verification.

---

## Configuration at the 1.38.0 bump

A version bump is where the applet selection can drift without anyone deciding
it. `build_busybox.sh` resolves the new Kconfig with `yes "" | make oldconfig`,
which accepts every new symbol at its upstream default, and only writes the
result back to `busybox.config` when the word "not set" appears in the output.
At this bump it did not write back — so the first 1.38.0 build shipped five
applets that the tracked configuration did not mention. Each new or flipped
symbol was therefore resolved deliberately and written into `busybox.config`,
which now determines the build on its own.

**Refused — five applets 1.38.0 would have added by default.** The curated set
is the point of this port, and none of them was ever chosen:

| Symbol | Applet | Why not |
|---|---|---|
| `CONFIG_SSL_SERVER` | `ssl_server` | network-facing, and we deliberately do not ship its `ssl_client` counterpart |
| `CONFIG_LSBLK` | `lsblk` | one flash device, no block topology to list |
| `CONFIG_VMSTAT` | `vmstat` | not used by any init script or tool |
| `CONFIG_UUIDGEN` | `uuidgen` | no consumer |
| `CONFIG_SHA384SUM` | `sha384sum` | no consumer |

**Accepted — four symbols that change no behaviour.** `FEATURE_IP_ROUTE` and
`FEATURE_IP_NEIGH` moved from "not set" to `y` because 1.38.0 has the `iproute`
and `ipneigh` applets `select` them, and both are already enabled here: the old
"not set" was an inconsistency the new Kconfig resolves rather than a choice
being overridden. `FEATURE_VERSION` is a new knob over `busybox --version`,
which was unconditional before, so `y` preserves today's behaviour.
`USE_BB_CRYPT_YES` adds verification of yescrypt (`$y$`) password hashes; it is
additive, and `CONFIG_FEATURE_DEFAULT_PASSWD_ALGO="des"` means nothing on the
device starts generating them.

**Dropped — one dead symbol.** `FEATURE_IFUPDOWN_IPV6` no longer exists in
1.38.0's Kconfig. It has no effect either way here: `ifup` and `ifdown` are not
among the installed applets.

---

## Verification

End-to-end build on 2026-09-03, BusyBox 1.38.0:

- All 19 patches apply sequentially with zero conflicts or fuzz.
- `busybox` binary: **ELF 32-bit MSB, MIPS-I, statically linked, stripped**,
  `BusyBox v1.38.0`. Text grows 758,297 → 770,516 bytes (+12,219, +1.6 %);
  data and bss shrink slightly. The on-disk size is unchanged at 787,708 bytes,
  which is alignment padding, not an unchanged build — the section sizes and the
  content hash both differ.
- **103 applets installed, byte-for-byte the same set as the 1.37.0 build** —
  none lost, none gained. This is the check that matters at a version bump, and
  it is the reason the five new applets above were refused.
- `busybox.config` is idempotent: a second `oldconfig` pass over it changes
  nothing but the timestamp comment.

Bench validation on 2026-09-03, Lidl gateway at 192.168.1.88, upgraded from
v4.0.0 by `flash_install_rtl8196e.sh` (full image, config preserved):

- boots to `RTL8196E Gateway - v4.2.0`, kernel `6.18.45-rtl8196e-v4.2.0`,
  `BusyBox v1.38.0`; a second, deliberate reboot came back in 29 seconds;
- twelve supervised services running across both boots — `syslogd`, `ntpd`,
  `watchdog`, `dropbear`, and `keepalive` over `s40button`, `otbr-agent` and
  `otbr-monitor`. `S80netwatch` is off by default and `S50uart_bridge` is
  skipped under `MODE=otbr`, both as designed;
- no panic, oops or call trace in the kernel log; no `segv`, `cannot exec` or
  `not found` in syslog, on either boot;
- the applet set on the device matches the flashed skeleton exactly; the twelve
  extra names are the `/userdata` overlay, and the preserved user additions
  (`iperf3`, `panic/`) survived the upgrade;
- `ash`: 300 cycles of "split, catch an expansion error, split something
  shorter" — the shape the `ifsbreakup` fix targets — with correct field counts
  throughout. This shows the shell healthy under that pattern; it is not proof
  the over-read is fixed, since the bug is a read and 1.37.0 would likely pass
  the same loop. A real repro needs the crafted stale-offset conditions.

Not covered: long-run soak, and the radio path — the bench carries no
`radio.conf`.

---

## Out of scope

- A static-build variant, and the items below. The 1.38 bump that used to
  sit here was done on 2026-09-03.
- Static-build variant — Alpine ships three BusyBox binaries (`/bin/busybox`,
  `busybox-extras`, `busybox.static`). We keep a single dynamic build
  to minimise the rootfs footprint (musl is already shipped for other
  userspace apps).
- HTTPS in wget via external `ssl_client` — requires a separate binary
  and Alpine-specific split.
- `utmps` / `wtmp` — not used on the gateway.
