#!/usr/bin/env python3
"""Compile the allocation paths unchanged, with fault injection and minimal OS stubs."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>

#ifdef NATIVE_TEST_WIN64
#ifndef _WIN64
#error Native Windows tests require a Win64 compiler
#endif
_Static_assert(sizeof(void *) == 8 && sizeof(size_t) == 8 && sizeof(long) == 4, "Expected LLP64");
#endif
"""

ALLOCATORS = r"""
static int fail_allocation;
static void *test_malloc(size_t size) {
    if (fail_allocation && !--fail_allocation) return NULL;
    void *p = malloc(size);
    if (p) memset(p, 0xa5, size);
    return p;
}
static void *test_calloc(size_t count, size_t size) {
    void *p = test_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
#define malloc test_malloc
#define calloc test_calloc
"""


def source(path: str, start: str, end: str) -> str:
    text = (ROOT / path).read_text()
    return text[text.index(start) : text.index(end, text.index(start))]


class NativeAllocations(unittest.TestCase):
    def compile_and_run(self, code: str, *, allocation_stubs: bool = True) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'test.c'
            exe = Path(tmp) / 'test.exe'
            cfile.write_text(PREAMBLE + (ALLOCATORS if allocation_stubs else '') + code)
            command = shlex.split(os.environ.get('CC', 'gcc'))
            command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g']
            if os.name == 'nt':
                command.append('-DNATIVE_TEST_WIN64')
            command += shlex.split(os.environ.get('NATIVE_TEST_CFLAGS', ''))
            subprocess.run([*command, str(cfile), '-o', str(exe), '-lm'], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_cli_terminator(self) -> None:
        allocator = source('kitty/launcher/cli-parser.h', 'static void *\nalloc_for_cli(', '\n#define set_err')
        self.compile_and_run(
            r"""
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct {
    struct {
        struct { char *buf; size_t capacity, used; } *items;
        size_t count, capacity;
    } blocks;
} CLISpec;
"""
            + allocator
            + r"""
int main(void) {
    for (int failure = 1; failure <= 2; failure++) {
        CLISpec spec = {0};
        fail_allocation = failure;
        assert(!alloc_for_cli(&spec, 8));
        for (size_t i = 0; i < spec.blocks.count; i++) free(spec.blocks.items[i].buf);
        free(spec.blocks.items);
    }
    const size_t lengths[] = {0, 1, 7, 8, 8190, 8191, 8192, 8193, 32768};
    for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
        CLISpec spec = {0};
        const size_t len = lengths[n];
        char *arg = malloc(len + 1);
        assert(arg);
        memset(arg, 'x', len);
        arg[len] = 0;
        for (int i = 0; i < 3; i++) {
            char *copy = alloc_for_cli(&spec, strlen(arg));
            assert(copy && (uintptr_t)copy % sizeof(void *) == 0);
            memcpy(copy, arg, len);
            assert(copy[len] == 0 && strcmp(copy, arg) == 0);
        }
        for (size_t i = 0; i < spec.blocks.count; i++) free(spec.blocks.items[i].buf);
        free(spec.blocks.items);
        free(arg);
    }
    CLISpec spec = {0};
    char *copies[40];
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
        copies[i] = alloc_for_cli(&spec, 8191);
        assert(copies[i] && (uintptr_t)copies[i] % sizeof(void *) == 0);
        memset(copies[i], 'a' + i % 26, 8191);
        assert(copies[i][8191] == 0);
    }
    assert(spec.blocks.count > 8 && spec.blocks.capacity > 8);
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
        for (size_t j = 0; j < 8191; j++) assert(copies[i][j] == 'a' + (int)(i % 26));
        assert(copies[i][8191] == 0);
    }
    for (size_t i = 0; i < spec.blocks.count; i++) free(spec.blocks.items[i].buf);
    free(spec.blocks.items);
    return 0;
}
"""
        )

    def test_box_glyph_capacity(self) -> None:
        checks = source('kitty/fonts.c', '    if (width <= 0 || height <= 0', '    RAII_PyObject(ans,')
        factor = source('kitty/decorations.h', '#define SUPERSAMPLE_FACTOR ', '\n')
        self.compile_and_run(
            factor
            + r"""
typedef intptr_t Py_ssize_t;
#define PY_SSIZE_T_MAX INTPTR_MAX
typedef struct { size_t pixels, capacity; } PyObject;
enum { PyExc_ValueError = 1, PyExc_OverflowError };
static int error;
static void PyErr_SetString(int exception, const char *message) {
    (void)message;
    error = exception;
}
static PyObject result;
static PyObject *glyph_capacity(Py_ssize_t width, Py_ssize_t height, double scale, double dpi_x, double dpi_y) {
    error = 0;
"""
            + checks
            + r"""
    result.pixels = pixels;
    result.capacity = capacity;
    return &result;
}
int main(void) {
    const Py_ssize_t dimensions[] = {1, 2, 16, 1023, 1024};
    for (size_t i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
        for (size_t j = 0; j < sizeof(dimensions) / sizeof(dimensions[0]); j++) {
            const size_t pixels = (size_t)dimensions[i] * dimensions[j];
            PyObject *ans = glyph_capacity(dimensions[i], dimensions[j], 1., 96., 96.);
            assert(ans && !error && ans->pixels == pixels);
            assert(ans->capacity == pixels + pixels * SUPERSAMPLE_FACTOR * SUPERSAMPLE_FACTOR);
            assert(ans->capacity <= (size_t)PY_SSIZE_T_MAX);
        }
    }
    const Py_ssize_t invalid[] = {-1, 0, 1025, INTPTR_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!glyph_capacity(invalid[i], 1, 1., 96., 96.) && error == PyExc_ValueError);
        assert(!glyph_capacity(1, invalid[i], 1., 96., 96.) && error == PyExc_ValueError);
    }
#ifdef NATIVE_TEST_WIN64
    const Py_ssize_t beyond_ulong = (Py_ssize_t)ULONG_MAX + 1;
    assert(!glyph_capacity(beyond_ulong, 1, 1., 96., 96.) && error == PyExc_ValueError);
    assert(!glyph_capacity(1, beyond_ulong, 1., 96., 96.) && error == PyExc_ValueError);
#endif
    const double invalid_scale[] = {0., -1., NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(invalid_scale) / sizeof(invalid_scale[0]); i++) {
        const double x = invalid_scale[i];
        assert(!glyph_capacity(1, 1, x, 96., 96.) && error == PyExc_ValueError);
        assert(!glyph_capacity(1, 1, 1., x, 96.) && error == PyExc_ValueError);
        assert(!glyph_capacity(1, 1, 1., 96., x) && error == PyExc_ValueError);
    }
    return 0;
}
""",
            allocation_stubs=False,
        )

    def test_raw_input(self) -> None:
        handler = source('glfw/win32_window.c', '        case WM_INPUT: {', '\n        case WM_MOUSELEAVE:')
        self.compile_and_run(
            r"""
typedef unsigned int UINT;
typedef uintptr_t HRAWINPUT;
typedef struct { int unused; } RAWINPUTHEADER;
typedef struct { struct { struct { unsigned usFlags; int lLastX, lLastY; } mouse; } data; } RAWINPUT;
typedef struct {
    bool rawMouseMotion;
    double virtualCursorPosX, virtualCursorPosY;
    struct { int lastCursorPosX, lastCursorPosY; } win32;
} _GLFWwindow;
static struct { struct { _GLFWwindow *disabledCursorWindow; RAWINPUT *rawInput; int rawInputSize; } win32; } _glfw;
enum { WM_INPUT = 1, RID_INPUT, GLFW_PLATFORM_ERROR, GLFW_OUT_OF_MEMORY, MOUSE_MOVE_ABSOLUTE = 8 };
static UINT required = sizeof(RAWINPUT);
static bool query_fails, read_fails;
static int errors, reads, moves;
static RAWINPUT event;
static UINT GetRawInputData(HRAWINPUT ri, UINT command, void *buffer, UINT *size, UINT header_size) {
    (void)ri; (void)command; (void)header_size;
    if (!buffer) {
        if (query_fails) return (UINT)-1;
        *size = required;
        return 0;
    }
    reads++;
    if (read_fails) return (UINT)-1;
    assert(*size >= required && required >= sizeof(event));
    memcpy(buffer, &event, sizeof(event));
    *size = required;
    return required;
}
static void _glfwInputError(int error, const char *message) { (void)error; (void)message; errors++; }
static void _glfwInputCursorPos(_GLFWwindow *window, double x, double y) {
    moves++;
    window->virtualCursorPosX = x;
    window->virtualCursorPosY = y;
}
static void input(_GLFWwindow *window) {
    const uintptr_t lParam = 1;
    switch (WM_INPUT) {
"""
            + handler
            + r"""
    }
}
int main(void) {
    _GLFWwindow window = {.rawMouseMotion = true};
    _glfw.win32.disabledCursorWindow = &window;
    fail_allocation = 1;
    input(&window);
    assert(errors == 1 && !reads && !moves && !_glfw.win32.rawInput && !_glfw.win32.rawInputSize);
    event.data.mouse.lLastX = 3;
    event.data.mouse.lLastY = -2;
    input(&window);
    assert(reads == 1 && moves == 1 && window.virtualCursorPosX == 3 && window.virtualCursorPosY == -2);
    RAWINPUT *old = _glfw.win32.rawInput;
    const int old_size = _glfw.win32.rawInputSize;
    fail_allocation = 1;
    input(&window);
    assert(fail_allocation == 1 && reads == 2 && moves == 2 && _glfw.win32.rawInput == old);
    required *= 2;
    input(&window);
    assert(errors == 2 && reads == 2 && moves == 2 && _glfw.win32.rawInput == old && _glfw.win32.rawInputSize == old_size);
    assert(old->data.mouse.lLastX == 3);
    input(&window);
    assert(reads == 3 && moves == 3 && _glfw.win32.rawInputSize == (int)required);
    query_fails = true;
    input(&window);
    assert(errors == 3 && reads == 3 && moves == 3);
    query_fails = false;
    required = 0;
    input(&window);
    assert(errors == 4 && reads == 3 && moves == 3);
    required = sizeof(RAWINPUT);
    read_fails = true;
    input(&window);
    assert(errors == 5 && reads == 4 && moves == 3);
    read_fails = false;
    event.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
    event.data.mouse.lLastX = 20;
    event.data.mouse.lLastY = 40;
    input(&window);
    assert(reads == 5 && moves == 4 && window.win32.lastCursorPosX == 20 && window.win32.lastCursorPosY == 40);
    old = _glfw.win32.rawInput;
    required = UINT_MAX;
    fail_allocation = 1;
    input(&window);
    assert(errors == 6 && reads == 5 && moves == 4 && _glfw.win32.rawInput == old);
    assert(_glfw.win32.rawInputSize == old_size * 2);
    free(_glfw.win32.rawInput);
    return 0;
}
"""
        )

    def test_shadow_tile(self) -> None:
        shadow = source('glfw/wl_client_side_decorations.c', 'typedef float kernel_type;', '\n\n// }}}')
        self.compile_and_run(
            r"""
typedef struct {
    struct {
        struct {
            struct { double width; } metrics;
            struct { double fscale; } for_window_state;
            bool titlebar_hidden;
            struct { uint32_t *data; size_t for_decoration_size, segments, stride, corner_size; } shadow_tile;
        } decorations;
    } wl;
} _GLFWwindow;
#define decs window->wl.decorations
"""
            + shadow
            + r"""
int main(void) {
    _GLFWwindow w = {0}, *window = &w;
    decs.metrics.width = 2;
    decs.for_window_state.fscale = 1.5;
    for (int failure = 1; failure <= 2; failure++) {
        fail_allocation = failure;
        assert(create_shadow_tile(window) == 0);
        assert(!st.data && !st.stride && !st.for_decoration_size);
    }
    assert(create_shadow_tile(window) == 3);
    assert(st.data && st.segments == 7 && st.stride == 21 && st.corner_size == 9 && st.for_decoration_size == 3);
    assert(st.data[10 * st.stride + 10] > st.data[0]);
    for (size_t i = 0; i < st.stride * st.stride; i++) assert((st.data[i] & 0xffffff) == 0);
    uint32_t *old = st.data;
    const uint32_t first = old[0];
    fail_allocation = 1;
    assert(create_shadow_tile(window) == 3 && st.data == old && fail_allocation == 1);
    decs.metrics.width = 4;
    for (int failure = 1; failure <= 2; failure++) {
        fail_allocation = failure;
        assert(create_shadow_tile(window) == 0);
        assert(st.data == old && old[0] == first && st.stride == 21 && st.corner_size == 9 && st.for_decoration_size == 3);
    }
    assert(create_shadow_tile(window) == 6);
    assert(st.data && st.stride == 42 && st.corner_size == 18 && st.for_decoration_size == 6);
    for (int i = 0; i < 10; i++) {
        decs.metrics.width = 0;
        assert(create_shadow_tile(window) == 0 && !st.data && !st.for_decoration_size);
        decs.metrics.width = 2 + i % 3;
        decs.titlebar_hidden = i % 2;
        for (int failure = 1; failure <= 2; failure++) {
            fail_allocation = failure;
            assert(create_shadow_tile(window) == 0 && !st.data && !st.for_decoration_size);
        }
        const size_t margin = (size_t)round(decs.metrics.width * decs.for_window_state.fscale);
        assert(create_shadow_tile(window) == margin && st.data && st.for_decoration_size == margin);
        assert(st.stride == 7 * margin && st.corner_size == 3 * margin);
    }
    free(st.data);
    return 0;
}
"""
        )


if __name__ == '__main__':
    unittest.main()
