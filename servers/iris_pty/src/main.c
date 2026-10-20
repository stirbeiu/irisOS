/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_pty/src/main.c
 * Purpose: Standalone Ring 3 User-Space Pseudo-Terminal (iris_pty) Daemon.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include <zircon/types.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iris/ipc.h>
#include <iris/init.h>
#include <iris/pci.h>
#include <iris/log.h>
#include <servers/drivers/pci/pci.h>
#include <servers/drivers/virtio_input/include/virtio_input.h>
#include <servers/iris_pty/include/pty.h>
#include <iris/fd_table.h>

#ifndef ZX_SEC
#define ZX_SEC(n) ((zx_duration_t)(n) * 1000000000ULL)
#endif
#ifndef ZX_MSEC
#define ZX_MSEC(n) ((zx_duration_t)(n) * 1000000ULL)
#endif

/* Global Server State */
static zx_handle_t        g_root_resource   = ZX_HANDLE_INVALID;
static zx_handle_t        g_debuglog_handle = ZX_HANDLE_INVALID;
static zx_handle_t        g_server_channel  = ZX_HANDLE_INVALID;

static virtio_input_dev_t g_input_dev;
static bool               g_has_virtio_input = false;

static pty_discipline_t   g_pty;
static volatile bool      g_shutdown = false;

/* Client channels connected to PTY */
static zx_handle_t        g_client_channels[PTY_MAX_CLIENTS];
static uint32_t           g_num_clients = 0;

static void pty_lock(pty_discipline_t* p) {
    while (__atomic_test_and_set(&p->lock, __ATOMIC_ACQUIRE)) {
        /* Spinlock wait */
    }
}

static void pty_unlock(pty_discipline_t* p) {
    __atomic_clear(&p->lock, __ATOMIC_RELEASE);
}

void pty_raw_output(const char* buf, size_t len) {
    if (!buf || len == 0) return;
    if (g_debuglog_handle != ZX_HANDLE_INVALID) {
        zx_debuglog_write(g_debuglog_handle, 0, buf, len);
    } else {
        write(1, buf, len);
    }
}

static void ready_push(pty_discipline_t* p, char c) {
    if (p->ready_count < PTY_READY_BUF_MAX) {
        p->ready_buf[p->ready_tail] = c;
        p->ready_tail = (p->ready_tail + 1) % PTY_READY_BUF_MAX;
        p->ready_count++;
    }
}

static char ready_pop(pty_discipline_t* p) {
    if (p->ready_count == 0) return 0;
    char c = p->ready_buf[p->ready_head];
    p->ready_head = (p->ready_head + 1) % PTY_READY_BUF_MAX;
    p->ready_count--;
    return c;
}

void pty_feed_char(pty_discipline_t* p, char c) {
    pty_lock(p);

    if (p->mode_flags & IRIS_PTY_MODE_CANONICAL) {
        /* Canonical (Cooked) Mode */
        if (c == '\r') {
            c = '\n';
        }

        /* 1. Handle Ctrl+C (0x03) -> SIGINT generation & editing line discard */
        if (c == 0x03) {
            if (p->mode_flags & IRIS_PTY_MODE_ECHO) {
                pty_raw_output("^C\n", 3);
            }
            p->line_len = 0;
            p->pending_signal = 2; /* POSIX SIGINT */
            pty_unlock(p);
            return;
        }

        /* 2. Handle Ctrl+D (0x04) -> EOF or partial flush */
        if (c == 0x04) {
            if (p->line_len == 0) {
                p->eof_pending = true;
            } else {
                /* Flush current line without newline */
                for (size_t i = 0; i < p->line_len; i++) {
                    ready_push(p, p->line_buf[i]);
                }
                p->line_len = 0;
            }
            pty_unlock(p);
            return;
        }

        /* 3. Handle Backspace ('\b' or 0x7F DEL) */
        if (c == '\b' || c == 0x7F) {
            if (p->line_len > 0) {
                p->line_len--;
                if (p->mode_flags & IRIS_PTY_MODE_ECHO) {
                    pty_raw_output("\b \b", 3);
                }
            }
            pty_unlock(p);
            return;
        }

        /* 4. Handle Line Completion ('\n') */
        if (c == '\n') {
            if (p->line_len < PTY_LINE_BUF_MAX - 1) {
                p->line_buf[p->line_len++] = '\n';
            }
            for (size_t i = 0; i < p->line_len; i++) {
                ready_push(p, p->line_buf[i]);
            }
            p->line_len = 0;
            if (p->mode_flags & IRIS_PTY_MODE_ECHO) {
                pty_raw_output("\r\n", 2);
            }
            pty_unlock(p);
            return;
        }

        /* 5. Regular Printable Characters */
        if (p->line_len < PTY_LINE_BUF_MAX - 1) {
            p->line_buf[p->line_len++] = c;
            if (p->mode_flags & IRIS_PTY_MODE_ECHO) {
                pty_raw_output(&c, 1);
            }
        }
    } else {
        /* Raw Mode: Immediate Character Pass-Through */
        ready_push(p, c);
        if (p->mode_flags & IRIS_PTY_MODE_ECHO) {
            pty_raw_output(&c, 1);
        }
    }

    pty_unlock(p);
}

/**
 * @brief Background UART Serial Console RX worker.
 */
static void uart_rx_thread_entry(uintptr_t arg) {
    zx_handle_t rsrc = (zx_handle_t)arg;
    while (!g_shutdown) {
        char c = 0;
        size_t len = 1;
        zx_status_t st = zx_debug_read(rsrc, &c, &len);
        if (st == ZX_OK && len == 1) {
            pty_feed_char(&g_pty, c);
        } else {
            zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
        }
    }
    zx_thread_exit();
}

/**
 * @brief Initialize UART serial console fallback input thread.
 */
static void init_uart_fallback(void) {
    if (g_root_resource == ZX_HANDLE_INVALID) return;

    zx_handle_t proc = zx_process_self();
    if (proc == ZX_HANDLE_INVALID) {
        proc = iris_process_self();
    }
    if (proc == ZX_HANDLE_INVALID) {
        proc = zx_take_startup_handle(PA_HND(PA_PROC_SELF, 0));
    }
    if (proc == ZX_HANDLE_INVALID) {
        log_status(STATUS_FAIL, "iris_pty", "Missing process handle for UART worker.");
        return;
    }

    zx_handle_t thrd = ZX_HANDLE_INVALID;
    zx_status_t st = zx_thread_create(proc, "uart_rx", 7, 0, &thrd);
    if (st != ZX_OK) {
        log_status(STATUS_FAIL, "iris_pty", "zx_thread_create for UART worker failed (%d).", st);
        return;
    }

    /* Allocate and map 64 KB thread stack */
    zx_handle_t stack_vmo = ZX_HANDLE_INVALID;
    zx_vmo_create(65536, 0, &stack_vmo);
    zx_vaddr_t stack_base = 0;
    zx_handle_t vmar = zx_vmar_root_self();
    if (vmar == ZX_HANDLE_INVALID) {
        vmar = iris_vmar_root_self();
    }
    if (vmar == ZX_HANDLE_INVALID) {
        vmar = zx_take_startup_handle(PA_HND(PA_VMAR_ROOT, 0));
    }
    zx_vmar_map(vmar, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, stack_vmo, 0, 65536, &stack_base);
    zx_handle_close(stack_vmo);

    uintptr_t sp = (stack_base + 65536) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
    sp -= 8;
#endif

    st = zx_thread_start(thrd, (uintptr_t)uart_rx_thread_entry, sp, (uintptr_t)g_root_resource, 0);
    zx_handle_close(thrd);

    if (st == ZX_OK) {
        log_status(STATUS_OKAY, "iris_pty", "UART serial console worker initialized.");
    }
}

/**
 * @brief Process an incoming IPC message on a PTY channel.
 */
static bool handle_pty_message(zx_handle_t ch) {
    uint8_t msg_buf[IRIS_IPC_MAX_MSG_BYTES] __attribute__((aligned(8)));
    zx_handle_t in_handles[4];
    uint32_t act_bytes = 0, act_handles = 0;

    zx_status_t st = zx_channel_read(ch, 0, msg_buf, in_handles,
                                     sizeof(msg_buf), 4, &act_bytes, &act_handles);
    if (st != ZX_OK) {
        return false;
    }

    if (act_bytes < sizeof(iris_msg_hdr_t)) {
        return true;
    }

    const iris_msg_hdr_t* req = (const iris_msg_hdr_t*)msg_buf;
    const uint8_t* payload = msg_buf + sizeof(iris_msg_hdr_t);
    uint32_t payload_len = req->payload_len;

    /* 1. Handle Core System Protocol (Ping / Shutdown) */
    if (req->protocol == IRIS_PROTO_SYSTEM) {
        if (req->opcode == IRIS_SYS_OP_PING) {
            iris_msg_hdr_t rep;
            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
            rep.opcode = IRIS_SYS_OP_PONG;
            zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            return true;
        } else if (req->opcode == IRIS_SYS_OP_SHUTDOWN) {
            iris_msg_hdr_t rep;
            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
            zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            g_shutdown = true;
            return true;
        }
    }

    /* 2. Handle PTY Subsystem Protocol */
    if (req->protocol != IRIS_PROTO_PTY) {
        iris_msg_hdr_t rep;
        iris_msg_hdr_init_reply(&rep, req, ZX_ERR_NOT_SUPPORTED, 0);
        zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
        return true;
    }

    switch (req->opcode) {
        case IRIS_PTY_OP_READ: {
            uint32_t req_count = 0;
            if (payload_len >= sizeof(iris_pty_read_req_t)) {
                req_count = ((const iris_pty_read_req_t*)payload)->count;
            }
            if (req_count == 0 || req_count > 4096) {
                req_count = 4096;
            }

            uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_pty_read_resp_t) + 4096] __attribute__((aligned(8)));
            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            iris_pty_read_resp_t* resp = (iris_pty_read_resp_t*)(rep_buf + sizeof(iris_msg_hdr_t));
            char* out_data = (char*)(rep_buf + sizeof(iris_msg_hdr_t) + sizeof(iris_pty_read_resp_t));

            pty_lock(&g_pty);
            uint32_t copied = 0;
            if (g_pty.ready_count > 0) {
                while (g_pty.ready_count > 0 && copied < req_count) {
                    out_data[copied++] = ready_pop(&g_pty);
                }
                resp->bytes_read = copied;
            } else if (g_pty.eof_pending) {
                g_pty.eof_pending = false;
                resp->bytes_read = 0; /* EOF */
            } else {
                resp->bytes_read = 0;
            }
            pty_unlock(&g_pty);

            uint32_t rep_payload = (uint32_t)(sizeof(iris_pty_read_resp_t) + resp->bytes_read);
            iris_msg_hdr_init_reply(rep_hdr, req, ZX_OK, rep_payload);
            zx_channel_write(ch, 0, rep_buf, sizeof(iris_msg_hdr_t) + rep_payload, NULL, 0);
            break;
        }

        case IRIS_PTY_OP_WRITE: {
            uint32_t count = 0;
            if (payload_len >= sizeof(iris_pty_write_req_t)) {
                count = ((const iris_pty_write_req_t*)payload)->count;
            }
            if (count > 0 && payload_len >= sizeof(iris_pty_write_req_t) + count) {
                const char* data = (const char*)(payload + sizeof(iris_pty_write_req_t));
                pty_raw_output(data, count);
            }

            uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_pty_write_resp_t)] __attribute__((aligned(8)));
            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            iris_pty_write_resp_t* resp = (iris_pty_write_resp_t*)(rep_buf + sizeof(iris_msg_hdr_t));
            resp->bytes_written = count;

            iris_msg_hdr_init_reply(rep_hdr, req, ZX_OK, sizeof(iris_pty_write_resp_t));
            zx_channel_write(ch, 0, rep_buf, sizeof(rep_buf), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_SET_MODE: {
            if (payload_len >= sizeof(iris_pty_mode_t)) {
                pty_lock(&g_pty);
                g_pty.mode_flags = ((const iris_pty_mode_t*)payload)->flags;
                pty_unlock(&g_pty);
            }
            iris_msg_hdr_t rep;
            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
            zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_GET_MODE: {
            uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_pty_mode_t)] __attribute__((aligned(8)));
            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            iris_pty_mode_t* resp = (iris_pty_mode_t*)(rep_buf + sizeof(iris_msg_hdr_t));

            pty_lock(&g_pty);
            resp->flags = g_pty.mode_flags;
            pty_unlock(&g_pty);

            iris_msg_hdr_init_reply(rep_hdr, req, ZX_OK, sizeof(iris_pty_mode_t));
            zx_channel_write(ch, 0, rep_buf, sizeof(rep_buf), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_SET_WINDOW: {
            if (payload_len >= sizeof(iris_pty_window_t)) {
                pty_lock(&g_pty);
                g_pty.window = *(const iris_pty_window_t*)payload;
                pty_unlock(&g_pty);
            }
            iris_msg_hdr_t rep;
            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
            zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_GET_WINDOW: {
            uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_pty_window_t)] __attribute__((aligned(8)));
            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            iris_pty_window_t* resp = (iris_pty_window_t*)(rep_buf + sizeof(iris_msg_hdr_t));

            pty_lock(&g_pty);
            *resp = g_pty.window;
            pty_unlock(&g_pty);

            iris_msg_hdr_init_reply(rep_hdr, req, ZX_OK, sizeof(iris_pty_window_t));
            zx_channel_write(ch, 0, rep_buf, sizeof(rep_buf), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_DRAIN: {
            iris_msg_hdr_t rep;
            iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
            zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_SIGNAL: {
            uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_pty_signal_req_t)] __attribute__((aligned(8)));
            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            iris_pty_signal_req_t* resp = (iris_pty_signal_req_t*)(rep_buf + sizeof(iris_msg_hdr_t));

            pty_lock(&g_pty);
            resp->signo = g_pty.pending_signal;
            g_pty.pending_signal = 0;
            pty_unlock(&g_pty);

            iris_msg_hdr_init_reply(rep_hdr, req, ZX_OK, sizeof(iris_pty_signal_req_t));
            zx_channel_write(ch, 0, rep_buf, sizeof(rep_buf), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_INJECT: {
            uint32_t count = 0;
            if (payload_len >= sizeof(iris_pty_inject_req_t)) {
                count = ((const iris_pty_inject_req_t*)payload)->count;
            }
            if (count > 0 && payload_len >= sizeof(iris_pty_inject_req_t) + count) {
                const char* data = (const char*)(payload + sizeof(iris_pty_inject_req_t));
                for (uint32_t i = 0; i < count; i++) {
                    pty_feed_char(&g_pty, data[i]);
                }
            }

            uint8_t rep_buf[sizeof(iris_msg_hdr_t) + sizeof(iris_pty_inject_resp_t)] __attribute__((aligned(8)));
            iris_msg_hdr_t* rep_hdr = (iris_msg_hdr_t*)rep_buf;
            iris_pty_inject_resp_t* resp = (iris_pty_inject_resp_t*)(rep_buf + sizeof(iris_msg_hdr_t));
            resp->bytes_injected = count;

            iris_msg_hdr_init_reply(rep_hdr, req, ZX_OK, sizeof(iris_pty_inject_resp_t));
            zx_channel_write(ch, 0, rep_buf, sizeof(rep_buf), NULL, 0);
            break;
        }

        case IRIS_PTY_OP_OPEN_CLIENT: {
            zx_handle_t srv = ZX_HANDLE_INVALID;
            zx_handle_t cli = ZX_HANDLE_INVALID;
            zx_status_t cr_st = zx_channel_create(0, &srv, &cli);

            if (cr_st == ZX_OK && g_num_clients < PTY_MAX_CLIENTS) {
                g_client_channels[g_num_clients++] = srv;
                iris_msg_hdr_t rep;
                iris_msg_hdr_init_reply(&rep, req, ZX_OK, 0);
                zx_channel_write(ch, 0, &rep, sizeof(rep), &cli, 1);
            } else {
                if (srv != ZX_HANDLE_INVALID) zx_handle_close(srv);
                if (cli != ZX_HANDLE_INVALID) zx_handle_close(cli);
                iris_msg_hdr_t rep;
                iris_msg_hdr_init_reply(&rep, req, ZX_ERR_NO_RESOURCES, 0);
                zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            }
            break;
        }

        default: {
            iris_msg_hdr_t rep;
            iris_msg_hdr_init_reply(&rep, req, ZX_ERR_NOT_SUPPORTED, 0);
            zx_channel_write(ch, 0, &rep, sizeof(rep), NULL, 0);
            break;
        }
    }

    return true;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /* 1. Extract capabilities */
    g_root_resource = iris_root_resource();
    if (g_root_resource == ZX_HANDLE_INVALID) {
        g_root_resource = zx_take_startup_handle(PA_HND(PA_RESOURCE, 0));
    }
    g_debuglog_handle = iris_get_debuglog();
    if (g_debuglog_handle == ZX_HANDLE_INVALID) {
        g_debuglog_handle = iris_fd_get_handle(1);
    }

    g_server_channel = ZX_HANDLE_INVALID;
    g_num_clients = 0;
    for (uint32_t i = 0; i < PTY_MAX_CLIENTS; i++) {
        zx_handle_t h = zx_take_startup_handle(PA_HND(PA_USER0, i));
        if (h != ZX_HANDLE_INVALID) {
            if (g_server_channel == ZX_HANDLE_INVALID) {
                g_server_channel = h;
            }
            g_client_channels[g_num_clients++] = h;
        }
    }

    log_status(STATUS_OKAY, "iris_pty", "Starting Pseudo-Terminal Server.");

    /* 2. Initialize line discipline */
    memset(&g_pty, 0, sizeof(g_pty));
    g_pty.mode_flags = IRIS_PTY_MODE_CANONICAL | IRIS_PTY_MODE_ECHO;
    g_pty.window.rows = 24;
    g_pty.window.cols = 80;

    /* 3. Discover and initialize PCIe VirtIO Input device */
    uint64_t ecam_phys = pci_get_default_ecam_base();
    zx_handle_t ecam_vmo = ZX_HANDLE_INVALID;
    zx_status_t st = zx_vmo_create_physical(g_root_resource, ecam_phys, PCI_ECAM_SIZE_16_BUSES, &ecam_vmo);
    if (st == ZX_OK) {
        zx_vaddr_t ecam_vaddr = 0;
        st = zx_vmar_map(zx_vmar_root_self(),
                         ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_MAP_RANGE,
                         0, ecam_vmo, 0, PCI_ECAM_SIZE_16_BUSES, &ecam_vaddr);
        zx_handle_close(ecam_vmo);

        if (st == ZX_OK) {
            st = virtio_input_init(ecam_vaddr, g_root_resource, &g_input_dev);
            if (st == ZX_OK) {
                g_has_virtio_input = true;
                log_status(STATUS_OKAY, "iris_pty", "Bound to PCI %02x:%02x.%x (Vendor: 0x%04x, Device: 0x%04x).",
                           g_input_dev.pci_dev.bus, g_input_dev.pci_dev.dev, g_input_dev.pci_dev.func,
                           g_input_dev.pci_dev.vendor_id, g_input_dev.pci_dev.device_id);
            } else {
                log_status(STATUS_OKAY, "iris_pty", "virtio-input-pci not present; fallback to UART serial console.");
            }
        }
    }

    /* 4. Start UART serial console input worker only as fallback when VirtIO input is absent */
    if (!g_has_virtio_input) {
        init_uart_fallback();
        log_status(STATUS_OKAY, "iris_pty", "UART serial console worker initialized.");
    } else {
        log_status(STATUS_OKAY, "iris_pty", "Primary input active via virtio-input-pci.");
    }

    log_status(STATUS_OKAY, "iris_pty", "Line discipline initialized (canonical cooked mode, echoing enabled).");
    log_status(STATUS_OKAY, "iris_pty", "IPC server listening on control channel.");

    /* 5. Main Server Event Loop */
    while (!g_shutdown && g_num_clients > 0) {
        /* Poll VirtIO keyboard if active */
        if (g_has_virtio_input) {
            char kbuf[32];
            size_t nchars = 0;
            virtio_input_poll(&g_input_dev, kbuf, sizeof(kbuf), &nchars);
            for (size_t i = 0; i < nchars; i++) {
                pty_feed_char(&g_pty, kbuf[i]);
            }
        }

        /* Wait on client channels */
        zx_wait_item_t wait_items[PTY_MAX_CLIENTS];
        for (uint32_t i = 0; i < g_num_clients; i++) {
            wait_items[i].handle = g_client_channels[i];
            wait_items[i].waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
            wait_items[i].pending = 0;
        }

        zx_status_t wait_st = zx_object_wait_many(wait_items, g_num_clients,
                                                  zx_deadline_after(ZX_MSEC(10)));
        if (wait_st != ZX_OK && wait_st != ZX_ERR_TIMED_OUT) {
            break;
        }

        for (uint32_t i = 0; i < g_num_clients; i++) {
            if (wait_items[i].pending & ZX_CHANNEL_READABLE) {
                handle_pty_message(g_client_channels[i]);
            }
            if (wait_items[i].pending & ZX_CHANNEL_PEER_CLOSED) {
                /* If main server channel closes, shut down */
                if (g_client_channels[i] == g_server_channel) {
                    g_shutdown = true;
                    break;
                }
                zx_handle_close(g_client_channels[i]);
                /* Remove client channel from array */
                for (uint32_t j = i; j < g_num_clients - 1; j++) {
                    g_client_channels[j] = g_client_channels[j + 1];
                }
                g_num_clients--;
                i--;
            }
        }
    }

    /* 7. Clean Teardown */
    if (g_has_virtio_input) {
        virtio_input_teardown(&g_input_dev);
    }

    for (uint32_t i = 0; i < g_num_clients; i++) {
        if (g_client_channels[i] != ZX_HANDLE_INVALID) {
            zx_handle_close(g_client_channels[i]);
        }
    }

    log_status(STATUS_OKAY, "iris_pty", "Daemon exited cleanly.");
    zx_process_exit(0);
    return 0;
}
