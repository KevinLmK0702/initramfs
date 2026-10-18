// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal bring-up init for the MT6833 (evergo) mainline port.
 *
 * It mounts the pseudo filesystems, prints what the kernel was given
 * (cmdline, device tree, kernel log) and then idles forever, so that the
 * device stays in a stable state instead of rebooting because Android's
 * init cannot cope with a mainline kernel.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAXDUMP (16 * 1024)

/* defined at the bottom; called from the slow diagnostic paths so the COM
 * port keeps answering while they block on i2c timeouts */
static void console_pump(void);
static void do_dump(void);

static void dump_file(const char *path, const char *label, int hex)
{
	char buf[MAXDUMP + 1];
	int fd, n, i;

	printf("\n--- %s (%s) ---\n", label, path);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("  <open failed: %s>\n", strerror(errno));
		return;
	}
	n = read(fd, buf, MAXDUMP);
	close(fd);
	if (n <= 0) {
		printf("  <empty/err: %s>\n", strerror(errno));
		return;
	}
	if (!hex) {
		int off = 0;

		/*
		 * Flush in <=900 byte chunks, each its own /dev/kmsg record:
		 * one 16 KiB record (the whole /proc/interrupts table) is too
		 * large for devkmsg and gets rejected - and a rejected
		 * fflush() keeps the data in the stdio buffer, which then
		 * blocks every later line from ever reaching the log.
		 * That is exactly how the heartbeat, the GICD dump and this
		 * table all went missing from b190's serial capture.
		 */
		buf[n] = 0;
		while (off < n) {
			char chunk[912];
			int len = n - off > 900 ? 900 : n - off;

			memcpy(chunk, buf + off, len);
			chunk[len] = 0;
			printf("%s", chunk);
			fflush(stdout);
			off += len;
		}
		if (buf[n - 1] != '\n')
			printf("\n");
	} else {
		for (i = 0; i < n; i += 16) {
			int j;
			printf("  %04x:", i);
			for (j = i; j < i + 16 && j < n; j++)
				printf(" %02x", (unsigned char)buf[j]);
			printf("\n");
		}
	}
}

static void dump_dir(const char *path)
{
	struct dirent *de;
	DIR *d = opendir(path);

	printf("\n--- ls %s ---\n  ", path);
	if (!d) {
		printf("<opendir failed: %s>\n", strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		printf("%s ", de->d_name);
	}
	closedir(d);
	printf("\n");
}


/* b267: the watchdog fd wdt_setup() opened down in the watchdog section;
 * the switch_root daemon inherits it -- a second open() of /dev/watchdog
 * can fail with -EBUSY while the first holder is still around, which is
 * what b266's daemon silently ran into.  The definition with the
 * initializer stays down with the other watchdog helpers. */
static int wdt_fd;

/* b264: does this mount point look like a bootable Linux rootfs? */
static int is_linux_rootfs(void)
{
	static const char *marks[] = { "/rootfs/sbin/init", "/rootfs/init",
				       "/rootfs/bin/sh", "/rootfs/bin/busybox" };
	int i;

	for (i = 0; i < (int)(sizeof(marks) / sizeof(marks[0])); i++)
		if (!access(marks[i], F_OK))
			return 1;
	return 0;
}

/*
 * b264: hand control to a real rootfs -- the same sequence the
 * MT6895-Mainline reference init uses: bind the inner pseudo filesystems,
 * move the new root over /, chroot into it and exec its init.  Only
 * reached when userdata carries a Linux rootfs (see is_linux_rootfs());
 * a stock (encrypted) Android userdata can never match, and nothing in
 * here formats or writes anything -- the read-only probe mount is the
 * only side effect before the point of no return.
 */
static void switch_root_and_exec(void)
{
	/*
	 * b266/b267: keep the TOPRGU watchdog fed across the hand-over.
	 * Alpine has no watchdog daemon yet, and silence trips the 31 s timer
	 * into a silent hard reset -- which is exactly the "the board reboots
	 * on its own before I can type anything" symptom.  b266's daemon
	 * likely lost its own open() to -EBUSY (the bring-up init already
	 * holds wdt_fd), so b267 inherits that fd instead, and logs its state
	 * to /dev/kmsg so the next expdb readback tells us what really
	 * happened.  It survives exec of /sbin/init (only the main process
	 * image is replaced).
	 */
	if (fork() == 0) {
		char b[160];
		int kfd = open("/dev/kmsg", O_WRONLY);
		int fd = wdt_fd;		/* inherited from wdt_setup() */
		int n, hb = 0;

		if (fd < 0)
			fd = open("/dev/watchdog", O_WRONLY);
		if (kfd >= 0) {
			n = snprintf(b, sizeof(b),
				     "evergo: wdt-daemon up, fd=%d (init had %d)\n",
				     fd, wdt_fd);
			write(kfd, b, n);
		}
		for (;;) {
			if (fd >= 0) {
				int r = ioctl(fd, WDIOC_KEEPALIVE, 0);

				if (kfd >= 0 && r < 0) {
					n = snprintf(b, sizeof(b),
						     "evergo: wdt-daemon KEEPALIVE failed: %s\n",
						     strerror(errno));
					write(kfd, b, n);
				}
			}
			if (kfd >= 0 && (++hb % 6) == 0) {
				struct timespec ts;

				clock_gettime(CLOCK_BOOTTIME, &ts);
				n = snprintf(b, sizeof(b),
					     "evergo: wdt-daemon alive t=%lds\n",
					     (long)ts.tv_sec);
				write(kfd, b, n);
			}
			sleep(10);
		}
	}

	mkdir("/rootfs/proc", 0755);
	mkdir("/rootfs/sys", 0755);
	mkdir("/rootfs/dev", 0755);
	mkdir("/rootfs/run", 0755);
	mkdir("/rootfs/tmp", 0755);
	mount("proc", "/rootfs/proc", "proc", MS_NOSUID | MS_NOEXEC, NULL);
	mount("sysfs", "/rootfs/sys", "sysfs", MS_NOSUID | MS_NOEXEC, NULL);
	mount("devtmpfs", "/rootfs/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	mount("tmpfs", "/rootfs/run", "tmpfs", MS_NOSUID | MS_NOEXEC, NULL);
	mount("tmpfs", "/rootfs/tmp", "tmpfs", MS_NOSUID | MS_NOEXEC, NULL);
	mkdir("/rootfs/dev/pts", 0755);
	mount("devpts", "/rootfs/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, NULL);

	printf("  handing over to the real rootfs: switch_root -> /sbin/init\n");
	fflush(stdout);

	chdir("/rootfs");
	if (mount(".", "/", NULL, MS_MOVE, NULL))
		printf("  MS_MOVE failed (%s) -- chroot without move\n",
		       strerror(errno));
	if (chroot("."))
		printf("  chroot failed: %s\n", strerror(errno));
	chdir("/");

	execl("/sbin/init", "init", NULL);
	execl("/init", "init", NULL);
	execl("/bin/sh", "sh", NULL);
	printf("  exec of init failed: %s -- staying in the initramfs\n",
	       strerror(errno));
	fflush(stdout);
}

/*
 * b262: rootfs bring-up.  UFS has produced all 59 GPT partitions since
 * b95, but nothing has ever tried to mount one.  Walk the sysfs uevent
 * files for /dev/sdcN to learn the partition names, then mount "userdata"
 * with the filesystems the kernel has (f2fs, the vendor layout, and
 * ext4).  Read-only probe first; only when the mount carries a real
 * Linux rootfs (see is_linux_rootfs()) does b264 hand over control --
 * a stock encrypted Android userdata stays untouched.
 */
static void probe_block_devices(void)
{
	char path[128], buf[512], *p, *nl, name[64];
	const char *fstypes[] = { "f2fs", "ext4" };
	int i, fd, n, userdata = 0, rc;

	printf("\n--- rootfs probe: UFS partitions ---\n");
	fflush(stdout);

	for (i = 1; i <= 64; i++) {
		snprintf(path, sizeof(path), "/sys/class/block/sdc%d/uevent", i);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			if (i == 1)
				printf("  <no sdcN uevent: %s>\n", strerror(errno));
			break;
		}
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;

		p = strstr(buf, "PARTNAME=");
		if (!p)
			continue;
		p += sizeof("PARTNAME=") - 1;
		nl = strchr(p, '\n');
		if (nl)
			*nl = 0;
		snprintf(name, sizeof(name), "%s", p);
		/*
		 * fflush every line: devkmsg_write() rejects any single write
		 * over PRINTKRB_RECORD_MAX (1024 bytes) with -EINVAL and glibc
		 * then drops the whole buffered block.  b262 lost this list and
		 * the mount results that way (a ~1.2 KB single flush).
		 */
		printf("  sdc%d %s\n", i, name);
		fflush(stdout);
		if (!strcmp(name, "userdata"))
			userdata = i;
	}

	if (!userdata) {
		printf("  <no userdata partition found>\n");
		fflush(stdout);
		return;
	}

	mkdir("/rootfs", 0755);
	snprintf(path, sizeof(path), "/dev/sdc%d", userdata);

	/*
	 * Peek at the first 64 bytes before mounting.  The kernel already
	 * logs the failed f2fs/ext4 magics; this sample says whether the
	 * partition holds a foreign filesystem, an erased area, or
	 * (metadata-)encrypted data that no plain mount can ever read.
	 */
	{
		unsigned char sb[64];
		int sfd, rn, err = 0;

		sfd = open(path, O_RDONLY);
		if (sfd < 0) {
			err = errno;
			rn = -1;
		} else {
			rn = read(sfd, sb, sizeof(sb));
			if (rn < 0)
				err = errno;
			close(sfd);
		}
		if (!err && rn == (int)sizeof(sb))
			printf("  %s first 64B: %02x %02x %02x %02x %02x %02x %02x %02x "
			       "%02x %02x %02x %02x %02x %02x %02x %02x\n",
			       path, sb[0], sb[1], sb[2], sb[3], sb[4], sb[5],
			       sb[6], sb[7], sb[8], sb[9], sb[10], sb[11],
			       sb[12], sb[13], sb[14], sb[15]);
		else
			printf("  %s read failed: %s\n", path,
			       err ? strerror(err) : "<short read>");
		fflush(stdout);
	}

	for (i = 0; i < 2; i++) {
		rc = mount(path, "/rootfs", fstypes[i], MS_RDONLY, NULL);
		printf("  mount %s as %s (ro): rc=%d (%s)\n", path, fstypes[i],
		       rc, rc ? strerror(errno) : "ok");
		fflush(stdout);
		if (rc)
			continue;

		dump_dir("/rootfs");

		if (!is_linux_rootfs()) {
			printf("  not a Linux rootfs (no /sbin/init, /init or "
			       "/bin/sh) -- leaving it alone\n");
			fflush(stdout);
			umount("/rootfs");
			continue;
		}

		/* Real rootfs found: remount read-write and hand over. */
		umount("/rootfs");
		rc = mount(path, "/rootfs", fstypes[i], 0, NULL);
		printf("  remount %s as %s (rw): rc=%d (%s)\n", path,
		       fstypes[i], rc, rc ? strerror(errno) : "ok");
		fflush(stdout);
		if (!rc)
			switch_root_and_exec();
	}
}


/*
 * The panel ("k16a_42_02_0b_vdo panel ready") and the LM36273 backlight
 * ("probe of 0-0011 with driver lm36273-backlight returned 0") both probe
 * now, but nothing turns them on: there is no userspace to do a modeset and
 * the kernel's fbdev client gives up with "[drm] Cannot find any crtc or
 * sizes".  So write the backlight directly -- one write to the class device
 * is enough to make the LED string visibly come on -- and print the state of
 * the whole display chain, so the next read-back says whether the missing
 * piece is the panel link, the CRTC, or just the modeset.
 */
static void poke_backlights(void)
{
	struct dirent *de;
	DIR *d;
	char path[256];
	char val[64];

	printf("\n--- display bring-up ---\n");
	d = opendir("/sys/class/backlight");
	if (!d) {
		printf("  <opendir /sys/class/backlight failed: %s>\n",
		       strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		long max = -1, now = -1, want;
		int fd, n;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		snprintf(path, sizeof(path),
			 "/sys/class/backlight/%s/max_brightness", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, val, sizeof(val) - 1);
			if (n > 0) {
				val[n] = 0;
				max = atol(val);
			}
			close(fd);
		}
		printf("  backlight %s: max_brightness=%ld\n", de->d_name, max);

		want = (max > 0) ? max : 255;
		snprintf(path, sizeof(path),
			 "/sys/class/backlight/%s/brightness", de->d_name);
		fd = open(path, O_WRONLY);
		if (fd < 0) {
			printf("    open %s: %s\n", path, strerror(errno));
			continue;
		}
		n = snprintf(val, sizeof(val), "%ld\n", want);
		fflush(stdout);
		console_pump();		/* the update behind this can take 8 s */
		if (write(fd, val, n) != n)
			printf("    write %s: %s\n", path, strerror(errno));
		else
			printf("    wrote %ld to %s\n", want, path);
		close(fd);
		console_pump();

		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, val, sizeof(val) - 1);
			if (n > 0) {
				val[n] = 0;
				now = atol(val);
			}
			close(fd);
		}
		printf("    read back brightness=%ld\n", now);
	}
	closedir(d);

	/* "connected" plus a mode list is what a modeset needs. */
	d = opendir("/sys/class/drm");
	if (d) {
		while ((de = readdir(d))) {
			if (strncmp(de->d_name, "card", 4) ||
			    !strchr(de->d_name, '-'))
				continue;
			snprintf(path, sizeof(path),
				 "/sys/class/drm/%s/status", de->d_name);
			dump_file(path, de->d_name, 0);
			snprintf(path, sizeof(path),
				 "/sys/class/drm/%s/modes", de->d_name);
			dump_file(path, de->d_name, 0);
		}
		closedir(d);
	}
	dump_dir("/sys/class/drm");
	dump_dir("/sys/class/graphics");
	/* Is fbcon actually bound to the framebuffer?  This is the difference
	 * between "the panel shows the log" and "there is a framebuffer nobody
	 * writes a console to". */
	dump_dir("/sys/class/vtconsole");
	{
		DIR *v = opendir("/sys/class/vtconsole");

		if (v) {
			while ((de = readdir(v))) {
				if (strncmp(de->d_name, "vtcon", 5))
					continue;
				snprintf(path, sizeof(path),
					 "/sys/class/vtconsole/%s/name", de->d_name);
				dump_file(path, de->d_name, 0);
				snprintf(path, sizeof(path),
					 "/sys/class/vtconsole/%s/bind", de->d_name);
				dump_file(path, de->d_name, 0);
			}
			closedir(v);
		}
	}
	dump_dir("/sys/class/i2c-dev");
	dump_file("/sys/bus/i2c/devices/0-0011/name", "i2c 0-0011 name", 0);
}


/* ---------------------------------------------------------------------- *
 * USB serial console (the kernel's legacy g_serial).
 *
 * There is no UART on this board, and a boot that hangs never reaches the
 * reboot that would flush our ring into expdb -- so the COM port is the only
 * live channel: g_serial binds the UDC by itself at boot and creates
 * /dev/ttyGS0, and everything that goes through printk (the whole boot log,
 * because init dup2()s /dev/kmsg onto stdout) is streamed to that port, with
 * a lone 'r' on the port asking the board to reboot.
 * ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------- *
 * Raw register access for the diagnostics below.
 *
 * Everything MMIO here goes through /dev/mem with mmap(): pread() is
 * rejected on arm64 for MMIO.
 *
 * (b189 dropped the old MUSB_POWER / USB-PHY pokes: the board enumerates on
 * its own - the COM port has come up on b187 and b188 even though those
 * pokes never got to run - and writing the PHY behind a live gadget is a
 * needless risk in an init that is finally running to completion.)
 * ---------------------------------------------------------------------- */
#define MUSB_WIN             0x1000

static void *mmio_map(unsigned long phys, size_t len)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *p;

	if (fd < 0)
		return NULL;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, phys);
	close(fd);
	return p == MAP_FAILED ? NULL : p;
}

static void udc_report(void)
{
	char path[256];
	struct dirent *de;
	DIR *d = opendir("/sys/class/udc");

	printf("\n--- usb: udc state ---\n");
	if (!d) {
		printf("  <no /sys/class/udc: %s>\n", strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		/*
		 * Read-only: poking soft_connect disconnects an already bound and
		 * enumerated gadget, which on Windows shows up as a dead COM port
		 * whose writes end in "semaphore timeout".  The built-in g_serial
		 * has the UDC up from boot anyway.
		 */
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", de->d_name);
		dump_file(path, de->d_name, 0);
		snprintf(path, sizeof(path), "/sys/class/udc/%s/soft_connect",
			 de->d_name);
		dump_file(path, "soft_connect", 0);
	}
	closedir(d);
}

static void usb_bringup(void)
{
	/*
	 * b189 dropped the MUSB/PHY register pokes: the port enumerates on its
	 * own (it came up on b187/b188 even though the pokes never ran), and
	 * writing PHY registers behind a live gadget is a needless risk.
	 */
	udc_report();
}

/*
 * Where does the DSI interrupt die?
 *
 * SPI 269 -> sysirq hwirq 269 -> GIC INTID 301 = bank 9 bit 13 (GICv3 adds
 * 32 to the DT SPI number).  The whole chain watched from one place:
 *
 *   DSI block @0x14013000 (INTEN/INTSTA)
 *     -> MTK intpol block @0xc53a650 (the "sysirq" polarity matrix; word 8
 *        bit 13 belongs to line 269, word 9 to lines 288+)
 *     -> GICD @0xc000000 (enable/pending/active/type/route)
 *
 * Read-only: a pending bit with the line dark just means some other block
 * on that bank keeps a level asserted; the real verdict comes from the
 * synthetic test behind console key 'g'.
 */
/* b248: list input devices (touchscreen brings up /dev/input/eventN) */
static void dump_input(void)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *de;
	int cnt = 0;

	if (!d) {
		printf("  input : no /sys/class/input\n");
		return;
	}
	while ((de = readdir(d)) != NULL) {
		char path[160], name[128] = "?";
		int fd, n;

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/name",
			 de->d_name);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, name, sizeof(name) - 1);
			close(fd);
			if (n > 0)
				name[n - 1] = 0;
		}
		printf("  input : %s -> %s\n", de->d_name, name);
		fflush(stdout);
		cnt++;
	}
	closedir(d);
	if (!cnt)
		printf("  input : (none)\n");
	fflush(stdout);
}

/* b253: touch pad forensics.  MODE registers live in the same 0x10005000
 * window dw reads safely: pins 8-15 @0x310, 24-31 @0x330, 4 bits per pin.
 * mode 1 = SPI1 function, 0 = plain GPIO. */
static void dump_touch_pads(void)
{
	volatile unsigned int *g = mmio_map(0x10005000UL, 0x1000);
	unsigned int m1, m2, dir, dout, din;

	if (!g) {
		printf("  pads  : mmap failed: %s\n", strerror(errno));
		return;
	}
	m1 = g[0x0310 / 4];	/* pins 8..15  */
	m2 = g[0x0330 / 4];	/* pins 24..31 */
	dir = g[0x0000 / 4];
	dout = g[0x0100 / 4];
	din = g[0x0200 / 4];
	printf("  pads  : mode13=%x mode14=%x mode27=%x mode28=%x mode29=%x mode30=%x\n",
	       (m1 >> 20) & 0xf, (m1 >> 24) & 0xf,
	       (m2 >> 12) & 0xf, (m2 >> 16) & 0xf,
	       (m2 >> 20) & 0xf, (m2 >> 24) & 0xf);
	printf("  pads  : dir13=%u dir14=%u dir27=%u dir28=%u dir29=%u dir30=%u\n",
	       (dir >> 13) & 1, (dir >> 14) & 1, (dir >> 27) & 1,
	       (dir >> 28) & 1, (dir >> 29) & 1, (dir >> 30) & 1);
	printf("  pads  : do13=%u do14=%u do27=%u do28=%u do29=%u do30=%u\n",
	       (dout >> 13) & 1, (dout >> 14) & 1, (dout >> 27) & 1,
	       (dout >> 28) & 1, (dout >> 29) & 1, (dout >> 30) & 1);
	printf("  pads  : di13=%u di14=%u di27=%u di28=%u di29=%u di30=%u\n",
	       (din >> 13) & 1, (din >> 14) & 1, (din >> 27) & 1,
	       (din >> 28) & 1, (din >> 29) & 1, (din >> 30) & 1);
	fflush(stdout);
	munmap((void *)g, 0x1000);
}

static void hw_diag_irq(void)
{
	volatile unsigned int *g = mmio_map(0xc000000UL, 0x8000);
	/* 0xc53a650 is not page-aligned (mmap rejects it): map the page */
	volatile unsigned int *p = mmio_map(0xc53a000UL, 0x1000);
	volatile unsigned int *d = mmio_map(0x14013000UL, 0x1000);

	printf("\n--- hw: where does the DSI IRQ die? ---\n");
	if (g) {
		printf("  gicd b8: pend=%08x en=%08x act=%08x icfgr17=%08x "
		       "group8=%08x\n",
		       g[0x220 / 4], g[0x120 / 4], g[0x320 / 4],
		       g[0xc44 / 4], g[0x0a0 / 4]);
		printf("  gicd b9: pend=%08x en=%08x act=%08x icfgr18=%08x "
		       "group9=%08x\n",
		       g[0x224 / 4], g[0x124 / 4], g[0x324 / 4],
		       g[0xc48 / 4], g[0x0a4 / 4]);
		printf("  gicd spi269(hw301): prio=%08x router=%08x.%08x\n",
		       g[0x52c / 4], g[0x696c / 4], g[0x6968 / 4]);
		/* b265: flush in small batches -- devkmsg rejects any single
		 * write over 1024 bytes and the whole block is then lost */
		fflush(stdout);
		munmap((void *)g, 0x8000);
	} else {
		printf("  gicd: mmap failed: %s\n", strerror(errno));
	}
	if (p)		/* word 7 starts at byte 0x650: p[0x194 + 7] */
		printf("  intpol w7/w8/w9: %08x %08x %08x\n",
		       p[0x194 + 7], p[0x194 + 8], p[0x194 + 9]);
	else
		printf("  intpol: mmap failed: %s\n", strerror(errno));
	if (d)
		printf("  dsi: inten=%08x intsta=%08x con=%08x mode=%08x\n",
		       d[0x08 / 4], d[0x0c / 4], d[0x10 / 4], d[0x14 / 4]);
	else
		printf("  dsi: mmap failed: %s\n", strerror(errno));
	fflush(stdout);
	/* b236: is the display pipeline itself alive? (mutex0/ovl0/rdma0) */
	{
		volatile unsigned int *mx = mmio_map(0x14001000UL, 0x1000);
		volatile unsigned int *ov = mmio_map(0x14005000UL, 0x1000);
		volatile unsigned int *rd = mmio_map(0x14007000UL, 0x1000);

		if (mx)
			printf("  mutex : en=%08x mtx=%08x rst=%08x sof=%08x "
			       "mod0=%08x mod1=%08x intsta=%08x\n",
			       mx[0x20 / 4], mx[0x24 / 4], mx[0x28 / 4],
			       mx[0x2c / 4], mx[0x30 / 4], mx[0x34 / 4],
			       mx[0x04 / 4]);
		else
			printf("  mutex : mmap failed: %s\n", strerror(errno));
		if (ov)
			printf("  ovl   : con=%08x inten=%08x intsta=%08x "
			       "en=%08x dbg=%08x\n",
			       ov[0x00 / 4], ov[0x04 / 4], ov[0x08 / 4],
			       ov[0x0c / 4], ov[0x240 / 4]);
		else
			printf("  ovl   : mmap failed: %s\n", strerror(errno));
		{
			volatile unsigned int *ms = mmio_map(0x14000000UL, 0x2000);

			if (ms) {
				printf("  mmsys : ovl0_mout(f18)=%08x rdma0_sel(f28)=%08x "
				       "dsi0_sel(f30)=%08x f14=%08x f10=%08x\n",
				       ms[0xf18 / 4], ms[0xf28 / 4], ms[0xf30 / 4],
				       ms[0xf14 / 4], ms[0xf10 / 4]);
				munmap((void *)ms, 0x2000);
			} else
				printf("  mmsys : mmap failed: %s\n", strerror(errno));
		}
		fflush(stdout);
		dump_input();
		dump_touch_pads();
		if (rd)
			printf("  rdma  : inten=%08x intsta=%08x gcon=%08x "
			       "sz0=%08x sz1=%08x pitch=%08x addr=%08x\n",
			       rd[0x00 / 4], rd[0x04 / 4], rd[0x10 / 4],
			       rd[0x14 / 4], rd[0x18 / 4], rd[0x2c / 4],
			       rd[0xf00 / 4]);
		else
			printf("  rdma  : mmap failed: %s\n", strerror(errno));
		fflush(stdout);
		if (mx)
			munmap((void *)mx, 0x1000);
		if (ov)
			munmap((void *)ov, 0x1000);
		if (rd)
			munmap((void *)rd, 0x1000);
	}
	fflush(stdout);
}

/* first CPU count of an /proc/interrupts row matching %name; -1 if absent */
static int irq_row(const char *name, char *copy, int sz)
{
	FILE *f = fopen("/proc/interrupts", "r");
	char line[512];

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char *c, *nl;

		if (!strstr(line, name))
			continue;
		if (copy) {
			snprintf(copy, sz, "%s", line);
			nl = strchr(copy, '\n');
			if (nl)
				*nl = 0;
		}
		c = strchr(line, ':');
		fclose(f);
		return c ? (int)strtol(c + 1, NULL, 10) : -1;
	}
	fclose(f);
	return -1;
}

/*
 * Console key 'g': the cleanest experiment left while the display stays
 * off.  Software can set the pending bit of any SPI right in the
 * distributor, so poke both the DSI line (INTID 301) and the OVL line
 * (INTID 287, which has really delivered interrupts before - the control)
 * and watch whose /proc counter moves:
 *
 *   both move      -> the whole delivery path works; whatever loses the
 *                     DSI interrupt sits upstream (the block never asserts,
 *                     or the intpol matrix routes it nowhere);
 *   only OVL moves -> INTID 301 specifically is stuck inside the GIC; the
 *                     register dump next to it shows which field differs.
 */
static void gic_deliver_test(void)
{
	volatile unsigned int *g = mmio_map(0xc000000UL, 0x8000);
	char row[200];
	int a137, a153, b137, b153;

	printf("\n=== gic delivery test (synthetic pending) ===\n");
	if (!g) {
		printf("  gicd: mmap failed: %s\n", strerror(errno));
		return;
	}
	a137 = irq_row("14005000.ovl", row, sizeof(row));
	printf("  before ovl: %s\n", row[0] ? row : "<not in /proc/interrupts>");
	a153 = irq_row("14013000.dsi", row, sizeof(row));
	printf("  before dsi: %s\n", row[0] ? row : "<not in /proc/interrupts>");

	/* set pending: ovl = bank 8 bit 31, dsi = bank 9 bit 13;
	 * GICD_ISPENDRn lives at 0x200 + n*4 */
	g[0x220 / 4] = 1u << 31;
	g[0x224 / 4] = 1u << 13;
	usleep(2000);
	printf("  set-> b8=%08x b9=%08x (en8=%08x en9=%08x)\n",
	       g[0x220 / 4], g[0x224 / 4], g[0x120 / 4], g[0x124 / 4]);

	usleep(200000);
	b137 = irq_row("14005000.ovl", row, sizeof(row));
	printf("  after ovl: %s\n", row[0] ? row : "<gone>");
	b153 = irq_row("14013000.dsi", row, sizeof(row));
	printf("  after dsi: %s\n", row[0] ? row : "<gone>");
	printf("  counters: ovl %d -> %d, dsi %d -> %d\n",
	       a137, b137, a153, b153);
	printf("  pending now: b8=%08x b9=%08x\n",
	       g[0x220 / 4], g[0x224 / 4]);

	if (b137 > a137)
		printf("  -> ovl line delivered: control ok\n");
	else
		printf("  -> ovl line NOT delivered: the test itself is broken\n");
	if (b153 > a153)
		printf("  -> dsi line delivered: GIC/CPU/handler path is FINE\n");
	else
		printf("  -> dsi line NOT delivered while the control is: "
		       "INTID 301 is stuck in the GIC\n");

	/* drop anything still set by the poke so real lines stay clean:
	 * GICD_ICPENDRn at 0x280 + n*4 */
	if (g[0x220 / 4] & (1u << 31))
		g[0x2a0 / 4] = 1u << 31;
	if (g[0x224 / 4] & (1u << 13))
		g[0x2a4 / 4] = 1u << 13;
	munmap((void *)g, 0x8000);
	fflush(stdout);
}


/* ---------------------------------------------------------------------- *
 * Two instruments for the dead backlight.
 *
 * The LM36273 never answers: every register write from the driver ends in
 * -ETIMEDOUT, even though the controller probes and the chip is wired the
 * same way the vendor kernel has it.  So measure instead of guessing:
 *
 *  - probe address 0x11 on every /dev/i2c-* bus.  An ACK anywhere means the
 *    bus and the chip are fine and the problem is inside the driver; no ACK
 *    anywhere means the bus itself (clock, pins, or the chip's supply).
 *    i2c-dev is built in (CONFIG_I2C_CHARDEV) so this needs no driver help.
 *  - dump the i2c/dsi/mipi/display lines of the clock tree, because a bus
 *    master with a wrong (or unparented) "main" clock produces exactly this
 *    silent timeout.
 *
 * struct i2c_msg and the ioctls are spelled out here so the initramfs build
 * does not depend on the userspace i2c headers of whatever host compiles it.
 * ---------------------------------------------------------------------- */
#define EG_I2C_M_RD          0x0001
#define EG_I2C_RDWR          0x0707

struct eg_i2c_msg {
	unsigned short addr;
	unsigned short flags;
	unsigned short len;
	unsigned char *buf;
};

struct eg_i2c_rdwr_ioctl_data {
	struct eg_i2c_msg *msgs;
	unsigned int nmsgs;
};

/*
 * Read side of the probe: write-then-read of register 0x03 (the same
 * shape as the driver's sanity read, which is the only read sequence
 * proven to work on this IP).  A pure single-message read wedges the
 * LM36273 mid-transfer -- afterwards the chip holds SDA low and every
 * address "ACKs" (b221 sweep: 111 ack, all fake).  Returns 0 on
 * success.
 */
static int i2c_ping(int bus, int addr)
{
	char path[64];
	unsigned char reg = 0x03;
	unsigned char val = 0;
	struct eg_i2c_msg msg[2];
	struct eg_i2c_rdwr_ioctl_data io;
	int fd, ret;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -errno;
	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &reg;
	msg[1].addr = addr;
	msg[1].flags = EG_I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = &val;
	io.msgs = msg;
	io.nmsgs = 2;
	ret = ioctl(fd, EG_I2C_RDWR, &io);
	close(fd);
	return ret < 0 ? -errno : 0;
}

/* b227: like i2c_ping(), but hands back the byte the chip returned */
static int i2c_ping_val(int bus, int addr)
{
        char path[64];
        unsigned char reg = 0x03;
        unsigned char val = 0;
        struct eg_i2c_msg msg[2];
        struct eg_i2c_rdwr_ioctl_data io;
        int fd, ret;

        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        fd = open(path, O_RDWR);
        if (fd < 0)
                return -errno;
        msg[0].addr = addr;
        msg[0].flags = 0;
        msg[0].len = 1;
        msg[0].buf = &reg;
        msg[1].addr = addr;
        msg[1].flags = EG_I2C_M_RD;
        msg[1].len = 1;
        msg[1].buf = &val;
        io.msgs = msg;
        io.nmsgs = 2;
        ret = ioctl(fd, EG_I2C_RDWR, &io);
        close(fd);
        return ret < 0 ? -errno : val;
}

/*
 * Write side of the probe: a one-byte write gets a clean ACK/NACK from
 * the controller while reads on this IP wedge the chip (b221), so the
 * bus sweep uses these.  Returns 0 on ACK.
 */
static int i2c_ping_wr(int bus, int addr)
{
        char path[64];
        unsigned char buf[2] = { 0x03, 0xcd };
        struct eg_i2c_msg msg[1];
        struct eg_i2c_rdwr_ioctl_data io;
        int fd, ret;

        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        fd = open(path, O_RDWR);
        if (fd < 0)
                return -errno;
        msg[0].addr = addr;
        msg[0].flags = 0;
        msg[0].len = 1;
        msg[0].buf = buf;
        io.msgs = msg;
        io.nmsgs = 1;
        ret = ioctl(fd, EG_I2C_RDWR, &io);
        close(fd);
        return ret < 0 ? -errno : 0;
}

/*
 * Backlight: the LM36273 talks i2c again (mt2712 layout fix), so the only
 * missing piece for a visible screen is a brightness write.  'b' = half,
 * 'B' = full.
 */
static void backlight_set(int full)
{
	struct dirent *de;
	DIR *d = opendir("/sys/class/backlight");
	char path[192], buf[32];
	int found = 0;

	if (!d) {
		printf("backlight: %s\n", strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		unsigned long max = 0, lvl;
		int fd, n;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path),
			 "/sys/class/backlight/%s/max_brightness", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (n > 0) {
				buf[n] = 0;
				max = strtoul(buf, NULL, 10);
			}
		}
		lvl = full ? max : (max ? max / 2 : 2048);
		snprintf(path, sizeof(path),
			 "/sys/class/backlight/%s/brightness", de->d_name);
		fd = open(path, O_WRONLY);
		if (fd < 0) {
			printf("backlight %s: open: %s\n", de->d_name,
			       strerror(errno));
			continue;
		}
		n = snprintf(buf, sizeof(buf), "%lu\n", lvl);
		if (write(fd, buf, n) < 0)
			printf("backlight %s: write %lu failed: %s\n",
			       de->d_name, lvl, strerror(errno));
		else
			printf("backlight %s: set to %lu (max %lu)\n",
			       de->d_name, lvl, max);
		close(fd);
		found++;
		fflush(stdout);
	}
	closedir(d);
	if (!found)
		printf("backlight: no device under /sys/class/backlight\n");
}

static void wr_file(const char *path, const char *val)
{
        FILE *f = fopen(path, "w");

        if (!f)
                return;
        fputs(val, f);
        fclose(f);
}

/*
 * b228: panel/bias pin monitor.  The GPIO data registers are bit-per-pin
 * at 0x10005000 (DIR 0x0000 + group*0x10, DO +0x0100, DI +0x0200; the
 * layout is straight from pinctrl-mt6833.c).  Printed every sample: the
 * 1.13 s NACK has to come from an external actor and a pin moving right
 * then would name it (prime suspect: the first DRM modeset driving the
 * panel; fbcon=nodefer makes it happen around that time).
 */
static void dw_pins(long long ms)
{
        static const struct {
                unsigned int pin, grp, bit;
        } p[] = {
                { 87, 2, 23 }, { 109, 3, 13 }, { 110, 3, 14 },
                { 137, 4, 9 }, { 138, 4, 10 },
                /* b253: touch pads -- 13=reset, 14=INT, 27-30=SPI1 */
                { 13, 0, 13 }, { 14, 0, 14 },
                { 27, 0, 27 }, { 28, 0, 28 }, { 29, 0, 29 }, { 30, 0, 30 },
        };
        static volatile unsigned int *g;
        unsigned int i;

        if (!g)
                g = mmio_map(0x10005000UL, 0x1000);
        if (!g)
                return;
        printf("dw t=%lldms pins:", ms);
        for (i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
                unsigned int dir, out, in;

                dir = (g[(0x0000 + p[i].grp * 0x10) / 4] >> p[i].bit) & 1;
                out = (g[(0x0100 + p[i].grp * 0x10) / 4] >> p[i].bit) & 1;
                in = (g[(0x0200 + p[i].grp * 0x10) / 4] >> p[i].bit) & 1;
                printf(" %u:%c%d%d", p[i].pin, dir ? 'O' : 'I', out, in);
        }
        printf("\n");
}

/*
 * b227/b228: i2c6 death watch.  b227 pinned the death to (0.83 s, 1.13 s):
 * the chip ACKs, then NACKs, then the controller times out -- with no i2c
 * traffic in the window, so an external actor must be pulling the rug
 * (prime suspect: the first DRM modeset driving panel GPIOs / bias /
 * backlight around that time; fbcon=nodefer fires it early).  The forked
 * sampler therefore tracks every 0.2 s: the ping result (the only
 * reliable signal), the two live clock-gate registers, the panel/bias
 * pin levels, and the clk trace events filtered to i2c/iic/imp names.
 */
static void i2c_deathwatch(void)
{
        volatile unsigned int *top = mmio_map(0x10000000UL, 0x1000);
        volatile unsigned int *inf = mmio_map(0x10001000UL, 0x1000);
        static const char *ev[] = {
                "clk_enable", "clk_disable", "clk_disable_complete",
                "clk_set_rate", "clk_set_parent",
        };
        const char *tdir = NULL;
        char path[192], vbuf[32];
        struct timespec ts;
        long long ms;
        unsigned int i;
        int ret, first_fail = -1;

        setvbuf(stdout, NULL, _IONBF, 0);

        /* tracefs is its own filesystem on this kernel */
        mkdir("/sys/kernel/tracing", 0755);
        mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL);
        if (!access("/sys/kernel/tracing/trace", F_OK))
                tdir = "/sys/kernel/tracing";
        else if (!access("/sys/kernel/debug/tracing/trace", F_OK))
                tdir = "/sys/kernel/debug/tracing";
        else
                printf("dw: no tracefs (tried /sys/kernel/tracing and "
                       "debugfs)\n");

        if (tdir) {
                snprintf(path, sizeof(path), "%s/buffer_size_kb", tdir);
                wr_file(path, "64");
                for (i = 0; i < sizeof(ev) / sizeof(ev[0]); i++) {
                        snprintf(path, sizeof(path),
                                 "%s/events/clk/%s/filter", tdir, ev[i]);
                        wr_file(path, "name ~ \"*i2c*\" || name ~ \"*iic*\" "
                                      "|| name ~ \"*imp*\"");
                        snprintf(path, sizeof(path),
                                 "%s/events/clk/%s/enable", tdir, ev[i]);
                        wr_file(path, "1");
                }
                snprintf(path, sizeof(path), "%s/tracing_on", tdir);
                wr_file(path, "1");
        }

        for (i = 0; i < 34; i++) {
                long gap = i < 18 ? 200 : (i < 30 ? 500 : 5000);

                usleep(gap * 1000);
                clock_gettime(CLOCK_BOOTTIME, &ts);
                ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;

                ret = i2c_ping_val(0, 0x11);
                if (ret >= 0)
                        snprintf(vbuf, sizeof(vbuf), "OK(%02x)", ret);
                else
                        snprintf(vbuf, sizeof(vbuf), "%d", ret);
                if (ret < 0 && first_fail < 0)
                        first_fail = i;

                printf("dw t=%lldms: ping=%s | cfg10=%08x ifrao=%08x\n",
                       ms, vbuf,
                       top ? top[0xb0 / 4] : 0xdeadU,
                       inf ? inf[0x90 / 4] : 0xdeadU);
                dw_pins(ms);
                fflush(stdout);

                if (i == 17 && tdir) {
                        snprintf(path, sizeof(path), "%s/tracing_on", tdir);
                        wr_file(path, "0");
                        printf("dw: first failed ping at sample %d\n",
                               first_fail);
                        printf("=== dw: clk events (i2c/iic/imp) ===\n");
                        snprintf(path, sizeof(path), "%s/trace", tdir);
                        dump_file(path, "clk trace", 0);
                        printf("=== dw: end of clk events ===\n");
                        fflush(stdout);
                }
        }
        _exit(0);
}

/*
 * i2c6 register forensics.  This SoC's i2c IP is the mt2712/mt8173 (v1)
 * flavour - FIFO_STAT 0x30, DEBUGSTAT 0x64, DEBUGCTRL 0x68, CLOCK_DIV 0x70,
 * DCM 0x54 - and the DTS used to select "mt8183", whose v2 offsets put
 * every FIFO/timing register at a wrong address (all transfers -110).
 * DEBUGSTAT exposes the live SCL/SDA pad levels: both high + a failing
 * transfer points at the controller side, a stuck-low line at the bus
 * being held down (unpowered slave, missing pull-up, dead pinmux).
 */
static void i2c6_regs(const char *tag)
{
	/*
	 * b230: no register dump any more.  b229 proved that a single CPU
	 * read of the 0x11fxxxxx window (i2c6 / imp_iic_wrap_n) while its
	 * clocks are cut kills the whole segment ~0.3 s later -- the chip
	 * stops ACKing, then the controller times out.  The dump is not
	 * worth losing the bus over (b228: no reads -> 34/34 pings OK).
	 */
	printf("  i2c6[%s]: (skipped; clock-off reads are poison)\n", tag);
}

/*
 * /proc/interrupts sampler: if the (unclocked?) i2c6 block parks its
 * level-triggered IRQ line high, the GIC re-delivers it forever and the
 * per-CPU count explodes; comparing before/after the scan shows it.
 */
static void irq_sample(const char *tag)
{
	FILE *f = fopen("/proc/interrupts", "r");
	char line[256];

	if (!f) {
		printf("  ints[%s]: %s\n", tag, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "i2c") && !strstr(line, "watchdog"))
			continue;
		printf("  ints[%s]:%s", tag, line);
	}
	fclose(f);
}

/*
 * The i2c6 / imp_iic_wrap_n window (0x11fxxxxx) must not be touched while
 * its clocks are cut.  The old story ("reads a constant 0xf00, the block
 * is missing a clock, force the gates open") turned out to be backwards:
 * b229 showed that a single CPU read of impn 0xe00 (or of the controller
 * registers) with the clocks cut kills the whole segment about 0.3 s
 * later -- the chip stops ACKing, then the controller times out -- while
 * b228 showed that leaving the window alone keeps the bus alive for as
 * long as anyone cares to ping.  Only topckgen / infracfg_ao (always
 * clocked) are read below, and only for the log.
 */
static void i2c_clk_probe(void)
{
	volatile unsigned int *top = mmio_map(0x10000000UL, 0x1000);
	volatile unsigned int *inf = mmio_map(0x10001000UL, 0x1000);

	/*
	 * b230: the old body read and wrote the 0x11fxxxxx window (i2c6 +
	 * imp_iic_wrap_n) to try to "wake" the block.  b229 proved that one
	 * read of impn 0xe00 with the clocks cut kills the segment ~0.3 s
	 * later, and b228 proved that leaving it alone keeps the bus alive
	 * indefinitely.  Only the always-clocked topckgen / infracfg_ao
	 * registers are read now -- and only for the log.
	 */
	if (!top || !inf) {
		printf("  clk-probe: mmap failed\n");
		return;
	}
	printf("  gates: cfg10=%08x upd1=%08x ifrao_sta=%08x\n",
	       top[0x0b0 / 4], top[0x08 / 4], inf[0x90 / 4]);
	munmap((void *)top, 0x1000);
	munmap((void *)inf, 0x1000);
}

static void i2c_scan(void)
{
	char path[64], link[256];
	int bus, found = 0, dumped = 0;

	printf("\n--- i2c: who answers at 0x11? ---\n");
	i2c6_regs("idle");
	i2c_clk_probe();
	irq_sample("before");
	for (bus = 0; bus < 16; bus++) {
		char buf[192];
		FILE *f;
		int ret, wret, n, a;

		snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
		if (access(path, F_OK))
			continue;
		snprintf(path, sizeof(path),
			 "/sys/class/i2c-dev/i2c-%d/name", bus);
		f = fopen(path, "r");
		printf("  adapter i2c-%d:", bus);
		if (f) {
			if (fgets(buf, sizeof(buf), f)) {
				buf[strcspn(buf, "\n")] = 0;
				printf(" name=%s", buf);
			}
			fclose(f);
		} else {
			printf(" name=<none>");
		}
		snprintf(path, sizeof(path),
			 "/sys/class/i2c-dev/i2c-%d/device/of_node", bus);
		n = readlink(path, link, sizeof(link) - 1);
		if (n > 0) {
			link[n] = 0;
			printf(" dt-node=%s\n", link);
		} else {
			printf(" dt-node=<none>\n");
		}
		snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
		/* b225: announce before each call so a hard hang cannot eat
		 * the evidence of how far the test got. */
		printf("  %s 0x11 read ping...\n", path);
		fflush(stdout);
		ret = i2c_ping(bus, 0x11);
		printf("  %s 0x11: %s\n", path,
		       ret == 0 ? "ACK (chip is there!)" : strerror(-ret));
		printf("  %s 0x11 wr ping...\n", path);
		fflush(stdout);
		wret = i2c_ping_wr(bus, 0x11);
		printf("  %s 0x11 wr: %s\n", path,
		       wret == 0 ? "ACK (chip is there!)" : strerror(-wret));
		fflush(stdout);
		if (!dumped)
			i2c6_regs("after ping");
		console_pump();		/* each ping can burn a 2 s timeout */
		if (ret == 0 || wret == 0)
			found++;

		/*
		 * Sweep with write pings only: a write gets a clean ACK/NACK,
		 * while reads on this IP wedge the controller (b213 scan died
		 * mid-sweep).  Never abort on a timeout -- one bad address must
		 * not hide the rest of the bus.
		 */
		printf("  --- sweep on %s (write pings) ---\n", path);
		{
			int ns = 0, nto = 0;

			for (a = 0x08; a <= 0x77; a++) {
				if (a == 0x11)
					continue;
				/* b225: print + flush before the ping (hang forensics) */
				printf("    ping 0x%02x ", a);
				fflush(stdout);
				wret = i2c_ping_wr(bus, a);
				if (wret == 0) {
					printf("-> ACK\n");
					ns++;
				} else if (wret == -ETIMEDOUT) {
					printf("-> timeout\n");
					nto++;
				} else {
					printf("-> %s\n", strerror(-wret));
				}
				fflush(stdout);
				console_pump();
			}
			printf("  --- sweep done: %d ack, %d timeouts ---\n",
			       ns, nto);
		}
	}
	irq_sample("after");
	if (!found)
		printf("  => no ACK: check the i2c6/pinctrl dumps above "
		       "(layout, pinmux, pad levels, chip supply)\n");
}

/*
 * Whether GPIO109/110 are really muxed to SCL6/SDA6 is the other half of a
 * dead bus: a wrong pinmux looks exactly like a missing pull-up (every
 * transfer times out, no device answers, the controller still probes).
 */
static void pinctrl_report(void)
{
	static const char *files[] = {
		"/sys/kernel/debug/pinctrl/10005000.pinctrl/pinmux-pins",
		"/sys/kernel/debug/pinctrl/10005000.pinctrl/pinconf-pins",
		"/sys/kernel/debug/pinctrl/pinctrl-maps",
	};
	unsigned int i;

	printf("\n--- pinctrl: i2c6 pins (109/110) ---\n");
	for (i = 0; i < 3; i++) {
		FILE *f = fopen(files[i], "r");
		char line[512];
		int hits = 0;

		if (!f) {
			printf("  <%s: %s>\n", files[i], strerror(errno));
			continue;
		}
		printf("  %s:\n", files[i]);
		while (fgets(line, sizeof(line), f)) {
			if (!strstr(line, "109") && !strstr(line, "110") &&
			    !strstr(line, "SCL6") && !strstr(line, "SDA6") &&
			    !strstr(line, "i2c6"))
				continue;
			printf("    %s", line);
			if (++hits > 6)
				break;
		}
		fclose(f);
	}
}

static void clk_report(void)
{
	FILE *f = fopen("/sys/kernel/debug/clk/clk_summary", "r");
	char line[512];
	int n = 0;

	printf("\n--- clocks (i2c/dsi/mipi/disp only) ---\n");
	if (!f) {
		printf("  <clk_summary unavailable: %s>\n", strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "i2c") && !strstr(line, "dsi") &&
		    !strstr(line, "mipi") && !strstr(line, "disp") &&
		    !strstr(line, "clk_summary"))
			continue;
		printf("  %s", line);
		if (++n > 40)
			break;
	}
	fclose(f);
}

/*
 * The gadget itself is the kernel's job now: legacy g_serial bound the UDC
 * during boot and created /dev/ttyGS0 before this init runs.  b185's
 * hand-built configfs composite (acm + mass storage) is gone for good --
 * the port has to stay reliable, not featureful.
 */

/* ------------------------------------------------------------------ *
 * Host console writes cannot be fire-and-forget.
 *
 * /dev/ttyGS0 is opened O_NONBLOCK.  There is a nasty detail in gs_write():
 * it copies what fits into its FIFO and then returns that -- often short,
 * and exactly 0 -- count; it never reports -EAGAIN.  The first version
 * treated a 0-byte write as fatal, so the very first FIFO-full moment ended
 * the replay: a terminal showed the banner and one or two records and
 * nothing else.  Now a 0-byte write is just back-pressure: sleep a little
 * and try again, within a modest budget, and remember whether the whole
 * replay made it out.
 * ------------------------------------------------------------------ */
static unsigned int rx_bytes, tx_bytes, tx_lost, zero_writes;
static int host_ok;		/* a host really received / sent something */
static int host_present;	/* a USB host is attached and configured */
static int dump_ok;		/* the last replay reached the end cleanly */
static int had_host;
static int pstore_done;		/* previous-boot replay sent to this host */
static int tx_jam;		/* FIFO stuck: stop trying until it clears */

static int gs_write_all(int fd, const char *buf, int len, int budget)
{
	int off = 0, left = budget;	/* budget = ms of stalling */

	while (off < len) {
		int n = write(fd, buf + off, len - off);

		if (n > 0) {
			off += n;
			left = budget;		/* progress: refresh the budget */
			continue;
		}
		if (n == 0) {
			/*
			 * gs_write() copied what fitted into its FIFO and
			 * returned 0: the FIFO is full right now, nothing
			 * went out.  This is back-pressure, not an error --
			 * treating it as fatal is what kept the console down
			 * to a single log line.
			 */
			zero_writes++;
			if (left-- <= 0)
				break;
			usleep(1000);
			continue;
		}
		if (errno == EAGAIN || errno == EINTR) {
			if (left-- <= 0)
				break;
			usleep(1000);
			continue;
		}
		break;				/* no host (ESHUTDOWN), EIO, ... */
	}
	tx_bytes += off;
	if (off < len)
		tx_lost += len - off;
	return off;
}

/*
 * A /dev/kmsg record reads "pri,seq,usec,flags;text" -- raw noise on a serial
 * console.  Turn it into the classic dmesg shape "[    0.000000] text" so the
 * replay is readable at a glance.  Returns the new length, or 0 if the buffer
 * is not a record (then the caller sends it verbatim).
 */
static int fmt_record(char *dst, int dlen, const char *src, int slen)
{
	const char *text = NULL, *p = src;
	int i, n, rest;
	unsigned long ts = 0;

	for (i = 0; i < slen; i++)
		if (src[i] == ';') {
			text = src + i + 1;
			break;
		}
	if (!text)
		return 0;
	/* walk the numeric fields; the third one is the usec timestamp */
	for (i = 0; i < 3 && p < text; i++) {
		const char *end = p;

		while (end < text && *end != ',')
			end++;
		if (i == 2) {
			unsigned long v = 0;

			while (p < end && *p >= '0' && *p <= '9')
				v = v * 10 + (*p++ - '0');
			ts = v;
		}
		p = end + 1;
	}
	if (i != 3)
		return 0;
	n = snprintf(dst, dlen, "[%5lu.%06lu] ", ts / 1000000, ts % 1000000);
	rest = slen - (int)(text - src);
	if (rest > dlen - n - 1)
		rest = dlen - n - 1;
	memcpy(dst + n, text, rest);
	if (rest > 0 && dst[n + rest - 1] != '\n')
		dst[n + rest++] = '\n';
	dst[n + rest] = 0;
	/* devkmsg escapes embedded newlines as "\x0a"; put the real line
	 * breaks back so the terminal shows lines, not the escape */
	{
		char *p = dst;

		while ((p = strstr(p, "\\x0a"))) {
			*p = '\n';
			memmove(p + 1, p + 4, strlen(p + 4) + 1);
			p++;
		}
	}
	return strlen(dst);
}

/*
 * ~31 s after every boot the board reset itself with no message at all:
 * setup.c arms the TOPRGU watchdog (31 s, EXRST) and, empirically, nothing
 * ever fed it (not the watchdog core, not anyone else).  init takes it over:
 * the pump pets it every ~10 s while the system is well, which stops the
 * reboot loop and keeps the old safety net (an init that wedges stops
 * petting and the WDT fires; LK then dumps the rings as before).  'r' stops
 * feeding on purpose and lets the same timer produce an abnormal reset.
 */
static int wdt_fd = -1;
static int wdt_feeding = 1;
static int wdt_last_rc = 1;		/* last KEEPALIVE result */
static volatile unsigned int *toprgu;

static void wdt_setup(void)
{
	int t = 31, rc;

	toprgu = mmio_map(0x10007000UL, 0x1000);
	if (toprgu)
		printf("[evergo] wdt: toprgu mode=%08x len=%08x rst=%08x\n",
		       (unsigned)toprgu[0], (unsigned)toprgu[1],
		       (unsigned)toprgu[2]);
	wdt_fd = open("/dev/watchdog", O_WRONLY);
	if (wdt_fd < 0) {
		printf("[evergo] wdt: no /dev/watchdog (%s) - 31 s reset timer unfed\n",
		       strerror(errno));
		return;
	}
	rc = ioctl(wdt_fd, WDIOC_SETTIMEOUT, &t);
	printf("[evergo] wdt: SETTIMEOUT(31) rc=%d errno=%d\n",
	       rc, rc < 0 ? errno : 0);
	wdt_last_rc = ioctl(wdt_fd, WDIOC_KEEPALIVE, 0);
	printf("[evergo] wdt: KEEPALIVE rc=%d errno=%d\n",
	       wdt_last_rc, wdt_last_rc < 0 ? errno : 0);
	printf("[evergo] wdt: feeding the watchdog every ~10 s\n");
	fflush(stdout);
}

static void wdt_ping(void)
{
	if (wdt_fd >= 0 && wdt_feeding) {
		int rc = ioctl(wdt_fd, WDIOC_KEEPALIVE, 0);

		if (rc != wdt_last_rc) {
			printf("[evergo] wdt: KEEPALIVE rc %d -> %d errno=%d\n",
			       wdt_last_rc, rc, rc < 0 ? errno : 0);
			fflush(stdout);
			wdt_last_rc = rc;
		}
	}
}

/*
 * The black box.  /dev/pmsg0 is write-only (the fops have no read), so the
 * persistent records are read back through the mounted pstore filesystem
 * as /sys/fs/pstore/pmsg-ramoops-0.  The 64 KiB pmsg zone in DRAM survives
 * a reset, which turns "the board rebooted for no reason" into "the last
 * heartbeat was at t=X".
 */
static int pmsg_fd = -1;		/* /dev/pmsg0, write side */
static int kmsg_wr_fd = -2;		/* /dev/kmsg, -2 = not tried yet */
static unsigned int hb_n;

static void pmsg_put(const char *s)
{
	if (pmsg_fd >= 0) {
		ssize_t r = write(pmsg_fd, s, strlen(s));

		(void)r;
	}
}

static void pmsg_open(void)
{
	pmsg_fd = open("/dev/pmsg0", O_WRONLY | O_NONBLOCK);
	if (pmsg_fd < 0)
		printf("[evergo] pmsg: cannot open /dev/pmsg0 (%s) - black box off\n",
		       strerror(errno));
	else
		printf("[evergo] pmsg: black box armed (/dev/pmsg0)\n");
	fflush(stdout);
}

static void pmsg_replay(void)
{
	static char buf[65536 + 1];
	char path[160];
	struct dirent *de;
	DIR *d;
	int fd, n;

	d = opendir("/sys/fs/pstore");
	if (!d) {
		printf("[evergo] pmsg: no /sys/fs/pstore (%s)\n",
		       strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "pmsg-ramoops", 12))
			continue;
		snprintf(path, sizeof(path), "/sys/fs/pstore/%s",
			 de->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0) {
			printf("[evergo] pmsg: %s is empty\n", de->d_name);
			continue;
		}
		buf[n] = 0;
		printf("\r\n=== evergo: pmsg history tail ===\r\n");
		if (n > 800) {
			printf("...(older bytes dropped)...\r\n%s",
			       buf + n - 800);
		} else {
			printf("%s", buf);
		}
		printf("\r\n=== evergo: end of pmsg history ===\r\n");
	}
	closedir(d);
}

static unsigned int mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* One black-box line every ~5 s: uptime, last KEEPALIVE result, feeding
 * state and the live TOPRGU mode.  Every 6th line also goes into the ring
 * so captures show the heartbeat alongside the kernel log. */
static void hb_emit(void)
{
	unsigned int ms = mono_ms();
	char l[160];

	snprintf(l, sizeof(l), "hb t=%u.%03u k=%d f=%d m=%08x\n",
		 ms / 1000, ms % 1000, wdt_last_rc,
		 wdt_fd >= 0 && wdt_feeding,
		 toprgu ? (unsigned)toprgu[0] : 0xffffffffu);
	pmsg_put(l);
	hb_n++;
	if (hb_n % 6)
		return;
	if (kmsg_wr_fd == -2)
		kmsg_wr_fd = open("/dev/kmsg", O_WRONLY);
	if (kmsg_wr_fd >= 0) {
		int n = snprintf(l, sizeof(l), "evergo-hb t=%u.%03u k=%d\n",
				 ms / 1000, ms % 1000, wdt_last_rc);
		ssize_t r = write(kmsg_wr_fd, l, n);

		(void)r;
	}
}

/*
 * 's': stop feeding and disarm the watchdog through the driver
 * (nowayout=0, so a disable is allowed).  If the board still resets
 * afterwards, the TOPRGU watchdog was not the executioner.
 */
static void wdt_disarm(void)
{
	int opt = WDIOS_DISABLECARD, rc;

	if (wdt_fd < 0) {
		printf("[evergo] wdt: not open, nothing to disarm\n");
		return;
	}
	wdt_feeding = 0;
	rc = ioctl(wdt_fd, WDIOC_SETOPTIONS, &opt);
	printf("[evergo] wdt: DISABLECARD rc=%d errno=%d toprgu mode=%08x\n",
	       rc, rc < 0 ? errno : 0,
	       toprgu ? (unsigned)toprgu[0] : 0xffffffffu);
	pmsg_put("ev: wdt disarmed (s)\n");
	fflush(stdout);
}

static void wdt_rearm(void)
{
	int opt = WDIOS_ENABLECARD, t = 31, rc;

	if (wdt_fd < 0) {
		wdt_setup();
		return;
	}
	rc = ioctl(wdt_fd, WDIOC_SETOPTIONS, &opt);
	printf("[evergo] wdt: ENABLECARD rc=%d errno=%d\n",
	       rc, rc < 0 ? errno : 0);
	ioctl(wdt_fd, WDIOC_SETTIMEOUT, &t);
	wdt_feeding = 1;
	rc = ioctl(wdt_fd, WDIOC_KEEPALIVE, 0);
	wdt_last_rc = rc;
	printf("[evergo] wdt: rearmed, KEEPALIVE rc=%d toprgu mode=%08x\n",
	       rc, toprgu ? (unsigned)toprgu[0] : 0xffffffffu);
	pmsg_put("ev: wdt rearmed (S)\n");
	fflush(stdout);
}

/*
 * Reset through the watchdog instead of a clean reboot.  LK only copies the
 * DRAM rings into expdb when the previous reset looked abnormal; a clean
 * RB_AUTOBOOT left every recent readback empty.  Opening /dev/watchdog takes
 * the feed away from the kernel; not petting it gives a watchdog reset,
 * which is exactly what LK dumps for.  With the COM log working this only
 * runs on demand: 'r' on the console port.
 */
static void evergo_force_reset(const char *why)
{
	int fd;

	printf("[evergo] forcing watchdog reset (%s)\n", why);
	fflush(stdout);
	sync();
	fd = open("/dev/watchdog", O_WRONLY);
	if (fd >= 0) {
		int t = 5;

		ioctl(fd, WDIOC_SETTIMEOUT, &t);
		sleep(60);		/* no feeding: the SoC resets in ~5 s */
		close(fd);
	}
	reboot(RB_AUTOBOOT);
}

/*
 * The log of the life before this one, if the board died after the kernel had
 * armed pstore/ramoops: every /sys/fs/pstore file (dmesg-ramoops-N from the
 * console zone, pmsg-ramoops-N from pmsg) is read once and mirrored to the
 * COM port.  On an abrupt reset the old records are still in DRAM when the
 * next kernel probes ramoops, so a crash is inspected by simply attaching
 * the terminal -- no read-back, no fastboot.
 */
static void replay_pstore(int gs)
{
	static const char banner[] =
		"\r\n=== evergo: log from the previous boot (pstore) ===\r\n";
	char path[256], buf[4096];
	struct dirent *de;
	DIR *d;
	int nf = 0, total = 0;

	if (gs < 0)
		return;
	d = opendir("/sys/fs/pstore");
	if (!d) {
		printf("[evergo] pstore: no /sys/fs/pstore (%s)\n",
		       strerror(errno));
		return;
	}
	while ((de = readdir(d))) {
		int fd, n, fsz = 0;

		if (de->d_name[0] == '.')
			continue;
		if (strstr(de->d_name, ".enc.z")) {
			/* the kernel could not decompress it: raw zlib bytes,
			 * useless on a terminal, skip it */
			printf("[evergo] pstore: skipping %s (still compressed)\n",
			       de->d_name);
			continue;
		}
		snprintf(path, sizeof(path), "/sys/fs/pstore/%s", de->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (!nf)
			gs_write_all(gs, banner, sizeof(banner) - 1, 2000);
		nf++;
		n = snprintf(buf, sizeof(buf), "--- %s ---\r\n", de->d_name);
		gs_write_all(gs, buf, n, 2000);
		while ((n = read(fd, buf, sizeof(buf))) > 0 &&
		       fsz < (128 << 10) && total < (512 << 10)) {
			gs_write_all(gs, buf, n, 2000);
			fsz += n;
			total += n;
		}
		if (fsz >= (128 << 10))
			gs_write_all(gs, "... (truncated)\r\n", 17, 1000);
		close(fd);
	}
	closedir(d);
	if (nf) {
		int n = snprintf(buf, sizeof(buf),
				 "=== evergo: end of previous-boot log: %d file(s), %d bytes ===\r\n",
				 nf, total);

		gs_write_all(gs, buf, n, 2000);
	}
	printf("[evergo] pstore: replayed %d file(s), %d bytes\n", nf, total);
}

/* Send the whole /dev/kmsg buffer again.  A fresh open starts at the oldest
 * record still in the ring, which is what a terminal that has just been
 * opened wants to see: without this the boot log was streamed once at boot
 * time and a serial console attached later just sat there empty. */
static void dump_backlog(int gs)
{
	static const char banner[] =
		"\r\n=== evergo: kernel log follows ===\r\n";
	char buf[4096], out[8192], foot[160];
	int fd, n, total = 0, sent = 0, len, err = 0;

	if (gs < 0)
		return;
	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		printf("[evergo] backlog: open /dev/kmsg: %s\n", strerror(errno));
		return;
	}
	gs_write_all(gs, banner, sizeof(banner) - 1, 1000);
	while ((n = read(fd, buf, sizeof(buf))) > 0 && total < (768 << 10)) {
		total += n;
		len = fmt_record(out, sizeof(out), buf, n);
		sent += gs_write_all(gs, len ? out : buf, len ? len : n, 2000);
		if (sent < total) {
			err = errno;		/* do not eat the rest silently */
			break;
		}
	}
	close(fd);
	len = snprintf(foot, sizeof(foot),
		       "=== evergo: end of log: %d of %d bytes%s ===\r\n",
		       sent, total, err ? " (write failed)" : "");
	gs_write_all(gs, foot, len, 1000);
	if (sent > 0 && host_present)
		host_ok = 1;
	dump_ok = (!err && total > 0);
	/* also visible on the next expdb readback, even if USB delivered none */
	printf("[evergo] backlog: read=%d sent=%d lost=%u zeros=%u errno=%d ok=%d\n",
	       total, sent, tx_lost, zero_writes, err, dump_ok);
}

/*
 * Is a USB host attached and enumerated?  TIOCMGET cannot answer this:
 * u_serial.c implements no ioctl at all, so reading DTR from user space is
 * impossible -- and gating "a host is listening" on it left host_ok at 0
 * forever, which made the board reset itself under a perfectly good terminal
 * every two minutes.  The UDC state is reliable and right there.
 */
static int gs_host(void)
{
	struct dirent *de;
	DIR *d = opendir("/sys/class/udc");
	char path[128], buf[64];
	int fd, n, up = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state",
			 de->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = 0;
			if (strstr(buf, "configured"))
				up = 1;
		}
	}
	closedir(d);
	return up;
}

/*
 * The serial console, as a pump rather than a loop.
 *
 * It has to be callable from inside do_dump() as well: the dump is slow (the
 * LM36273 register writes each burn an i2c timeout, ~8 s in total) and a host
 * that opens the port and sends Enter during that window used to get nothing
 * but a stalled write ("semaphore timeout") because nobody was reading.
 */
static int gs_fd = -1;			/* /dev/ttyGS0, opened early */
static int kmsg_fd = -1;		/* live log stream */
static unsigned int pump_ticks;
static unsigned int last_rx_tick;	/* when the host last sent a byte */


/*
 * Paint the kernel log on the panel.  With console=tty0 removed the kernel
 * no longer renders every printk line to fbcon (that alone cost 86 seconds of
 * boot), but the framebuffer console is still there -- writing to /dev/tty0
 * makes fbcon draw it.  Slow, so it is on demand only.
 */
static void paint_panel(void)
{
	char buf[8192];
	int tty = open("/dev/tty0", O_WRONLY);
	int fd, n, total = 0;

	if (tty < 0) {
		printf("panel: cannot open /dev/tty0: %s\n", strerror(errno));
		return;
	}
	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		printf("panel: cannot open /dev/kmsg: %s\n", strerror(errno));
		close(tty);
		return;
	}
	write(tty, "\r\n=== evergo: log on the panel (may take a minute) ===\r\n", 55);
	while ((n = read(fd, buf, sizeof(buf))) > 0 && total < 512 * 1024) {
		write(tty, buf, n);
		total += n;
	}
	write(tty, "\r\n=== evergo: end of log ===\r\n", 30);
	close(fd);
	close(tty);
	printf("panel: painted %d bytes of log on /dev/tty0\n", total);
}

static void console_pump(void)
{
	char buf[4096], out[8192];
	int n, i;

	if (gs_fd < 0)
		return;

	if (pump_ticks % 25 == 0 || !host_present)
		host_present = gs_host();
	if (host_present && !had_host) {
		printf("[evergo] usb host configured\n");
		dump_ok = 0;
		tx_jam = 0;
		dump_backlog(gs_fd);	/* somebody just enumerated us */
		if (!pstore_done) {	/* and how the previous life ended */
			pstore_done = 1;
			replay_pstore(gs_fd);
		}
	}
	had_host = host_present;

	if (kmsg_fd >= 0)
		while ((n = read(kmsg_fd, buf, sizeof(buf))) > 0) {
			int len;

			rx_bytes += n;
			/*
			 * No host: do not push anything into ttyGS0.  A full FIFO
			 * with nobody draining it turns every record into a full
			 * write-budget stall, and a multi-thousand-record boot
			 * backlog then keeps this loop busy for half an hour --
			 * that is exactly how b187's init ended up frozen before
			 * it ever reached its own banner.  dump_backlog() replays
			 * the whole ring from the start anyway once a host shows
			 * up, so nothing is lost by skipping here.
			 *
			 * The same trap exists with a host that is configured
			 * but not draining (terminal not open yet): the first
			 * zero-byte write trips tx_jam and the rest of this run
			 * just drains.  It is retried every ~10 s and on every
			 * host edge.  b188 skipped only the !host_present case
			 * and init still froze in exactly this window.
			 */
			if (!host_present || tx_jam)
				continue;
			len = fmt_record(out, sizeof(out), buf, n);
			if (gs_write_all(gs_fd, len ? out : buf,
					 len ? len : n, 1000) > 0)
				host_ok = 1;
			else
				tx_jam = 1;
		}

	n = read(gs_fd, buf, sizeof(buf));
	if (n > 0) {
		host_ok = 1;		/* input works: a host is really there */
		last_rx_tick = pump_ticks;
	}
	for (i = 0; i < n; i++) {
		switch (buf[i]) {
		case 'r':
		case 'R':
			gs_write_all(gs_fd, "\r\nevergo: remote reboot\r\n", 25,
				     300);
			if (wdt_fd >= 0) {
				wdt_feeding = 0;	/* the WDT resets within 31 s */
				pmsg_put("ev: r pressed, feeding stopped\n");
			} else {
				evergo_force_reset("remote r");
			}
			break;
		case 'd':
		case 'D':
			clk_report();
			break;
		case 'p':
		case 'P':
			pinctrl_report();
			break;
		case 'i':
		case 'I':
			i2c_scan();
			break;
		case 'v':
		case 'V':
			paint_panel();
			break;
		case 'x':
		case 'X':
			do_dump();	/* full diagnostics, on demand */
			break;
		case 'g':
		case 'G':
			gic_deliver_test();
			break;
		case 's':
			wdt_disarm();
			break;
		case 'S':
			wdt_rearm();
			break;
		case 'b':
			backlight_set(0);
			break;
		case 'B':
			backlight_set(1);
			break;
		case 'm':
		case 'M':
			pmsg_replay();
			break;
		case 'l':
		case 'L':
		case '\r':
		case '\n':
			dump_backlog(gs_fd);
			break;
		default:
			break;
		}
	}

	pump_ticks++;
	if (pump_ticks % 25 == 0)	/* ~5 s: persistent heartbeat */
		hb_emit();
	if (pump_ticks % 50 == 0)	/* ~10 s: keep the 31 s WDT away */
		wdt_ping();
	if (pump_ticks == 25 || pump_ticks % 150 == 0) {	/* ~5 s, then ~30 s */
		unsigned int ms = mono_ms();

		printf("[evergo] pump: t=%u.%03u gs=%d host=%d host_ok=%d "
		       "dump_ok=%d jam=%d rx=%u tx=%u lost=%u zeros=%u "
		       "wdt=%d krc=%d\n",
		       ms / 1000, ms % 1000,
		       gs_fd, host_present, host_ok, dump_ok, tx_jam,
		       rx_bytes, tx_bytes, tx_lost, zero_writes,
		       wdt_fd >= 0 && wdt_feeding, wdt_last_rc);
		/*
		 * Only replay the ring when somebody is actually on the other
		 * end: a blind 40 KiB burst into an undrained FIFO is how a
		 * dead port used to look busy for half an hour.
		 */
		if (host_present && !tx_jam &&
		    (dump_ok == 0 || (int)(pump_ticks - last_rx_tick) < 225))
			dump_backlog(gs_fd);
	}

	/* a jammed FIFO gets another chance every ~10 s: the terminal may
	 * only have opened after the gadget was configured */
	if (tx_jam && pump_ticks % 50 == 0)
		tx_jam = 0;

	/*
	 * A partial replay is worth another try: the FIFO may simply have been
	 * full at the wrong moment.  Only while a host is really there, and not
	 * more often than every ~10 s.
	 */
	if (host_present && !dump_ok && !tx_jam && pump_ticks % 50 == 0)
		dump_backlog(gs_fd);

	/*
	 * The old ~120 s expdb refresh cycle lived here: while the console was
	 * blind, a timer reboot was the only way to keep the LK ring dump (and
	 * with it the read-back channel) fresh.  The COM log works now, so the
	 * board is meant to run indefinitely: no timer-driven reboots any
	 * more.  The rings can still be refreshed on demand -- 'r' on the
	 * console performs the same watchdog reset, and a long-press power
	 * cycle dumps them just the same.
	 */

	/* keep status lines and the heartbeat moving: a stdio buffer stuck
	 * behind one rejected large write hid them all on b190 */
	fflush(stdout);
}

/* devtmpfs is usually fast enough, but if the node lags behind the tty, make
 * it by hand from the sysfs major:minor -- otherwise the port exists on the
 * host while init cannot open it and the console stays dead. */
static int ttygs0_mknod(void)
{
	FILE *f = fopen("/sys/class/tty/ttyGS0/dev", "r");
	int maj, min;

	if (!f)
		return -1;
	if (fscanf(f, "%d:%d", &maj, &min) != 2) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return mknod("/dev/ttyGS0", S_IFCHR | 0666, makedev(maj, min));
}

/* Open the port before the dump so the pumps above have something to talk to.
 * The gadget is bound at the end of main, so the first call (before do_dump)
 * is only a cheap breadcrumb and will always report "no ttyGS0". */
static void console_start(int ms)
{
	int waited = 0, tried_mknod = 0;

	printf("[evergo] console: waiting for /dev/ttyGS0 (up to %d ms)\n", ms);
	fflush(stdout);
	while (gs_fd < 0 && waited < ms) {
		gs_fd = open("/dev/ttyGS0", O_RDWR | O_NONBLOCK);
		if (gs_fd < 0) {
			if (!tried_mknod && waited >= 500) {
				tried_mknod = 1;
				if (!ttygs0_mknod())
					printf("[evergo] console: made /dev/ttyGS0 "
					       "by hand (devtmpfs lagged)\n");
			}
			usleep(100000);
			waited += 100;
		}
	}
	if (gs_fd < 0) {
		printf("[evergo] console: no /dev/ttyGS0 (%s), live console "
		       "unavailable\n", strerror(errno));
		return;
	}
	if (kmsg_fd < 0)
		kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	printf("[evergo] console: live on /dev/ttyGS0 (d i p l v x g b / Enter, r=reboot)\n");
	fflush(stdout);
	console_pump();
}


static void do_dump(void)
{
	printf("\n=== EVERGO-DUMP-BEGIN ===\n\n");
	printf("uid=%d gid=%d\n", getuid(), getgid());
	fflush(stdout);
	hw_diag_irq();		/* live GIC/DSI/intpol state, same as the boot dump */
	dump_file("/proc/version", "kernel version", 0);
	dump_file("/proc/cmdline", "cmdline", 0);
	dump_file("/proc/device-tree/model", "DT model", 0);
	dump_file("/proc/device-tree/compatible", "DT compatible", 0);
	dump_file("/proc/device-tree/chosen/bootargs", "DT bootargs", 0);
	dump_file("/proc/device-tree/memory@40000000/reg", "DT memory reg", 1);
	dump_file("/proc/device-tree/chosen/linux,initrd-start", "initrd start", 1);
	dump_dir("/proc/device-tree");
	dump_dir("/proc/device-tree/reserved-memory");
	console_pump();
	i2c_scan();
	clk_report();
	pinctrl_report();
	console_pump();
	fflush(stdout);
	/*
	 * Everything above has to be out before this: the LM36273 writes below
	 * each take an i2c timeout (~2 s x 4 registers) while the chip is still
	 * unreachable, and with full buffering the whole dump would sit in the
	 * 64 KiB buffer until after that stall.
	 */
	console_pump();
	poke_backlights();
	console_pump();
	fflush(stdout);
	printf("=== EVERGO-DUMP-END ===\n");
}

int main(void)
{
	static char stdout_buf[64 * 1024];
	int kmsg;

	/* one big write() per flush keeps /dev/kmsg happy */
	setvbuf(stdout, stdout_buf, _IOFBF, sizeof(stdout_buf));

	if (mount("proc", "/proc", "proc", 0, NULL))
		printf("mount /proc: %s\n", strerror(errno));
	if (mount("sysfs", "/sys", "sysfs", 0, NULL))
		printf("mount /sys: %s\n", strerror(errno));
	if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL))
		printf("mount /dev: %s\n", strerror(errno));
	mkdir("/sys/kernel/debug", 0755);

	/*
	 * Everything from here on goes through printk.  console=tty0 is in the
	 * bootargs (it is what makes fbcon pick the log up once the panel works),
	 * so /dev/console is the VT -- and with no fbcon there is no driver
	 * behind it: on b163 this whole dump vanished into a dummy console.
	 * printk is the one channel that always reaches the ring (and from there
	 * plog and expdb).  Do it before the first diagnostics: on /dev/console
	 * they land on ttyS0 and are invisible in a COM capture (b196's banner
	 * and wdt lines never showed up for exactly that reason).
	 */
	kmsg = open("/dev/kmsg", O_WRONLY);
	if (kmsg >= 0) {
		dup2(kmsg, 1);
		dup2(kmsg, 2);
		close(kmsg);
	} else {
		printf("cannot open /dev/kmsg: %s\n", strerror(errno));
	}
	fflush(stdout);

	printf("\n\n=== evergo bring-up init [b267: wdt-inherit] ===\n");

	/*
	 * pstore carries what must survive a reset: the ramoops console zone
	 * (the dying boot's last words) and the pmsg ring (the black-box
	 * heartbeats below).  Mount it before anything can go wrong.
	 */
	mkdir("/sys/fs/pstore", 0755);
	if (mount("pstore", "/sys/fs/pstore", "pstore", 0, NULL) &&
	    errno != EBUSY)
		printf("mount pstore: %s\n", strerror(errno));

	/*
	 * Nobody feeds the 31 s TOPRGU watchdog that setup.c arms, so the
	 * board resets itself on a timer; take it over (see wdt_setup).
	 */
	wdt_setup();
	if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) &&
	    errno != EBUSY)
		printf("mount debugfs: %s\n", strerror(errno));

        /*
         * b227: the i2c6 death watch forks right here (~0.65 s in): the
         * block dies somewhere between 1.4 s and 3.2 s, so the sampler
         * has to be pinging well before that.
         */
        fflush(stdout);
        if (fork() == 0)
                i2c_deathwatch();       /* does not return */

	/*
	 * The black box: /dev/pmsg0 is the write side, the history comes back
	 * through /sys/fs/pstore/pmsg-ramoops-0.  A marker per boot plus a
	 * heartbeat every ~5 s turns any silent death into "last seen at
	 * t=X", read back on the next boot, across resets.
	 */
	pmsg_open();
	pmsg_put("- boot v13 -\n");
	pmsg_replay();
	fflush(stdout);

	/*
	 * Get the console up before anything else: the built-in g_serial has the
	 * UDC bound from boot, so /dev/ttyGS0 is normally already there and
	 * everything printed from now on is streamed live.
	 */
	console_start(2000);
	usb_bringup();

	/*
	 * No do_dump() here any more: it blocks for a minute in i2c timeouts
	 * (16 pings x 2 s plus the backlight writes), and while it runs the
	 * device does not read the port at all -- Windows reports that as
	 * "semaphore timeout" on every write and the pump cannot serve anyone.
	 * The full dump is one key away ('x').
	 */
	printf("\n=== evergo: init reached userspace ===\n");
	{
		DIR *pd = opendir("/sys/fs/pstore");
		int cnt = 0;

		if (pd) {
			struct dirent *pe;

			while ((pe = readdir(pd)))
				if (pe->d_name[0] != '.')
					cnt++;
			closedir(pd);
			printf("[evergo] pstore: %d record(s) ready for replay\n",
			       cnt);
		} else {
			printf("[evergo] pstore: /sys/fs/pstore missing (%s)\n",
			       strerror(errno));
		}
	}
	fflush(stdout);

	/*
	 * The DSI is the current blocker: it waits 500 ms for a VM_DONE
	 * interrupt that never wakes anyone (irq_data stays 0) while the
	 * status register shows the flag set.  /proc/interrupts settles
	 * which side is broken: a zero count on the DSI line means the
	 * delivery side (INTEN/GIC), a non-zero count the handler side.
	 */
	dump_file("/proc/interrupts", "interrupts (DSI line)", 0);
	/*
	 * b265: which initramfs is actually running?  If the FDT's chosen
	 * node has no linux,initrd-start, the LK-provided boot.img ramdisk
	 * section was never attached and only the initramfs built into the
	 * kernel image is unpacked -- the reason init-only changes (b263/
	 * b264) never took effect until the kernel itself was rebuilt.
	 */
	dump_file("/proc/device-tree/chosen/linux,initrd-start",
		  "chosen: linux,initrd-start", 1);
	dump_file("/proc/device-tree/chosen/linux,initrd-end",
		  "chosen: linux,initrd-end", 1);
	hw_diag_irq();
	probe_block_devices();
	fflush(stdout);

	/*
	 * The kernel's built-in g_serial has bound the UDC since boot; the
	 * port only needs opening, and this is where we live from now on.
	 * A truly absent port (gadget never bound) used to trigger one last
	 * watchdog reset; now the board simply idles and keeps retrying --
	 * no reset loop, which also keeps LK's A/B boot-failure counter from
	 * tripping, and a long press still dumps the rings into expdb.
	 */
	console_start(15000);
	if (gs_fd < 0) {
		printf("=== no COM port: idling, retrying every ~20 s ===\n");
		fflush(stdout);
		for (;;) {
			sleep(20);
			wdt_ping();
			hb_emit();
			console_start(15000);
			if (gs_fd >= 0)
				break;
		}
	}
	for (;;) {
		console_pump();
		usleep(200000);
	}

	return 0;
}
