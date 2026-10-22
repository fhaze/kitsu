#!/usr/bin/env python
"""Run against a built package: python windows/test_opengl.py windows-package/kitsu -v."""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import tempfile
import unittest
from ctypes import wintypes
from pathlib import Path
from typing import Any

PACKAGE = Path(globals().get('PACKAGE', 'windows-package/kitsu')).resolve()


def require(condition: Any, message: Any) -> None:
    if not condition:
        raise RuntimeError(str(message))


def system_probe() -> None:
    class PixelFormat(ctypes.Structure):
        _fields_ = [('size', wintypes.WORD), ('version', wintypes.WORD), ('flags', wintypes.DWORD)] + [
            (name, wintypes.BYTE) for name in (
                'pixel_type', 'color', 'red', 'red_shift', 'green', 'green_shift', 'blue', 'blue_shift',
                'alpha', 'alpha_shift', 'accum', 'accum_red', 'accum_green', 'accum_blue', 'accum_alpha',
                'depth', 'stencil', 'aux', 'layer', 'reserved',
            )
        ] + [(name, wintypes.DWORD) for name in ('layer_mask', 'visible_mask', 'damage_mask')]

    user = ctypes.WinDLL('user32')
    gdi = ctypes.WinDLL('gdi32')
    gl = ctypes.WinDLL(str(Path(os.environ['SYSTEMROOT']) / 'System32/opengl32.dll'))
    user.CreateWindowExW.argtypes = [
        wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p,
    ]
    user.CreateWindowExW.restype = wintypes.HWND
    user.GetDC.argtypes, user.GetDC.restype = [wintypes.HWND], wintypes.HDC
    user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    user.DestroyWindow.argtypes = [wintypes.HWND]
    gdi.ChoosePixelFormat.argtypes = [wintypes.HDC, ctypes.POINTER(PixelFormat)]
    gdi.SetPixelFormat.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.POINTER(PixelFormat)]
    gl.wglCreateContext.argtypes, gl.wglCreateContext.restype = [wintypes.HDC], ctypes.c_void_p
    gl.wglMakeCurrent.argtypes = [wintypes.HDC, ctypes.c_void_p]
    gl.wglDeleteContext.argtypes = [ctypes.c_void_p]
    gl.glGetString.argtypes, gl.glGetString.restype = [ctypes.c_uint], ctypes.c_char_p
    window = user.CreateWindowExW(0, 'STATIC', 'System OpenGL probe', 0x06000000, 0, 0, 1, 1, None, None, None, None)
    dc = user.GetDC(window) if window else None
    context = None
    result = {}
    try:
        pfd = PixelFormat(size=ctypes.sizeof(PixelFormat), version=1, flags=0x25, color=24)
        pixel_format = gdi.ChoosePixelFormat(dc, ctypes.byref(pfd)) if dc else 0
        if pixel_format and gdi.SetPixelFormat(dc, pixel_format, ctypes.byref(pfd)):
            context = gl.wglCreateContext(dc)
        if context and gl.wglMakeCurrent(dc, context):
            result = {'version': gl.glGetString(0x1F02).decode(), 'renderer': gl.glGetString(0x1F01).decode()}
    finally:
        gl.wglMakeCurrent(None, None)
        if context:
            gl.wglDeleteContext(context)
        if dc:
            user.ReleaseDC(window, dc)
        if window:
            user.DestroyWindow(window)
    print(json.dumps(result))


def probe() -> None:
    ctypes.WinDLL(str(Path(os.environ['SYSTEMROOT']) / 'System32/opengl32.dll'))
    dll_directory = os.add_dll_directory(str(PACKAGE / 'bin'))
    glfw = ctypes.CDLL(str(PACKAGE / 'lib/kitty/kitty/glfw-win32.dll'))
    glfw.glfwInit.argtypes = [ctypes.c_longlong, ctypes.POINTER(ctypes.c_bool)]
    glfw.glfwInit.restype = ctypes.c_int
    glfw.glfwWindowHint.argtypes = [ctypes.c_int, ctypes.c_int]
    glfw.glfwCreateWindow.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    glfw.glfwCreateWindow.restype = ctypes.c_void_p
    glfw.glfwDestroyWindow.argtypes = [ctypes.c_void_p]
    glfw.glfwMakeContextCurrent.argtypes = [ctypes.c_void_p]
    glfw.glfwSwapBuffers.argtypes = [ctypes.c_void_p]
    glfw.glfwGetProcAddress.argtypes = [ctypes.c_char_p]
    glfw.glfwGetProcAddress.restype = ctypes.c_void_p
    glfw.glfwGetError.argtypes = [ctypes.POINTER(ctypes.c_char_p)]
    errors: list[str] = []
    error_callback = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)

    @error_callback
    def on_error(code: int, description: bytes) -> None:
        errors.append(description.decode())

    glfw.glfwSetErrorCallback.argtypes = [error_callback]
    glfw.glfwSetErrorCallback(on_error)
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel32.GetModuleHandleW.restype = ctypes.c_void_p
    kernel32.GetModuleFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint]
    results = []
    for _ in range(2):  # Includes terminate/reinitialize, not just a second window.
        require(glfw.glfwInit(0, ctypes.byref(ctypes.c_bool())), 'glfwInit failed')
        glfw.glfwWindowHint(0x00020004, 0)  # GLFW_VISIBLE
        glfw.glfwWindowHint(0x00022002, 3)  # GLFW_CONTEXT_VERSION_MAJOR
        glfw.glfwWindowHint(0x00022003, 1)  # GLFW_CONTEXT_VERSION_MINOR
        glfw.glfwWindowHint(0x00022006, 1)  # GLFW_OPENGL_FORWARD_COMPAT, as used by Kitsu
        first = glfw.glfwCreateWindow(64, 64, b'OpenGL regression', None, None, None)
        if not first:
            error = ctypes.c_char_p()
            glfw.glfwGetError(ctypes.byref(error))
            message = error.value.decode() if error.value else 'unknown'
            glfw.glfwTerminate()
            print(json.dumps({'error': message}))
            return
        second = glfw.glfwCreateWindow(64, 64, b'Shared OpenGL regression', None, first, None)
        require(second, 'Shared context creation failed')
        glfw.glfwMakeContextCurrent(first)

        def function(name: str, result: Any, *args: Any) -> Any:
            address = glfw.glfwGetProcAddress(name.encode())
            require(address, name)
            return ctypes.WINFUNCTYPE(result, *args)(address)

        get_string = function('glGetString', ctypes.c_char_p, ctypes.c_uint)
        clear_color = function('glClearColor', None, *([ctypes.c_float] * 4))
        clear = function('glClear', None, ctypes.c_uint)
        read_pixels = function('glReadPixels', None, *([ctypes.c_int] * 4), ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p)
        get_error = function('glGetError', ctypes.c_uint)
        gen_textures = function('glGenTextures', None, ctypes.c_int, ctypes.POINTER(ctypes.c_uint))
        bind_texture = function('glBindTexture', None, ctypes.c_uint, ctypes.c_uint)
        is_texture = function('glIsTexture', ctypes.c_ubyte, ctypes.c_uint)
        texture = ctypes.c_uint()
        gen_textures(1, ctypes.byref(texture))
        bind_texture(0x0DE1, texture.value)  # GL_TEXTURE_2D
        for window in (first, second):
            glfw.glfwMakeContextCurrent(window)
            require(is_texture(texture.value), 'Texture is not shared between contexts')
            clear_color(0.25, 0.5, 0.75, 1.0)
            clear(0x4000)  # GL_COLOR_BUFFER_BIT
            pixel = (ctypes.c_ubyte * 4)()
            read_pixels(0, 0, 1, 1, 0x1908, 0x1401, pixel)  # GL_RGBA, GL_UNSIGNED_BYTE
            require(all(abs(actual - expected) <= 2 for actual, expected in zip(pixel, (64, 128, 191, 255))), list(pixel))
            glfw.glfwSwapBuffers(window)
            require(get_error() == 0, 'OpenGL error')
        version = get_string(0x1F02).decode()
        renderer = get_string(0x1F01).decode()
        module_path = ctypes.create_unicode_buffer(32768)
        module = kernel32.GetModuleHandleW(str(PACKAGE / 'bin/mesa/opengl32.dll')) or kernel32.GetModuleHandleW('opengl32.dll')
        require(kernel32.GetModuleFileNameW(module, module_path, len(module_path)), 'OpenGL DLL path')
        major, minor = map(int, version.split()[0].split('.')[:2])
        require((major, minor) >= (3, 1), version)
        results.append({'version': version, 'renderer': renderer, 'pixel': list(pixel), 'library': module_path.value})
        glfw.glfwMakeContextCurrent(None)
        glfw.glfwDestroyWindow(first)
        glfw.glfwDestroyWindow(second)
        glfw.glfwTerminate()
    dll_directory.close()
    require(not errors, errors)
    print(json.dumps({'contexts': results}))


class OpenGLFallback(unittest.TestCase):
    def run_probe(self, driver: str, version_override: str = '', *, system: bool = False) -> dict[str, Any]:
        env = os.environ.copy()
        env['KITSU_OPENGL_DRIVER'] = driver
        env['GALLIUM_DRIVER'] = 'not-a-driver'  # The forced software path must override this.
        env.pop('MESA_GL_VERSION_OVERRIDE', None)
        if version_override:
            env['MESA_GL_VERSION_OVERRIDE'] = version_override
        env['PATH'] = os.pathsep.join((env['SYSTEMROOT'], str(Path(env['SYSTEMROOT']) / 'System32')))
        mode = '__system_opengl_probe__' if system else '__opengl_probe__'
        code = f'import runpy; runpy.run_path({str(Path(__file__).resolve())!r}, init_globals={{"PACKAGE": {str(PACKAGE)!r}}}, run_name={mode!r})'
        with tempfile.TemporaryDirectory(prefix='kitsu-gl-') as cwd:
            # Neither the current directory nor a development toolchain should supply GL.
            for name in ('opengl32.dll', 'libgallium_wgl.dll'):
                Path(cwd, name).write_bytes(b'not a DLL')
            completed = subprocess.run([str(PACKAGE / 'bin/kitsu.exe'), '+runpy', code], env=env, cwd=cwd, capture_output=True, text=True, timeout=90)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        result = json.loads(completed.stdout)
        print(f'{driver}: {result}', flush=True)
        return result

    def test_native_uses_available_system_opengl(self) -> None:
        system = self.run_probe('native', system=True)
        if not system or tuple(map(int, system['version'].split()[0].split('.')[:2])) < (3, 1):
            self.skipTest('System OpenGL 3.1 is unavailable')
        result = self.run_probe('native')
        self.assertNotIn('error', result)
        for context in result['contexts']:
            self.assertEqual(context['renderer'], system['renderer'])
            self.assertEqual(Path(context['library']), Path(os.environ['SYSTEMROOT']) / 'System32/opengl32.dll')

    def test_forced_llvmpipe_renders_with_shared_contexts(self) -> None:
        self.assertFalse((PACKAGE / 'bin/opengl32.dll').exists())
        self.assertTrue((PACKAGE / 'bin/mesa/libgallium_wgl.dll').is_file())
        result = self.run_probe('llvmpipe')
        self.assertNotIn('error', result)
        for context in result['contexts']:
            self.assertIn('llvmpipe', context['renderer'].lower())
            self.assertEqual(Path(context['library']), PACKAGE / 'bin/mesa/opengl32.dll')

    def test_auto_retries_when_native_is_unavailable(self) -> None:
        native = self.run_probe('native')
        auto = self.run_probe('auto')
        self.assertNotIn('error', auto)
        if 'error' in native:
            for context in auto['contexts']:
                self.assertIn('llvmpipe', context['renderer'].lower())
        else:
            self.assertEqual(native['contexts'], auto['contexts'])

    def test_missing_mesa_has_actionable_error(self) -> None:
        mesa = PACKAGE / 'bin/mesa/opengl32.dll'
        hidden = mesa.with_suffix('.dll.disabled')
        mesa.rename(hidden)
        try:
            result = self.run_probe('llvmpipe')
            self.assertIn('Restore bin\\mesa', result['error'])
            self.assertIn('Compatibility Pack', result['error'])
        finally:
            hidden.rename(mesa)

    def test_invalid_driver_is_rejected(self) -> None:
        result = self.run_probe('invalid')
        self.assertIn('must be auto, native or llvmpipe', result['error'])

    def test_mesa_below_required_version_is_rejected(self) -> None:
        result = self.run_probe('llvmpipe', '2.1')
        self.assertIn('OpenGL 3.1 is unavailable', result['error'])


if __name__ == '__opengl_probe__':
    probe()
elif __name__ == '__system_opengl_probe__':
    system_probe()
elif __name__ == '__main__':
    if len(sys.argv) > 1:
        PACKAGE = Path(sys.argv.pop(1)).resolve()
    unittest.main()
