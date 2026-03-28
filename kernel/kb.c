/*
 * Anykernel OS — PS/2 Keyboard Driver
 *
 * Reads scancodes from the PS/2 keyboard controller (port 0x60).
 * Converts Set 1 scancodes to ASCII via a lookup table.
 * Supports Shift and Caps Lock for uppercase.
 * Maintains a small ring buffer for key events.
 */

#include "kb.h"
#include "io.h"
#include "pic.h"
#include "log.h"
#include <stdint.h>

/* ── Scancode Set 1 → ASCII tables ───────────────────────────── */

static const char sc_lower[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0
};

static const char sc_upper[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 0, 0
};

/* Modifier scancodes */
#define SC_LSHIFT_PRESS   0x2A
#define SC_RSHIFT_PRESS   0x36
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_RELEASE 0xB6
#define SC_CAPSLOCK       0x3A

/* Modifier state */
static uint8_t shift_held = 0;
static uint8_t caps_on = 0;

/* ── Ring buffer ─────────────────────────────────────────────── */

#define KB_BUF_SIZE 64

static char    kb_buffer[KB_BUF_SIZE];
static uint8_t kb_head = 0;
static uint8_t kb_tail = 0;

static void kb_buf_push(char c) {
    uint8_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next == kb_tail) return;  /* Buffer full, drop */
    kb_buffer[kb_head] = c;
    kb_head = next;
}

/* ── Public API ──────────────────────────────────────────────── */

void kb_init(void) {
    /* Flush any pending scancodes */
    while (inb(0x64) & 0x01) {
        inb(0x60);
    }
    pic_unmask(IRQ_KEYBOARD);
    LOG_OK("Keyboard initialized (PS/2, Set 1, Shift+CapsLock)");
}

void kb_irq_handler(void) {
    uint8_t scancode = inb(0x60);

    /* Handle modifier keys */
    if (scancode == SC_LSHIFT_PRESS || scancode == SC_RSHIFT_PRESS) {
        shift_held = 1;
        return;
    }
    if (scancode == SC_LSHIFT_RELEASE || scancode == SC_RSHIFT_RELEASE) {
        shift_held = 0;
        return;
    }
    if (scancode == SC_CAPSLOCK) {
        caps_on = !caps_on;
        return;
    }

    /* Ignore other key releases and extended scancodes */
    if (scancode & 0x80) return;
    if (scancode == 0xE0 || scancode == 0xE1) return;

    /* Determine if we should use uppercase */
    int use_upper = shift_held ^ caps_on;
    char c = use_upper ? sc_upper[scancode] : sc_lower[scancode];

    if (c != 0) {
        kb_buf_push(c);
    }
}

char kb_getchar(void) {
    if (kb_head == kb_tail) return 0;
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

bool kb_has_input(void) {
    return kb_head != kb_tail;
}
