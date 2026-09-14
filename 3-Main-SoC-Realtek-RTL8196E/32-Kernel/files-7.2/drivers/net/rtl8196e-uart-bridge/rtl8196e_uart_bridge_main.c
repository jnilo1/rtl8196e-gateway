// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8196e_uart_bridge_main.c - In-kernel UART<->TCP byte shoveler.
 *
 * Replaces the former userspace serialgateway daemon (removed in v3.0).
 * All configuration goes through module parameters exposed in
 *   /sys/module/rtl8196e_uart_bridge/parameters/
 * Writes to these files are applied live, so the baud rate or listen port
 * can be changed at runtime without rebooting or rebuilding the kernel:
 *
 *   tty                    — tty device path (default "/dev/ttyS1")              [rw, root]
 *   baud                   — UART baud rate (default 115200)                     [rw]
 *   port                   — TCP listen port (default 8888)                      [rw, root]
 *   bind_addr              — TCP bind address (default "0.0.0.0" = all)          [rw, root]
 *   flow_control           — 0/none, 1/hw RTS-CTS (default), 2/sw XON-XOFF;
 *                            set 0 for EFR32 flash                               [rw]
 *   enable                 — arm/disarm the bridge (default 0)                   [rw]
 *   armed                  — actual bridge state (read-only)                     [ro]
 *   stats                  — rx/tx/drop counters (read-only)                     [ro]
 *   nrst_pulse             — write 1: pulse EFR32 nRST low for 100 ms            [wo, root]
 *   nrst_gpio              — gpio-rtl819x line wired to EFR32 nRST (default 12)  [rw]
 *   blmode_pulse           — write 1: reset the EFR32 INTO its bootloader
 *                            (hold blmode through an nRST pulse)                 [wo, root]
 *   blmode_gpio            — gpio-rtl819x line wired to the EFR32 bootloader-
 *                            entry pin (-1 = board has none, default)            [rw]
 *   status_led_brightness  — brightness fired on 'uart-bridge-client' LED
 *                            trigger when a TCP client is connected (default 255) [rw]
 *
 * Boot sequence: the module loads with enable=0 and does nothing.
 * The init script S50uart_bridge sets the baud rate and writes
 * enable=1 once /dev/ttyS1 exists.
 *
 * Defaults for nrst_gpio and blmode_gpio can be described per-board in the
 * device tree (optional /radio-bridge node, matched by compatible
 * "realtek,rtl8196e-uart-bridge"). The same node's "realtek,hw-flow-control"
 * boolean declares whether the board physically wires RTS/CTS, which sets the
 * flow_control default (present -> hw, absent -> sw) and caps it: an hw request
 * on a board without the boolean is clamped to sw. Precedence: DT < kernel
 * command line < runtime sysfs writes. See bridge_seed_defaults_from_dt().
 *
 * Rationale and architecture: see DESIGN.md in this directory.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/tty.h>
#include <linux/tty_port.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/uio.h>
#include <linux/string.h>
#include <linux/leds.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <net/sock.h>
#include <net/tcp.h>

#define DRV_NAME    "rtl8196e-uart-bridge"
#define DRV_VERSION "1.7"

/* Software flow control bytes (flow_control=sw): sent bare by a radio
 * firmware built for XON/XOFF (e.g. NCP-UART-SW); such firmware escapes
 * data-plane 0x11/0x13, so bare occurrences are genuine flow control. */
#define BRIDGE_XON  0x11
#define BRIDGE_XOFF 0x13

/*
 * Guards all bridge_state transitions and the UART->TCP hot path.
 *
 * Config-transition atomicity (audit BRIDGE-001 / S01): the disarm and
 * relisten paths drop this mutex around blocking teardown
 * (kernel_sock_shutdown / kthread_stop) and retake it to finish clearing
 * state. Nothing rebuilds a consistent config view inside that window.
 * Today this is safe ONLY because every entry point that can mutate the
 * configuration is a built-in module-param setter, and kernel/params.c
 * serializes all built-in param sysfs access on one global `param_lock`
 * mutex — two setters can never interleave. If a non-param entry point
 * (ioctl, netlink, platform-driver bind, ...) is ever added, it will NOT
 * hold param_lock and the mid-teardown window becomes a live race:
 * introduce a dedicated config mutex held across whole transitions first.
 */
static DEFINE_MUTEX(bridge_lock);
/*
 * Serializes nrst_pulse and blmode_pulse callers without blocking the
 * UART->TCP hot path. Held across the claim / assert / msleep / release
 * sequence in param_set_nrst_pulse() and param_set_blmode_pulse() (the
 * latter holds it ~5.1 s — a concurrent pulse writer just waits);
 * bridge_lock is never taken there, so bridge_port_receive_buf() can
 * keep forwarding bytes to any TCP client still connected (the radio
 * reset is expected to drop in-flight bytes on the wire, but it must
 * not stall a concurrent receive_buf invocation that holds no relation
 * to the reset). Note that sysfs readers are NOT protected by this
 * design: every built-in param read shares the global param_lock in
 * kernel/params.c, so `cat stats` still blocks for the duration of a
 * pulse (~100 ms nrst, ~1.1 s blmode) — audit BRIDGE-003.
 * Also guards rtl_nrst_gpio and rtl_blmode_gpio, read by the pulse paths.
 */
static DEFINE_MUTEX(nrst_pulse_lock);

/* ------------------------------------------------------------------ state */

static struct bridge_state {
	struct tty_struct  *tty;
	struct socket      *listen_sock;
	struct socket      *client_sock;
	struct task_struct *worker;		/* accept worker */
	struct task_struct *client_worker;	/* current TCP -> UART worker */
	/*
	 * Snapshot of tty->port->client_ops captured at arm time, restored
	 * verbatim at disarm. Today /dev/ttyS1 always carries
	 * tty_port_default_client_ops, so the saved pointer is just the
	 * default — but stashing it explicitly insulates the disarm path
	 * from any future serdev/other layered consumer that might own
	 * the port before us.
	 */
	const struct tty_port_client_operations *saved_client_ops;
	bool                armed;
	/*
	 * Set under bridge_lock by disarm / reconfig_listen paths BEFORE they
	 * call kthread_stop(), cleared at next arm (or after the new worker is
	 * installed by reconfig_listen). The worker consults it under the
	 * same lock right after kernel_accept() returns a fresh client, so a
	 * connection that sneaks in during the tear-down window is released
	 * immediately instead of being installed as state.client_sock (which
	 * the outgoing disarm would then no longer know about).
	 */
	bool                stopping_worker;
	/*
	 * Software flow control (flow_control=sw): set when the radio sent
	 * a bare XOFF, cleared on XON. Written under bridge_lock (RX path,
	 * param setter, arm / client transitions) with WRITE_ONCE; read
	 * locklessly (READ_ONCE) by the worker's TX gate, which tolerates
	 * staleness by design (bounded wait + fail-open).
	 */
	bool                sw_tx_paused;
	/* stats */
	u64 rx_bytes;      /* UART -> TCP forwarded */
	u64 tx_bytes;      /* TCP -> UART forwarded */
	u64 drops_nocli;   /* UART bytes dropped: no client connected */
	u64 drops_err;     /* UART bytes dropped: sendmsg error */
	u64 drops_tx;      /* TCP bytes dropped: tty->write short */
	u64 xoff_events;   /* sw mode: bare XOFF received from radio */
	u64 xon_events;    /* sw mode: bare XON received from radio */
	u64 tx_pause_timeouts; /* sw mode: XOFF held > 1 s, failed open */
} state;

/* ----------------------------------------------------------- module params */

static char rtl_tty[64]       = "/dev/ttyS1";
static int  rtl_baud          = 115200;
static int  rtl_port          = 8888;
static char rtl_bind[64]      = "0.0.0.0";

/*
 * Flow-control mode. Int-backed (sysfs readback prints 0/1/2, never a
 * string): flash_efr32.sh writes literal 0/1 and string-compares the
 * readback, so the historical numeric ABI must survive the tri-state.
 */
enum bridge_fc_mode {
	BRIDGE_FC_NONE = 0,	/* no flow control (EFR32 flash / Xmodem) */
	BRIDGE_FC_HW   = 1,	/* RTS/CTS — CRTSCTS termios -> 8250 AFE */
	BRIDGE_FC_SW   = 2,	/* XON/XOFF from the radio, handled in-bridge */
};
static int  rtl_flow_control  = BRIDGE_FC_HW;

/*
 * Board capability: does this board physically wire RTS/CTS? Seeded from the
 * DT boolean "realtek,hw-flow-control" (compiled default true = Lidl). It is a
 * ceiling, never a writable param: an hw request on a !capable board is clamped
 * to sw, so CRTSCTS is never asserted on an unwired UART.
 */
static bool rtl_hw_fc_capable = true;

static bool rtl_enable        = false;
static int  rtl_nrst_gpio     = 12;     /* gpio-rtl819x line wired to EFR32 nRST */
static int  rtl_blmode_gpio   = -1;     /* EFR32 bootloader-entry pin; -1 = none
					 * (the Lidl board has no such pin —
					 * bootloader entry is done in-band) */

/* Set by the param setters (kernel cmdline or sysfs). The driver is
 * built-in, so cmdline params are applied before late_initcall — these
 * flags keep bridge_seed_defaults_from_dt() from clobbering an explicit
 * user choice with the device-tree value. */
static bool rtl_flow_control_set_by_user;
static bool rtl_nrst_gpio_set_by_user;
static bool rtl_blmode_gpio_set_by_user;

/* Status LED control: fire an LED trigger when a TCP client is connected,
 * clear it on disconnect. Mirrors the pre-v3.0 serialgateway behaviour
 * where the STATUS LED reflected "Zigbee host connected".
 *
 * Brightness is configurable (0-255) so S50uart_bridge can map it to the
 * eth0 led_mode (bright=255, dim=60, off=0) before arming. Users bind the
 * trigger to the physical LED with:
 *   echo uart-bridge-client > /sys/class/leds/status/trigger
 */
#define BRIDGE_LED_TRIG_NAME "uart-bridge-client"
static struct led_trigger *bridge_led_trig;
static int rtl_status_led_brightness = 255;

/* Forward decls: defined below but referenced by param set callbacks. */
static int  bridge_arm_locked(void);
static void bridge_disarm_locked(void);
static int  bridge_reconfig_baud_locked(void);
static int  bridge_reconfig_listen_locked(void);

/* --------------------------------------------------------------- hot path */

/*
 * UART -> TCP hot path.
 *
 * Installed directly as tty_port->client_ops.receive_buf, not via a
 * line discipline. Bypasses tty_port_default_receive_buf (which would
 * do an extra tty_ldisc_ref/deref round-trip and then call
 * tty_ldisc_receive_buf) and avoids the legacy receive_room gate that
 * ate our bytes in the previous ldisc-based iteration.
 *
 * Runs in the tty flip-buffer workqueue (process context, single
 * threaded per tty_port). Flow control is at the TCP sendmsg level:
 * MSG_DONTWAIT + drop-count on EAGAIN. We always consume the full
 * `count` (what we tell the tty core we ate) because the return value
 * is what the core uses for its own flow control and we'd rather drop
 * inside our stats than stall the flip buffer.
 *
 * Pattern lifted from drivers/tty/serdev/serdev-ttyport.c.
 */
/* Must be called with bridge_lock held. Forwards one chunk to the TCP
 * client with the historical drop-don't-block semantics. */
static void bridge_send_to_client_locked(const u8 *cp, size_t count)
{
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };
	struct kvec vec = { .iov_base = (void *)cp, .iov_len = count };
	int ret;

	if (!count)
		return;
	if (!state.client_sock) {
		state.drops_nocli += count;
		return;
	}

	ret = kernel_sendmsg(state.client_sock, &msg, &vec, 1, count);
	if (ret > 0) {
		state.rx_bytes += ret;
		if ((size_t)ret < count)
			state.drops_err += count - ret;
	} else if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
		state.drops_err += count;
	} else {
		/* Unrecoverable sendmsg error (peer closed, reset, etc.).
		 *
		 * We intentionally do NOT sock_release() here: the worker
		 * thread may still be blocked inside kernel_recvmsg() on
		 * the same socket. Releasing from this context would leave
		 * the worker dereferencing a freed socket (UAF). It would
		 * also race the disarm path, which could sock_release the
		 * same pointer (double free).
		 *
		 * But we DO kernel_sock_shutdown() it: that wakes the worker
		 * out of kernel_recvmsg() right away (next recvmsg returns
		 * 0/-EPIPE) instead of waiting for TCP keepalive or the
		 * peer's FIN, which can take seconds. The worker then
		 * releases the socket through its own cleanup path. Safe
		 * because kernel_sock_shutdown only flips the socket state;
		 * it does not free anything. Ownership of the final
		 * sock_release stays with the worker (or with the disarm
		 * path if disarm gets there first).
		 */
		state.drops_err += count;
		kernel_sock_shutdown(state.client_sock, SHUT_RDWR);
		pr_warn_ratelimited(DRV_NAME ": client sendmsg=%d, dropping %zu bytes\n",
				    ret, count);
	}
}

/* Must be called with bridge_lock held. sw mode only: scan the chunk for
 * bare XON/XOFF from the radio, gate the TCP->UART worker accordingly,
 * strip the control bytes from the TCP-bound stream and forward the data
 * segments in between unchanged. No ldisc is attached (client_ops
 * bypass), so termios IXON/IXOFF could not do this for us. */
static void bridge_receive_sw_locked(const u8 *cp, size_t count)
{
	size_t seg = 0, i;

	for (i = 0; i < count; i++) {
		if (cp[i] != BRIDGE_XON && cp[i] != BRIDGE_XOFF)
			continue;

		bridge_send_to_client_locked(cp + seg, i - seg);
		seg = i + 1;
		if (cp[i] == BRIDGE_XOFF) {
			WRITE_ONCE(state.sw_tx_paused, true);
			state.xoff_events++;
		} else {
			WRITE_ONCE(state.sw_tx_paused, false);
			state.xon_events++;
		}
	}
	bridge_send_to_client_locked(cp + seg, count - seg);
}

static size_t bridge_port_receive_buf(struct tty_port *port, const u8 *cp,
				      const u8 *fp, size_t count)
{
	mutex_lock(&bridge_lock);
	/* hw/none: single full-chunk send, byte-for-byte the v1.1 path.
	 * The scan branch costs nothing unless sw mode is selected. */
	if (rtl_flow_control == BRIDGE_FC_SW)
		bridge_receive_sw_locked(cp, count);
	else
		bridge_send_to_client_locked(cp, count);
	mutex_unlock(&bridge_lock);
	return count;
}

/* ------------------------------------------------------------ worker threads
 *
 * The accept worker remains in kernel_accept(), independently of the current
 * client's TCP->UART worker. This is what makes replace-on-connect real: a new
 * connection can be accepted while the old client is blocked in recvmsg().
 *
 * UART -> TCP bytes still flow through bridge_port_receive_buf() (tty rx
 * worker context), which sendmsgs directly on state.client_sock.
 */

/*
 * Bounded TTY write helper: retries short writes up to a small budget so a
 * TCP burst that outruns the 8250 tx_buf doesn't get truncated on the first
 * partial write. Without this, any write that only takes N<len bytes would
 * drop (len-N) silently — which breaks TCP's reliable byte-stream contract
 * and corrupts ASH/EZSP framing.
 *
 * Design constraints on RTL8196E / Lexra RLX4181:
 *   - single-core, no spinning: we sleep briefly between attempts
 *   - CPU-constrained: bounded number of retries (no unbounded loop)
 *   - no sleep under any kernel lock (caller holds none here)
 *   - drops_tx still accounts for bytes that truly didn't make it out
 */
static int bridge_tty_write_bounded(struct tty_struct *tty,
				    const u8 *buf, int len)
{
	int done = 0;
	int idle = 0;

	while (done < len && !kthread_should_stop()) {
		int ret;

		if (!tty || !tty->ops || !tty->ops->write)
			break;

		ret = tty->ops->write(tty, buf + done, len - done);
		if (ret > 0) {
			done += ret;
			idle = 0;
			continue;
		}

		/* Zero or negative return: no progress. Bail after a few
		 * short sleeps rather than busy-looping — on a single-core
		 * Lexra every spin is a spin the UART TX fifo can't use.
		 */
		if (++idle >= 4)
			break;

		usleep_range(1000, 2000);
	}

	return done;
}

static int bridge_client_thread(void *data)
{
	struct socket *sock = data;
	u8 buf[512];
	int last_recv = 0;

	/* Pairs with the accept worker publishing both state pointers while
	 * holding bridge_lock immediately after kthread_run(). */
	mutex_lock(&bridge_lock);
	if (state.client_sock != sock || state.client_worker != current) {
		mutex_unlock(&bridge_lock);
		while (!kthread_should_stop())
			msleep_interruptible(100);
		sock_release(sock);
		return 0;
	}
	mutex_unlock(&bridge_lock);

	while (!kthread_should_stop()) {
		struct kvec vec = { .iov_base = buf, .iov_len = sizeof(buf) };
		struct msghdr msg = { .msg_flags = 0 };
		struct tty_struct *tty;
		int n, written;

		n = kernel_recvmsg(sock, &msg, &vec, 1, sizeof(buf), 0);
		if (n == -EINTR)
			continue;
		if (n <= 0) {
			last_recv = n;
			break;
		}

		if (READ_ONCE(rtl_flow_control) == BRIDGE_FC_SW &&
		    READ_ONCE(state.sw_tx_paused)) {
			unsigned long deadline = jiffies + HZ;

			while (READ_ONCE(state.sw_tx_paused) &&
			       time_before(jiffies, deadline) &&
			       !kthread_should_stop())
				usleep_range(1000, 2000);

			if (READ_ONCE(state.sw_tx_paused) &&
			    !kthread_should_stop()) {
				mutex_lock(&bridge_lock);
				WRITE_ONCE(state.sw_tx_paused, false);
				state.tx_pause_timeouts++;
				mutex_unlock(&bridge_lock);
				pr_warn_ratelimited(DRV_NAME
					": radio XOFF held > 1 s, failing open\n");
			}
		}

		mutex_lock(&bridge_lock);
		tty = state.tty;
		mutex_unlock(&bridge_lock);

		written = bridge_tty_write_bounded(tty, buf, n);

		mutex_lock(&bridge_lock);
		state.tx_bytes += written;
		if (written < n)
			state.drops_tx += n - written;
		mutex_unlock(&bridge_lock);
	}

	mutex_lock(&bridge_lock);
	if (state.client_sock == sock && state.client_worker == current) {
		WRITE_ONCE(state.sw_tx_paused, false);
		state.client_sock = NULL;
		mutex_unlock(&bridge_lock);
		led_trigger_event(bridge_led_trig, 0);
		pr_info_ratelimited(DRV_NAME
			": client disconnected (recvmsg=%d%s)\n",
			last_recv, last_recv == 0 ? " EOF" : "");
	} else {
		mutex_unlock(&bridge_lock);
	}
	/*
	 * Keep both the task and socket alive until the accept worker or
	 * disarm joins us. The captured socket must remain valid for their
	 * kernel_sock_shutdown(), even if EOF races the replacement. On a
	 * natural EOF state.client_worker deliberately remains current, so
	 * the next accept can reap us before installing its replacement.
	 */
	while (!kthread_should_stop())
		msleep_interruptible(100);
	sock_release(sock);
	return 0;
}

static int bridge_worker_thread(void *data)
{
	while (!kthread_should_stop()) {
		struct task_struct *old_th, *new_th;
		struct socket *newsock = NULL, *old_sock;
		int brightness, ret;

		ret = kernel_accept(READ_ONCE(state.listen_sock), &newsock, 0);
		if (kthread_should_stop())
			break;
		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			msleep_interruptible(100);
			continue;
		}

		tcp_sock_set_nodelay(newsock->sk);
		sock_set_keepalive(newsock->sk);

		mutex_lock(&bridge_lock);
		if (state.stopping_worker || !state.armed) {
			mutex_unlock(&bridge_lock);
			sock_release(newsock);
			continue;
		}

		old_sock = state.client_sock;
		old_th = state.client_worker;
		if (old_sock || old_th) {
			state.client_sock = NULL;
			state.client_worker = NULL;
			WRITE_ONCE(state.sw_tx_paused, false);
		}
		mutex_unlock(&bridge_lock);

		if (old_sock || old_th) {
			pr_info_ratelimited(DRV_NAME ": replacing previous client\n");
			if (old_sock)
				kernel_sock_shutdown(old_sock, SHUT_RDWR);
			if (old_th)
				kthread_stop(old_th);
			else if (old_sock)
				sock_release(old_sock);
		}

		mutex_lock(&bridge_lock);
		if (state.stopping_worker || !state.armed) {
			mutex_unlock(&bridge_lock);
			sock_release(newsock);
			continue;
		}

		/*
		 * kthread_run() may schedule the new thread immediately. It
		 * starts with bridge_lock, so keeping the mutex held until both
		 * state pointers are published makes ownership atomic.
		 */
		new_th = kthread_run(bridge_client_thread, newsock,
				     DRV_NAME "-client");
		if (IS_ERR(new_th)) {
			ret = PTR_ERR(new_th);
			mutex_unlock(&bridge_lock);
			led_trigger_event(bridge_led_trig, 0);
			pr_warn_ratelimited(DRV_NAME
				": client worker creation failed: %d\n", ret);
			sock_release(newsock);
			continue;
		}
		state.client_sock = newsock;
		state.client_worker = new_th;
		WRITE_ONCE(state.sw_tx_paused, false);
		brightness = clamp(rtl_status_led_brightness, 0, 255);
		mutex_unlock(&bridge_lock);

		led_trigger_event(bridge_led_trig, brightness);
		{
			struct sockaddr_in peer;

			if (kernel_getpeername(newsock,
					       (struct sockaddr *)&peer) >= 0)
				pr_info_ratelimited(DRV_NAME
					": client connected from %pI4:%u\n",
					&peer.sin_addr, ntohs(peer.sin_port));
			else
				pr_info_ratelimited(DRV_NAME
					": client connected\n");
		}
	}
	return 0;
}

/*
 * tty_wakeup() in the tty core calls port->client_ops->write_wakeup()
 * unconditionally (no NULL check). The 8250 driver fires this every time
 * it drains its TX buffer, so leaving this NULL is a free kernel oops.
 * Our worker thread doesn't actually wait on a write_wakeup event — it
 * just writes and drops on partial — so the wakeup is a no-op for us.
 */
static void bridge_port_write_wakeup(struct tty_port *port)
{
}

/* ------------------------------------------------------ tty_port client_ops
 *
 * We install this directly on tty->port->client_ops at arm time and
 * restore tty_port_default_client_ops on disarm. No line discipline in
 * the hot path: the tty flip buffer workqueue calls us directly.
 */
static const struct tty_port_client_operations bridge_port_client_ops = {
	.receive_buf  = bridge_port_receive_buf,
	.write_wakeup = bridge_port_write_wakeup,
};

/* ------------------------------------------------------ path -> dev_t look */

static int resolve_tty_devt(const char *tty_path, dev_t *out)
{
	struct path p;
	struct inode *inode;
	int ret;

	ret = kern_path(tty_path, LOOKUP_FOLLOW, &p);
	if (ret)
		return ret;

	inode = d_backing_inode(p.dentry);
	if (!inode || !S_ISCHR(inode->i_mode)) {
		path_put(&p);
		return -ENOTTY;
	}
	*out = inode->i_rdev;
	path_put(&p);
	return 0;
}

/* ----------------------------------------------------------- tty configure */

static int bridge_apply_termios(struct tty_struct *tty, int baud)
{
	struct ktermios k;
	int ret;

	k = tty->termios;
	/* Raw 8N1, carrier-detect ignored; RTS/CTS only in hw mode.
	 * IXON/IXOFF/IXANY are always cleared: no ldisc is attached
	 * (client_ops bypass), so termios software flow control would be
	 * inert anyway — in sw mode the bridge itself handles the radio's
	 * XON/XOFF (see bridge_receive_sw_locked). */
	k.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
		       INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
	k.c_oflag &= ~OPOST;
	k.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	k.c_cflag &= ~(CSIZE | PARENB | CBAUD | CRTSCTS);
	k.c_cflag |= CS8 | CLOCAL | CREAD;
	if (rtl_flow_control == BRIDGE_FC_HW)
		k.c_cflag |= CRTSCTS;
	tty_termios_encode_baud_rate(&k, baud, baud);

	ret = tty_set_termios(tty, &k);
	pr_debug(DRV_NAME ": termios applied: requested=%d ospeed=%u ispeed=%u cflag=0x%x ret=%d\n",
		 baud, tty->termios.c_ospeed, tty->termios.c_ispeed,
		 tty->termios.c_cflag, ret);
	return ret;
}

/* ----------------------------------------------------- listen socket setup */

static int bridge_create_listen_sock(int port, const char *bind_addr,
				     struct socket **out)
{
	struct socket *sock;
	struct sockaddr_in addr;
	__be32 ip = htonl(INADDR_ANY);
	int ret;

	if (bind_addr && bind_addr[0] &&
	    !in4_pton(bind_addr, -1, (u8 *)&ip, -1, NULL)) {
		pr_err(DRV_NAME ": invalid bind address '%s'\n", bind_addr);
		return -EINVAL;
	}

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &sock);
	if (ret)
		return ret;

	sock_set_reuseaddr(sock->sk);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16)port);
	addr.sin_addr.s_addr = ip;

	ret = kernel_bind(sock, (struct sockaddr_unsized *)&addr, sizeof(addr));
	if (ret)
		goto fail;

	ret = kernel_listen(sock, 1);
	if (ret)
		goto fail;

	*out = sock;
	return 0;
fail:
	sock_release(sock);
	return ret;
}

/* ----------------------------------------------------------- arm / disarm */

/* Must be called with bridge_lock held. */
static int bridge_arm_locked(void)
{
	struct tty_struct *tty = NULL;
	struct socket *ls = NULL;
	struct task_struct *th = NULL;
	dev_t devt;
	int ret;

	if (state.armed)
		return 0;

	ret = resolve_tty_devt(rtl_tty, &devt);
	if (ret) {
		pr_err(DRV_NAME ": cannot resolve %s: %d\n", rtl_tty, ret);
		return ret;
	}

	tty = tty_kopen_exclusive(devt);
	if (IS_ERR(tty)) {
		ret = PTR_ERR(tty);
		pr_err(DRV_NAME ": tty_kopen_exclusive(%s) failed: %d\n",
		       rtl_tty, ret);
		return ret;
	}

	/* tty_kopen_exclusive returns with tty_lock held but has NOT called
	 * the driver's ops->open — that's what actually asserts DTR/RTS and
	 * enables the UART RX path.
	 */
	if (!tty->ops->open || !tty->ops->close) {
		tty_unlock(tty);
		tty_kclose(tty);
		pr_err(DRV_NAME ": tty ops missing open/close\n");
		return -ENODEV;
	}
	tty_unlock(tty);

	/* Install our port client_ops before opening the UART. This replaces
	 * the previous client_ops
	 * (typically tty_port_default_client_ops, which would forward bytes
	 * through the ldisc layer); we want to bypass the ldisc entirely
	 * and take the bytes directly from the flip buffer.
	 * tty_buffer_lock_exclusive() excludes receive callback dispatch
	 * while the pointer changes. Installing before ops->open also means
	 * the UART cannot concurrently call write_wakeup on the old table.
	 * Keep the buffer exclusion until arm is completely published.
	 * The previous pointer is stashed so disarm restores it verbatim.
	 */
	tty_buffer_lock_exclusive(tty->port);
	state.saved_client_ops = tty->port->client_ops;
	tty->port->client_ops = &bridge_port_client_ops;

	tty_lock(tty);
	ret = tty->ops->open(tty, NULL);
	tty_unlock(tty);
	if (ret) {
		pr_err(DRV_NAME ": tty ops->open failed: %d\n", ret);
		goto err_client_ops;
	}

	ret = bridge_apply_termios(tty, rtl_baud);
	if (ret) {
		pr_err(DRV_NAME ": apply termios failed: %d\n", ret);
		goto err_tty_open;
	}

	ret = bridge_create_listen_sock(rtl_port, rtl_bind, &ls);
	if (ret) {
		pr_err(DRV_NAME ": listen on %s:%d failed: %d\n",
		       rtl_bind, rtl_port, ret);
		goto err_tty_open;
	}

	state.tty = tty;
	/* WRITE_ONCE: publishes the socket to the worker's lockless
	 * READ_ONCE(state.listen_sock) in the accept loop. Happens
	 * before kthread_run so there's no pre-existing reader yet.
	 */
	WRITE_ONCE(state.listen_sock, ls);
	state.rx_bytes = 0;
	state.tx_bytes = 0;
	state.drops_nocli = 0;
	state.drops_err = 0;
	state.drops_tx = 0;
	state.xoff_events = 0;
	state.xon_events = 0;
	state.tx_pause_timeouts = 0;
	WRITE_ONCE(state.sw_tx_paused, false);

	th = kthread_run(bridge_worker_thread, NULL, DRV_NAME "-worker");
	if (IS_ERR(th)) {
		ret = PTR_ERR(th);
		pr_err(DRV_NAME ": accept kthread failed: %d\n", ret);
		state.tty = NULL;
		WRITE_ONCE(state.listen_sock, NULL);
		goto err_sock;
	}
	state.worker = th;
	/* Clear any sticky stopping_worker left over from a previous disarm
	 * cycle — this is a fresh worker bound to a fresh listen socket. */
	state.stopping_worker = false;
	state.armed = true;
	tty_buffer_unlock_exclusive(tty->port);

	pr_info(DRV_NAME ": armed on %s @ %d baud, listening on %s:%d\n",
		rtl_tty, rtl_baud, rtl_bind, rtl_port);
	return 0;

err_sock:
	sock_release(ls);
err_tty_open:
	/*
	 * uart_close() reaches tty_buffer_flush(), which takes the same
	 * non-recursive flip-buffer mutex held here. Drop both exclusions
	 * while closing: an already-dispatched bridge callback can then finish
	 * under bridge_lock, and the close path can flush before stopping RX.
	 * The global built-in param_lock still serializes configuration writers.
	 */
	tty_buffer_unlock_exclusive(tty->port);
	mutex_unlock(&bridge_lock);
	tty_lock(tty);
	tty->ops->close(tty, NULL);
	tty_unlock(tty);
	tty_buffer_lock_exclusive(tty->port);
	tty->port->client_ops = state.saved_client_ops;
	state.saved_client_ops = NULL;
	tty_buffer_unlock_exclusive(tty->port);
	tty_kclose(tty);
	mutex_lock(&bridge_lock);
	return ret;
err_client_ops:
	tty->port->client_ops = state.saved_client_ops;
	state.saved_client_ops = NULL;
	tty_buffer_unlock_exclusive(tty->port);
	mutex_unlock(&bridge_lock);
	tty_kclose(tty);
	mutex_lock(&bridge_lock);
	return ret;
}

/* Must be called with bridge_lock held. */
static void bridge_disarm_locked(void)
{
	struct task_struct *th, *client_th;
	struct tty_struct *tty;
	struct socket *ls, *cs;
	u64 rx, tx, dn, de, dtx, xo, xn, tpt;

	if (!state.armed)
		return;

	/* Capture refs. Two different concerns:
	 *
	 *  - state.client_sock is ALWAYS accessed under bridge_lock, so it's
	 *    safe to claim (NULL under lock) immediately. Any racing
	 *    receive_buf that grabs the lock after us will see NULL and
	 *    take its early-return path (drops_nocli++). This also prevents
	 *    the original double-free: receive_buf's own error path NULLs
	 *    state.client_sock under lock — if it ran after our capture but
	 *    before our NULL, both sides would try sock_release(cs).
	 *
	 *  - state.listen_sock is read UNLOCKED by the worker thread in
	 *    kernel_accept(). NULLing it before kthread_stop() returns
	 *    races the worker's next loop iteration after our
	 *    kernel_sock_shutdown() wakes it, causing a NULL deref and
	 *    kernel oops. So we keep it populated until the worker has
	 *    actually exited (kthread_stop is synchronous).
	 */
	th = state.worker;
	tty = state.tty;
	ls = state.listen_sock;
	cs = state.client_sock;
	client_th = state.client_worker;
	state.client_sock = NULL;
	state.client_worker = NULL;
	state.armed = false;
	/* Tell the worker to reject any connection it accepts from this
	 * point on (see bridge_worker_thread). The flag stays set until the
	 * next arm — disarm never re-arms a worker itself.
	 */
	state.stopping_worker = true;

	/* Bridge is going down. Clear the STATUS LED unconditionally —
	 * idempotent if no client was connected. */
	led_trigger_event(bridge_led_trig, 0);
	WRITE_ONCE(state.sw_tx_paused, false);
	rx = state.rx_bytes;
	tx = state.tx_bytes;
	dn = state.drops_nocli;
	de = state.drops_err;
	dtx = state.drops_tx;
	xo = state.xoff_events;
	xn = state.xon_events;
	tpt = state.tx_pause_timeouts;

	/* Drop the mutex before blocking on socket shutdown / kthread_stop
	 * so we don't deadlock with receive_buf (which grabs the same lock).
	 * Only the global built-in param_lock makes this drop-and-retake
	 * window safe against concurrent config writers — see the
	 * bridge_lock definition comment (audit BRIDGE-001).
	 */
	mutex_unlock(&bridge_lock);

	/* Wake the accept and client workers out of their blocking calls. */
	if (cs)
		kernel_sock_shutdown(cs, SHUT_RDWR);
	if (ls)
		kernel_sock_shutdown(ls, SHUT_RDWR);

	/* Stop accepting first; stopping_worker prevents a late installation. */
	if (th)
		kthread_stop(th);
	if (client_th)
		kthread_stop(client_th);

	/*
	 * Stop UART IRQ activity before changing client_ops. uart_close()
	 * flushes the tty buffer and therefore must run before
	 * tty_buffer_lock_exclusive(): taking the exclusive lock first would
	 * self-deadlock when tty_buffer_flush() tried to take it again.
	 * bridge_lock remains dropped so an already-dispatched bridge callback
	 * can finish while close/flush waits for it.
	 */
	if (tty) {
		tty_lock(tty);
		tty->ops->close(tty, NULL);
		tty_unlock(tty);
		tty_buffer_lock_exclusive(tty->port);
		tty->port->client_ops = state.saved_client_ops;
		state.saved_client_ops = NULL;
		tty_buffer_unlock_exclusive(tty->port);
	}

	/* The client worker owns and releases its socket on every exit path. */
	if (cs && !client_th)
		sock_release(cs);
	if (ls)
		sock_release(ls);
	if (tty)
		tty_kclose(tty);

	pr_info(DRV_NAME ": disarmed (rx=%llu tx=%llu drops_nocli=%llu drops_err=%llu drops_tx=%llu xoff=%llu xon=%llu tx_pause_timeouts=%llu)\n",
		rx, tx, dn, de, dtx, xo, xn, tpt);

	/* Clear the rest of the state under the lock so subsequent sysfs
	 * readers / receive_buf see a consistent disarmed state.
	 */
	mutex_lock(&bridge_lock);
	state.worker = NULL;
	state.client_worker = NULL;
	state.tty = NULL;
	/* WRITE_ONCE pairs with the worker's READ_ONCE in the accept loop.
	 * Safe to clear here because kthread_stop() above has guaranteed
	 * the worker is no longer running.
	 */
	WRITE_ONCE(state.listen_sock, NULL);
}

/* --------------------------------------------------- live reconfig helpers */

/* Must be called with bridge_lock held. */
static int bridge_reconfig_baud_locked(void)
{
	if (!state.armed || !state.tty)
		return 0;
	return bridge_apply_termios(state.tty, rtl_baud);
}

/* Must be called with bridge_lock held. Rebuilds listen_sock + accept
 * thread without touching the tty / ldisc. Useful when the port changes.
 */
static int bridge_reconfig_listen_locked(void)
{
	struct task_struct *th_old, *th_new;
	struct socket *ls_old, *ls_new = NULL;
	int ret;

	if (!state.armed)
		return 0;

	/* Stop and release the old listener before binding the replacement.
	 * This is required for wildcard -> specific-address transitions on
	 * the same port; SO_REUSEADDR alone does not make both listeners
	 * coexist. The independent client worker/socket remain untouched.
	 */
	th_old = state.worker;
	ls_old = state.listen_sock;
	state.stopping_worker = true;

	mutex_unlock(&bridge_lock);
	if (ls_old)
		kernel_sock_shutdown(ls_old, SHUT_RDWR);
	if (th_old)
		kthread_stop(th_old);
	if (ls_old)
		sock_release(ls_old);

	mutex_lock(&bridge_lock);
	state.worker = NULL;
	WRITE_ONCE(state.listen_sock, NULL);

	ret = bridge_create_listen_sock(rtl_port, rtl_bind, &ls_new);
	if (ret)
		return ret;

	WRITE_ONCE(state.listen_sock, ls_new);

	th_new = kthread_run(bridge_worker_thread, NULL, DRV_NAME "-worker");
	if (IS_ERR(th_new)) {
		ret = PTR_ERR(th_new);
		pr_err(DRV_NAME ": accept worker restart failed: %d\n", ret);
		WRITE_ONCE(state.listen_sock, NULL);
		sock_release(ls_new);
		return ret;
	}
	state.worker = th_new;
	state.stopping_worker = false;

	pr_info(DRV_NAME ": relistening on %s:%d\n", rtl_bind, rtl_port);
	return 0;
}

/* ----------------------------------------------------------- param ops */

static int param_set_tty(const char *val, const struct kernel_param *kp)
{
	char buf[sizeof(rtl_tty)];
	ssize_t n;
	int ret = 0;

	n = strscpy(buf, val, sizeof(buf));
	if (n < 0)
		return -ENAMETOOLONG;
	/* Strip trailing newline if any (sysfs writes include it) */
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	mutex_lock(&bridge_lock);
	if (strcmp(buf, rtl_tty) == 0) {
		mutex_unlock(&bridge_lock);
		return 0;
	}
	if (state.armed) {
		char old_tty[sizeof(rtl_tty)];

		strscpy(old_tty, rtl_tty, sizeof(old_tty));
		strscpy(rtl_tty, buf, sizeof(rtl_tty));
		bridge_disarm_locked();
		ret = bridge_arm_locked();
		if (ret) {
			pr_warn(DRV_NAME ": tty=%s failed (%d), rolling back to %s\n",
				buf, ret, old_tty);
			strscpy(rtl_tty, old_tty, sizeof(rtl_tty));
			bridge_arm_locked();
		}
	} else {
		strscpy(rtl_tty, buf, sizeof(rtl_tty));
	}
	mutex_unlock(&bridge_lock);
	return ret;
}

static int param_get_tty(char *buffer, const struct kernel_param *kp)
{
	int n;

	mutex_lock(&bridge_lock);
	n = scnprintf(buffer, PAGE_SIZE, "%s\n", rtl_tty);
	mutex_unlock(&bridge_lock);
	return n;
}

static const struct kernel_param_ops tty_ops = {
	.set = param_set_tty,
	.get = param_get_tty,
};

static int param_set_baud(const char *val, const struct kernel_param *kp)
{
	int new_baud, ret;

	ret = kstrtoint(val, 0, &new_baud);
	if (ret)
		return ret;
	if (new_baud < 1200 || new_baud > 4000000)
		return -EINVAL;

	mutex_lock(&bridge_lock);
	if (new_baud != rtl_baud) {
		int old_baud = rtl_baud;

		rtl_baud = new_baud;
		ret = bridge_reconfig_baud_locked();
		if (ret) {
			pr_warn(DRV_NAME ": baud=%d failed (%d), rolling back to %d\n",
				new_baud, ret, old_baud);
			rtl_baud = old_baud;
			bridge_reconfig_baud_locked();
		}
	}
	mutex_unlock(&bridge_lock);
	return ret;
}

static int param_get_baud(char *buffer, const struct kernel_param *kp)
{
	int v;

	mutex_lock(&bridge_lock);
	v = rtl_baud;
	mutex_unlock(&bridge_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v);
}

static const struct kernel_param_ops baud_ops = {
	.set = param_set_baud,
	.get = param_get_baud,
};

static int param_set_port(const char *val, const struct kernel_param *kp)
{
	int new_port, ret;

	ret = kstrtoint(val, 0, &new_port);
	if (ret)
		return ret;
	if (new_port < 1 || new_port > 65535)
		return -EINVAL;

	mutex_lock(&bridge_lock);
	if (new_port == rtl_port) {
		mutex_unlock(&bridge_lock);
		return 0;
	}
	{
		int old_port = rtl_port;

		rtl_port = new_port;
		ret = bridge_reconfig_listen_locked();
		if (ret) {
			int rollback_ret;

			pr_warn(DRV_NAME ": port=%d failed (%d), rolling back to %d\n",
				new_port, ret, old_port);
			rtl_port = old_port;
			rollback_ret = bridge_reconfig_listen_locked();
			if (rollback_ret) {
				pr_err(DRV_NAME
					": port rollback failed: %d, disarming\n",
					rollback_ret);
				bridge_disarm_locked();
			}
		}
	}
	mutex_unlock(&bridge_lock);
	return ret;
}

static int param_get_port(char *buffer, const struct kernel_param *kp)
{
	int v;

	mutex_lock(&bridge_lock);
	v = rtl_port;
	mutex_unlock(&bridge_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v);
}

static const struct kernel_param_ops port_ops = {
	.set = param_set_port,
	.get = param_get_port,
};

static int param_set_enable(const char *val, const struct kernel_param *kp)
{
	bool new_en;
	int ret;

	ret = kstrtobool(val, &new_en);
	if (ret)
		return ret;

	mutex_lock(&bridge_lock);
	rtl_enable = new_en;
	if (new_en)
		ret = bridge_arm_locked();
	else
		bridge_disarm_locked();
	mutex_unlock(&bridge_lock);
	return ret;
}

static int param_get_enable(char *buffer, const struct kernel_param *kp)
{
	bool v;

	/* rtl_enable is mutated under bridge_lock in param_set_enable; read
	 * it under the same lock for consistency with the other getters and
	 * to avoid a data race the compiler is free to tear.
	 */
	mutex_lock(&bridge_lock);
	v = rtl_enable;
	mutex_unlock(&bridge_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v ? 1 : 0);
}

static const struct kernel_param_ops enable_ops = {
	.set = param_set_enable,
	.get = param_get_enable,
};

static int param_set_bind(const char *val, const struct kernel_param *kp)
{
	char buf[sizeof(rtl_bind)];
	ssize_t n;
	__be32 test;
	int ret = 0;

	n = strscpy(buf, val, sizeof(buf));
	if (n < 0)
		return -ENAMETOOLONG;
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	/* Validate the address before accepting it */
	if (buf[0] && !in4_pton(buf, -1, (u8 *)&test, -1, NULL))
		return -EINVAL;

	mutex_lock(&bridge_lock);
	if (strcmp(buf, rtl_bind) == 0) {
		mutex_unlock(&bridge_lock);
		return 0;
	}
	{
		char old_bind[sizeof(rtl_bind)];

		strscpy(old_bind, rtl_bind, sizeof(old_bind));
		strscpy(rtl_bind, buf, sizeof(rtl_bind));
		ret = bridge_reconfig_listen_locked();
		if (ret) {
			int rollback_ret;

			pr_warn(DRV_NAME ": bind_addr=%s failed (%d), rolling back to %s\n",
				buf, ret, old_bind);
			strscpy(rtl_bind, old_bind, sizeof(rtl_bind));
			rollback_ret = bridge_reconfig_listen_locked();
			if (rollback_ret) {
				pr_err(DRV_NAME
					": bind rollback failed: %d, disarming\n",
					rollback_ret);
				bridge_disarm_locked();
			}
		}
	}
	mutex_unlock(&bridge_lock);
	return ret;
}

static int param_get_bind(char *buffer, const struct kernel_param *kp)
{
	int n;

	mutex_lock(&bridge_lock);
	n = scnprintf(buffer, PAGE_SIZE, "%s\n", rtl_bind);
	mutex_unlock(&bridge_lock);
	return n;
}

static const struct kernel_param_ops bind_ops = {
	.set = param_set_bind,
	.get = param_get_bind,
};

/*
 * flow_control: switch the flow-control mode at runtime without
 * disarming the bridge.
 *
 * Accepted spellings: 0/1/2, "none"/"hw"/"sw" (lowercase), plus the
 * legacy kstrtobool forms (y/n/on/off — flash_efr32.sh and older docs
 * write 0/1). Readback always prints the numeric value: flash_efr32.sh
 * string-compares it against '0'/'1', so this ABI must stay numeric.
 *
 * Needed by flash_efr32.sh: the Gecko bootloader Xmodem path requires
 * all flow control off (Xmodem payloads contain raw 0x11/0x13, so sw
 * mode must be off too).  Write 0 before flashing and restore after.
 * Re-applies termios through bridge_reconfig_baud_locked() (which
 * re-calls bridge_apply_termios() with the updated rtl_flow_control).
 */
static int bridge_parse_fc(const char *val, int *out)
{
	bool b;
	int v;

	if (!kstrtoint(val, 0, &v)) {
		if (v < BRIDGE_FC_NONE || v > BRIDGE_FC_SW)
			return -EINVAL;
		*out = v;
		return 0;
	}
	if (sysfs_streq(val, "none")) {
		*out = BRIDGE_FC_NONE;
		return 0;
	}
	if (sysfs_streq(val, "hw")) {
		*out = BRIDGE_FC_HW;
		return 0;
	}
	if (sysfs_streq(val, "sw")) {
		*out = BRIDGE_FC_SW;
		return 0;
	}
	if (!kstrtobool(val, &b)) {
		*out = b ? BRIDGE_FC_HW : BRIDGE_FC_NONE;
		return 0;
	}
	return -EINVAL;
}

static int param_set_flow_control(const char *val, const struct kernel_param *kp)
{
	int new_fc, ret;

	ret = bridge_parse_fc(val, &new_fc);
	if (ret)
		return ret;

	mutex_lock(&bridge_lock);
	rtl_flow_control_set_by_user = true;
	/* Capability ceiling: never assert CRTSCTS on a board without RTS/CTS
	 * wiring. Clamp (not -EINVAL) so the write still succeeds. */
	if (new_fc == BRIDGE_FC_HW && !rtl_hw_fc_capable) {
		pr_warn(DRV_NAME ": flow_control=hw but board has no RTS/CTS wiring; using sw\n");
		new_fc = BRIDGE_FC_SW;
	}
	if (new_fc != rtl_flow_control) {
		int old_fc = rtl_flow_control;

		rtl_flow_control = new_fc;
		/* Mode switch invalidates any pending XOFF gate. */
		WRITE_ONCE(state.sw_tx_paused, false);
		if (state.armed) {
			ret = bridge_reconfig_baud_locked();
			if (ret) {
				pr_warn(DRV_NAME ": flow_control=%d failed (%d), rolling back to %d\n",
					new_fc, ret, old_fc);
				rtl_flow_control = old_fc;
				WRITE_ONCE(state.sw_tx_paused, false);
				bridge_reconfig_baud_locked();
			}
		}
	}
	mutex_unlock(&bridge_lock);
	return ret;
}

static int param_get_flow_control(char *buffer, const struct kernel_param *kp)
{
	int v;

	mutex_lock(&bridge_lock);
	v = rtl_flow_control;
	mutex_unlock(&bridge_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v);
}

static const struct kernel_param_ops flow_control_ops = {
	.set = param_set_flow_control,
	.get = param_get_flow_control,
};

/*
 * nrst_pulse: assert the EFR32's nRST line, hold 100 ms, release. Recovers
 * a stuck EFR32 (crashed/livelock app, J-Link halt, corrupted-app
 * `pc == 0xFFFFFFFF`) without rebooting the SoC.
 *
 * Mechanism: nRST is wired to a single SoC pad — GPIO B4 (line 12 on the
 * gpio-rtl819x chip) on the Lidl gateway, isolated per-pad on the bench
 * (discussion #121). The pulse claims that line through the gpiod consumer
 * API with open-drain semantics: assert drives the pad low; release floats
 * it back to input and lets the EFR32's internal RESETn pull-up raise the
 * line (RESETn must never be driven high). The pad mux to GPIO mode
 * (PIN_MUX_SEL_2 field, datasheet Table 36) is applied by the gpio-rtl819x
 * request() hook on every claim.
 *
 * Boards that route nRST to a different pad (e.g. the Sengled G4 port) set
 * the line number via the nrst_gpio parameter. gpio-rtl819x only auto-muxes
 * pads B2-B6 (lines 10-14); a line outside that range needs its pad mux
 * established separately before pulsing.
 *
 * Up to driver v1.0 this knob instead set PIN_MUX_SEL_2 bits {7,10,13}
 * (mask 0x2480, mirroring the chip reset default — see
 * POST-MORTEM-bootloader-recovery.md in 2-Zigbee-Radio-Silabs-EFR32/),
 * which also mux-glitched the two unrelated pads B5/B6 for the duration
 * of the pulse. Only the B4 field ever mattered.
 *
 * Userspace triggers a pulse with:
 *   echo 1 > /sys/module/rtl8196e_uart_bridge/parameters/nrst_pulse
 * The pulse is write-only (no meaningful value to read). The bridge's TCP
 * connection is not closed — in-flight UART bytes are lost during the reset,
 * which is expected. The caller (typically the recover_efr32 helper script)
 * is responsible for stopping the radio daemon (otbr-agent / cpcd / zigbeed)
 * before triggering, and restarting it after.
 */
static struct gpiod_lookup_table nrst_lookup = {
	.dev_id = NULL,		/* global lookup: matched by gpiod_get(NULL, ...) */
	.table = {
		GPIO_LOOKUP("gpio-rtl819x", 12, "efr32-nrst",
			    GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN),
		{ /* sentinel */ }
	},
};

static int param_set_nrst_pulse(const char *val, const struct kernel_param *kp)
{
	struct gpio_desc *nrst;
	bool trigger;
	int ret;

	ret = kstrtobool(val, &trigger);
	if (ret)
		return ret;
	if (!trigger)
		return 0;

	mutex_lock(&nrst_pulse_lock);
	nrst_lookup.table[0].chip_hwnum = rtl_nrst_gpio;
	gpiod_add_lookup_table(&nrst_lookup);
	/* ACTIVE_LOW + OPEN_DRAIN + OUT_HIGH: pad driven low = chip in reset */
	nrst = gpiod_get(NULL, "efr32-nrst", GPIOD_OUT_HIGH);
	gpiod_remove_lookup_table(&nrst_lookup);
	if (IS_ERR(nrst)) {
		ret = PTR_ERR(nrst);
		mutex_unlock(&nrst_pulse_lock);
		pr_err(DRV_NAME ": nrst_pulse: GPIO %d unavailable (%d)\n",
		       rtl_nrst_gpio, ret);
		return ret;
	}
	pr_info(DRV_NAME ": nrst_pulse: asserting EFR32 nRST (GPIO %d) for 100 ms\n",
		rtl_nrst_gpio);
	msleep(100);
	/* logical 0 -> open-drain release: line floats, pull-up raises nRST */
	gpiod_set_value_cansleep(nrst, 0);
	gpiod_put(nrst);
	mutex_unlock(&nrst_pulse_lock);
	pr_info(DRV_NAME ": nrst_pulse: EFR32 nRST released, chip rebooting\n");
	return 0;
}

static const struct kernel_param_ops nrst_pulse_ops = {
	.set = param_set_nrst_pulse,
};

/* nrst_gpio: which gpio-rtl819x line the pulse claims. Runtime-tunable so
 * ports of this firmware to RTL8196E twins with different nRST routing
 * don't need to patch the driver. Takes effect on the next pulse.
 */
static int param_set_nrst_gpio(const char *val, const struct kernel_param *kp)
{
	int new_v, ret;

	ret = kstrtoint(val, 0, &new_v);
	if (ret)
		return ret;
	if (new_v < 0 || new_v > 31)
		return -EINVAL;

	mutex_lock(&nrst_pulse_lock);
	rtl_nrst_gpio = new_v;
	rtl_nrst_gpio_set_by_user = true;
	mutex_unlock(&nrst_pulse_lock);
	return 0;
}

static int param_get_nrst_gpio(char *buffer, const struct kernel_param *kp)
{
	int v;

	mutex_lock(&nrst_pulse_lock);
	v = rtl_nrst_gpio;
	mutex_unlock(&nrst_pulse_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v);
}

static const struct kernel_param_ops nrst_gpio_ops = {
	.set = param_set_nrst_gpio,
	.get = param_get_nrst_gpio,
};

/*
 * blmode_pulse: reset the EFR32 INTO its bootloader, on boards that wire a
 * bootloader-entry pin (e.g. the Sengled G4 — discussion #123/#126). The
 * Gecko bootloader samples the pin after reset, so the sequence holds it
 * through and past the nRST pulse:
 *
 *   assert blmode -> assert nRST -> 100 ms -> release nRST -> 1 s
 *   (bootloader pin-sampling window) -> release blmode
 *
 * This is deliberately a SEPARATE trigger from nrst_pulse: nrst_pulse must
 * keep meaning "reset into the application" (flash_efr32.sh and
 * recover_efr32 depend on it after a flash completes) — if the presence of
 * blmode-gpios changed nrst_pulse's behaviour, every reset would land in
 * the bootloader and nothing could start the app.
 *
 * Boards without the pin (Lidl: bootloader entry is in-band via
 * universal-silabs-flasher) keep blmode_gpio at -1 and get -ENODEV here.
 * Like nrst_pulse, the caller is responsible for stopping the radio
 * daemon first, and the chip is left sitting in the Gecko bootloader
 * (Xmodem @ 115200, no flow control) until the next nrst_pulse or
 * bootloader-driven application launch.
 */
static struct gpiod_lookup_table blmode_lookup = {
	.dev_id = NULL,		/* global lookup: matched by gpiod_get(NULL, ...) */
	.table = {
		GPIO_LOOKUP("gpio-rtl819x", 13, "efr32-blmode",
			    GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN),
		{ /* sentinel */ }
	},
};

static int param_set_blmode_pulse(const char *val, const struct kernel_param *kp)
{
	struct gpio_desc *blmode, *nrst;
	bool trigger;
	int ret;

	ret = kstrtobool(val, &trigger);
	if (ret)
		return ret;
	if (!trigger)
		return 0;

	mutex_lock(&nrst_pulse_lock);
	if (rtl_blmode_gpio < 0) {
		mutex_unlock(&nrst_pulse_lock);
		pr_err(DRV_NAME ": blmode_pulse: no blmode pin on this board (blmode_gpio=-1)\n");
		return -ENODEV;
	}
	if (rtl_blmode_gpio == rtl_nrst_gpio) {
		mutex_unlock(&nrst_pulse_lock);
		pr_err(DRV_NAME ": blmode_pulse: blmode_gpio and nrst_gpio are both %d\n",
		       rtl_nrst_gpio);
		return -EINVAL;
	}

	blmode_lookup.table[0].chip_hwnum = rtl_blmode_gpio;
	gpiod_add_lookup_table(&blmode_lookup);
	/* ACTIVE_LOW + OPEN_DRAIN + OUT_HIGH: blmode asserted (pad low) */
	blmode = gpiod_get(NULL, "efr32-blmode", GPIOD_OUT_HIGH);
	gpiod_remove_lookup_table(&blmode_lookup);
	if (IS_ERR(blmode)) {
		ret = PTR_ERR(blmode);
		mutex_unlock(&nrst_pulse_lock);
		pr_err(DRV_NAME ": blmode_pulse: GPIO %d unavailable (%d)\n",
		       rtl_blmode_gpio, ret);
		return ret;
	}

	nrst_lookup.table[0].chip_hwnum = rtl_nrst_gpio;
	gpiod_add_lookup_table(&nrst_lookup);
	nrst = gpiod_get(NULL, "efr32-nrst", GPIOD_OUT_HIGH);
	gpiod_remove_lookup_table(&nrst_lookup);
	if (IS_ERR(nrst)) {
		ret = PTR_ERR(nrst);
		gpiod_set_value_cansleep(blmode, 0);
		gpiod_put(blmode);
		mutex_unlock(&nrst_pulse_lock);
		pr_err(DRV_NAME ": blmode_pulse: nRST GPIO %d unavailable (%d)\n",
		       rtl_nrst_gpio, ret);
		return ret;
	}

	pr_info(DRV_NAME ": blmode_pulse: resetting EFR32 with blmode asserted (blmode GPIO %d, nRST GPIO %d)\n",
		rtl_blmode_gpio, rtl_nrst_gpio);
	msleep(100);
	/* logical 0 -> open-drain release: line floats, pull-up raises nRST */
	gpiod_set_value_cansleep(nrst, 0);
	gpiod_put(nrst);
	/* hold blmode through the bootloader's pin-sampling window */
	msleep(1000);
	gpiod_set_value_cansleep(blmode, 0);
	gpiod_put(blmode);
	mutex_unlock(&nrst_pulse_lock);
	/*
	 * Report the sequence, not the outcome. Whether the radio actually
	 * stopped in its bootloader depends on the firmware at the other end
	 * of the pin: a Gecko bootloader built without GPIO activation never
	 * samples it and boots the application instead. Nothing on this side
	 * can observe the difference, so claiming entry here would be a guess
	 * dressed as a fact — and a misleading one in exactly the case an
	 * operator is trying to diagnose. The caller's probe establishes what
	 * the chip is really running.
	 */
	pr_info(DRV_NAME ": blmode_pulse: blmode released, reset sequence complete\n");
	return 0;
}

static const struct kernel_param_ops blmode_pulse_ops = {
	.set = param_set_blmode_pulse,
};

/* blmode_gpio: which gpio-rtl819x line blmode_pulse claims; -1 disables
 * the trigger (boards without a bootloader-entry pin). Runtime-tunable
 * like nrst_gpio; takes effect on the next pulse. */
static int param_set_blmode_gpio(const char *val, const struct kernel_param *kp)
{
	int new_v, ret;

	ret = kstrtoint(val, 0, &new_v);
	if (ret)
		return ret;
	if (new_v < -1 || new_v > 31)
		return -EINVAL;

	mutex_lock(&nrst_pulse_lock);
	rtl_blmode_gpio = new_v;
	rtl_blmode_gpio_set_by_user = true;
	mutex_unlock(&nrst_pulse_lock);
	return 0;
}

static int param_get_blmode_gpio(char *buffer, const struct kernel_param *kp)
{
	int v;

	mutex_lock(&nrst_pulse_lock);
	v = rtl_blmode_gpio;
	mutex_unlock(&nrst_pulse_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v);
}

static const struct kernel_param_ops blmode_gpio_ops = {
	.set = param_set_blmode_gpio,
	.get = param_get_blmode_gpio,
};

/* Read-only "armed" parameter: actual bridge state (vs. "enable" = intent). */
static int param_get_armed(char *buffer, const struct kernel_param *kp)
{
	bool v;

	mutex_lock(&bridge_lock);
	v = state.armed;
	mutex_unlock(&bridge_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v ? 1 : 0);
}

static const struct kernel_param_ops armed_ops = {
	.get = param_get_armed,
};

/* Read-only "stats" parameter: live counters without having to disarm. */
static int param_get_stats(char *buffer, const struct kernel_param *kp)
{
	int n;

	mutex_lock(&bridge_lock);
	/* Append-only: existing fields keep their order so ad-hoc parsers
	 * of the v1.x output keep working. */
	n = scnprintf(buffer, PAGE_SIZE,
		      "rx=%llu tx=%llu drops_nocli=%llu drops_err=%llu drops_tx=%llu xoff=%llu xon=%llu tx_pause_timeouts=%llu\n",
		      state.rx_bytes, state.tx_bytes,
		      state.drops_nocli, state.drops_err, state.drops_tx,
		      state.xoff_events, state.xon_events,
		      state.tx_pause_timeouts);
	mutex_unlock(&bridge_lock);
	return n;
}

static const struct kernel_param_ops stats_ops = {
	.get = param_get_stats,
};

/* status_led_brightness: brightness fired on the LED trigger when a TCP
 * client connects. Read by the worker thread under bridge_lock (snapshot
 * into a local before unlocking, see bridge_worker_thread); written by
 * sysfs through the same lock here. Matches the locked-getter/setter
 * pattern of every other live-tunable param in this driver.
 */
static int param_set_status_led_brightness(const char *val,
					   const struct kernel_param *kp)
{
	int new_v, ret;

	ret = kstrtoint(val, 0, &new_v);
	if (ret)
		return ret;
	if (new_v < 0 || new_v > 255)
		return -EINVAL;

	mutex_lock(&bridge_lock);
	rtl_status_led_brightness = new_v;
	mutex_unlock(&bridge_lock);
	return 0;
}

static int param_get_status_led_brightness(char *buffer,
					   const struct kernel_param *kp)
{
	int v;

	mutex_lock(&bridge_lock);
	v = rtl_status_led_brightness;
	mutex_unlock(&bridge_lock);
	return scnprintf(buffer, PAGE_SIZE, "%d\n", v);
}

static const struct kernel_param_ops status_led_brightness_ops = {
	.set = param_set_status_led_brightness,
	.get = param_get_status_led_brightness,
};

module_param_cb(tty,       &tty_ops,    NULL, 0600);
MODULE_PARM_DESC(tty,       "TTY device path (default /dev/ttyS1)");
module_param_cb(baud,      &baud_ops,   NULL, 0644);
MODULE_PARM_DESC(baud,      "UART baud rate (default 115200)");
module_param_cb(port,      &port_ops,   NULL, 0600);
MODULE_PARM_DESC(port,      "TCP listen port (default 8888)");
module_param_cb(bind_addr, &bind_ops,   NULL, 0600);
MODULE_PARM_DESC(bind_addr, "TCP bind address (default 0.0.0.0 = all interfaces)");
module_param_cb(flow_control, &flow_control_ops, NULL, 0644);
MODULE_PARM_DESC(flow_control,
	"0/none, 1/hw=RTS-CTS (default), 2/sw=XON-XOFF; set 0 for EFR32 flash; "
	"hw is clamped to sw on boards without realtek,hw-flow-control");
module_param_cb(enable,    &enable_ops, NULL, 0644);
MODULE_PARM_DESC(enable,    "1=arm bridge, 0=disarm (default 0, arm via init script)");
module_param_cb(armed,     &armed_ops,  NULL, 0444);
MODULE_PARM_DESC(armed,     "Actual bridge state (read-only)");
module_param_cb(stats,     &stats_ops,  NULL, 0444);
MODULE_PARM_DESC(stats,     "Live rx/tx/drop counters (read-only)");
module_param_cb(nrst_pulse, &nrst_pulse_ops, NULL, 0200);
MODULE_PARM_DESC(nrst_pulse,
	"Write 1 to assert EFR32 nRST for 100 ms via the nrst_gpio line "
	"(recovers stuck radio without rebooting SoC)");
module_param_cb(nrst_gpio, &nrst_gpio_ops, NULL, 0644);
MODULE_PARM_DESC(nrst_gpio,
	"gpio-rtl819x line wired to EFR32 nRST (default 12 = pad B4 on the "
	"Lidl gateway)");
module_param_cb(blmode_pulse, &blmode_pulse_ops, NULL, 0200);
MODULE_PARM_DESC(blmode_pulse,
	"Write 1 to reset the EFR32 into its bootloader: assert blmode_gpio, "
	"pulse nRST, hold blmode 1 s, release");
module_param_cb(blmode_gpio, &blmode_gpio_ops, NULL, 0644);
MODULE_PARM_DESC(blmode_gpio,
	"gpio-rtl819x line wired to the EFR32 bootloader-entry pin "
	"(-1 = board has none, default)");
module_param_cb(status_led_brightness, &status_led_brightness_ops, NULL, 0644);
MODULE_PARM_DESC(status_led_brightness,
	"Brightness 0-255 applied to the '" BRIDGE_LED_TRIG_NAME
	"' LED trigger when a client is connected (default 255)");

/* ------------------------------------------------------------------ init */

/*
 * Seed nrst_gpio, blmode_gpio and the hw-flow-control capability from an
 * optional board node:
 *
 *   radio-bridge {
 *           compatible = "realtek,rtl8196e-uart-bridge";
 *           nrst-gpios = <&gpio0 12 (GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN)>;
 *           blmode-gpios = <&gpio0 13 (GPIO_ACTIVE_LOW | GPIO_OPEN_DRAIN)>;
 *           realtek,hw-flow-control;   // omit on boards without RTS/CTS wiring
 *   };
 *
 * Only the line numbers of nrst-gpios / blmode-gpios are consumed — the
 * pulse paths keep their hardcoded ACTIVE_LOW | OPEN_DRAIN lookup flags
 * because EFR32 RESETn (and the bootloader-entry pin as wired on known
 * boards) is inherently open-drain active-low; the DT cell flags document
 * the same fact for the reader.
 *
 * "realtek,hw-flow-control" is a board-capability boolean (is RTS/CTS wired?),
 * not a firmware-mode selection: present -> flow_control defaults to hw,
 * absent-in-a-present-node -> defaults to sw, and an hw request is clamped to
 * sw on a board that lacks it. The firmware-mode choice is a separate, runtime
 * concern (radio.conf FIRMWARE_FLOW_CTRL -> the sysfs flow_control knob).
 *
 * The node is optional: absent node keeps the compiled-in Lidl defaults
 * (hw-capable, blmode_pulse disabled at -1). Explicit cmdline/sysfs writes win
 * over the DT-seeded flow_control default (see rtl_flow_control_set_by_user);
 * the capability itself is DT/compiled-only.
 */
static void __init bridge_seed_defaults_from_dt(void)
{
	struct of_phandle_args args;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "realtek,rtl8196e-uart-bridge");
	if (!np)
		return;

	if (!rtl_nrst_gpio_set_by_user &&
	    !of_parse_phandle_with_args(np, "nrst-gpios", "#gpio-cells", 0,
					&args)) {
		if (args.args_count >= 1 && args.args[0] <= 31) {
			mutex_lock(&nrst_pulse_lock);
			rtl_nrst_gpio = args.args[0];
			mutex_unlock(&nrst_pulse_lock);
		} else {
			pr_warn(DRV_NAME ": DT nrst-gpios out of range, keeping %d\n",
				rtl_nrst_gpio);
		}
		of_node_put(args.np);
	}

	if (!rtl_blmode_gpio_set_by_user &&
	    !of_parse_phandle_with_args(np, "blmode-gpios", "#gpio-cells", 0,
					&args)) {
		if (args.args_count >= 1 && args.args[0] <= 31) {
			mutex_lock(&nrst_pulse_lock);
			rtl_blmode_gpio = args.args[0];
			mutex_unlock(&nrst_pulse_lock);
		} else {
			pr_warn(DRV_NAME ": DT blmode-gpios out of range, keeping %d\n",
				rtl_blmode_gpio);
		}
		of_node_put(args.np);
	}

	/* Board capability (DT/compiled-only, never a writable param). A present
	 * node without the boolean means "board declares it has no RTS/CTS". */
	rtl_hw_fc_capable = of_property_read_bool(np, "realtek,hw-flow-control");
	if (!rtl_flow_control_set_by_user)
		rtl_flow_control = rtl_hw_fc_capable ? BRIDGE_FC_HW : BRIDGE_FC_SW;

	pr_info(DRV_NAME ": DT defaults: nrst_gpio=%d blmode_gpio=%d hw_fc_capable=%d flow_control=%d\n",
		rtl_nrst_gpio, rtl_blmode_gpio, rtl_hw_fc_capable, rtl_flow_control);
	of_node_put(np);
}

static int __init rtl8196e_uart_bridge_init(void)
{
	bridge_seed_defaults_from_dt();

	/* Register the "client connected" LED trigger so userspace can bind
	 * it to an actual LED (/sys/class/leds/<led>/trigger). The trigger
	 * persists for the lifetime of the kernel (built-in driver, no
	 * module unload path). led_trigger_event() is a no-op when nothing
	 * is bound, so it's safe to fire it unconditionally from the worker. */
	led_trigger_register_simple(BRIDGE_LED_TRIG_NAME, &bridge_led_trig);

	pr_info(DRV_NAME ": v" DRV_VERSION " (J. Nilo) - tty_port client_ops, no ldisc\n");
	return 0;
}
/*
 * Built-in only: Kconfig declares CONFIG_RTL8196E_UART_BRIDGE as bool, not
 * tristate, so =m is rejected by the build system. This is intentional —
 * late_initcall ensures we run after tty/serial and net subsystems are
 * ready, and there is no matching exit path because the driver is never
 * unloaded. (An earlier scoping considered placing the hot path in IRAM,
 * which would also have required a built-in build; that idea was shelved
 * after measurements showed it unnecessary — see DESIGN.md "Options
 * considered and dropped".)
 */
late_initcall(rtl8196e_uart_bridge_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jacques Nilo");
MODULE_DESCRIPTION("RTL8196E UART<->TCP kernel bridge");
MODULE_VERSION(DRV_VERSION);
