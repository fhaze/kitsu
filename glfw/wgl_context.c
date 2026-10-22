//========================================================================
// GLFW 3.4 WGL - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2002-2006 Marcus Geelnard
// Copyright (c) 2006-2019 Camilla Löwy <elmindreda@glfw.org>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================
// Please use C89 style variable declarations in this file because VS 2010
//========================================================================

#include "internal.h"

#include <stdlib.h>
#include <malloc.h>
#include <assert.h>
#include <limits.h>

#define GL_RENDERER 0x1f01


// Chooses the framebuffer config that best matches the desired one
//
static const _GLFWfbconfig *
_glfwChooseFBConfig(const _GLFWfbconfig *desired, const _GLFWfbconfig *alternatives, unsigned int count) {
    unsigned int i;
    unsigned int missing, leastMissing = UINT_MAX;
    unsigned int colorDiff, leastColorDiff = UINT_MAX;
    unsigned int extraDiff, leastExtraDiff = UINT_MAX;
    const _GLFWfbconfig *current;
    const _GLFWfbconfig *closest = NULL;

    for (i = 0; i < count; i++) {
        current = alternatives + i;

        if (desired->stereo && !current->stereo) continue; // Stereo is a hard constraint

        missing = 0;
        if (desired->alphaBits > 0 && current->alphaBits == 0) missing++;
        if (desired->depthBits > 0 && current->depthBits == 0) missing++;
        if (desired->stencilBits > 0 && current->stencilBits == 0) missing++;
        if (desired->auxBuffers > 0 && current->auxBuffers < desired->auxBuffers) missing += desired->auxBuffers - current->auxBuffers;
        if (desired->samples > 0 && current->samples == 0) missing++;
        if (desired->transparent != current->transparent) missing++;
        if (desired->sRGB && !current->sRGB) missing++;

        colorDiff = 0;
        if (desired->redBits != GLFW_DONT_CARE) colorDiff += (desired->redBits - current->redBits) * (desired->redBits - current->redBits);
        if (desired->greenBits != GLFW_DONT_CARE) colorDiff += (desired->greenBits - current->greenBits) * (desired->greenBits - current->greenBits);
        if (desired->blueBits != GLFW_DONT_CARE) colorDiff += (desired->blueBits - current->blueBits) * (desired->blueBits - current->blueBits);

        extraDiff = 0;
        if (desired->alphaBits != GLFW_DONT_CARE) extraDiff += (desired->alphaBits - current->alphaBits) * (desired->alphaBits - current->alphaBits);
        if (desired->depthBits != GLFW_DONT_CARE) extraDiff += (desired->depthBits - current->depthBits) * (desired->depthBits - current->depthBits);
        if (desired->stencilBits != GLFW_DONT_CARE) extraDiff += (desired->stencilBits - current->stencilBits) * (desired->stencilBits - current->stencilBits);
        if (desired->accumRedBits != GLFW_DONT_CARE)
            extraDiff += (desired->accumRedBits - current->accumRedBits) * (desired->accumRedBits - current->accumRedBits);
        if (desired->accumGreenBits != GLFW_DONT_CARE)
            extraDiff += (desired->accumGreenBits - current->accumGreenBits) * (desired->accumGreenBits - current->accumGreenBits);
        if (desired->accumBlueBits != GLFW_DONT_CARE)
            extraDiff += (desired->accumBlueBits - current->accumBlueBits) * (desired->accumBlueBits - current->accumBlueBits);
        if (desired->accumAlphaBits != GLFW_DONT_CARE)
            extraDiff += (desired->accumAlphaBits - current->accumAlphaBits) * (desired->accumAlphaBits - current->accumAlphaBits);
        if (desired->samples != GLFW_DONT_CARE) extraDiff += (desired->samples - current->samples) * (desired->samples - current->samples);
        if (desired->sRGB && !current->sRGB) extraDiff++;

        if (missing < leastMissing) closest = current;
        else if (missing == leastMissing) {
            if ((colorDiff < leastColorDiff) || (colorDiff == leastColorDiff && extraDiff < leastExtraDiff)) closest = current;
        }

        if (current == closest) {
            leastMissing = missing;
            leastColorDiff = colorDiff;
            leastExtraDiff = extraDiff;
        }
    }

    return closest;
}

// Return the value corresponding to the specified attribute
//
static int
findPixelFormatAttribValue(const int *attribs, int attribCount, const int *values, int attrib) {
    int i;

    for (i = 0; i < attribCount; i++) {
        if (attribs[i] == attrib) return values[i];
    }

    _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Unknown pixel format attribute requested");
    return 0;
}

#define addAttrib(a)                                                        \
    {                                                                       \
        assert((size_t)attribCount < sizeof(attribs) / sizeof(attribs[0])); \
        attribs[attribCount++] = a;                                         \
    }
#define findAttribValue(a) findPixelFormatAttribValue(attribs, attribCount, values, a)

// Return a list of available and usable framebuffer configs
//
static int
choosePixelFormat(_GLFWwindow *window, const _GLFWctxconfig *ctxconfig, const _GLFWfbconfig *fbconfig) {
    _GLFWfbconfig *usableConfigs;
    const _GLFWfbconfig *closest;
    int i, pixelFormat, nativeCount, usableCount = 0, attribCount = 0;
    int attribs[40];
    int values[sizeof(attribs) / sizeof(attribs[0])];

    nativeCount = _glfw.wgl.DescribePixelFormat(window->context.wgl.dc, 1, sizeof(PIXELFORMATDESCRIPTOR), NULL);

    if (_glfw.wgl.ARB_pixel_format) {
        // NOTE: In a Parallels VM WGL_ARB_pixel_format returns fewer pixel formats than
        //       DescribePixelFormat, violating the guarantees of the extension spec
        // HACK: Iterate through the minimum of both counts

        const int attrib = WGL_NUMBER_PIXEL_FORMATS_ARB;
        int extensionCount;

        if (!wglGetPixelFormatAttribivARB(window->context.wgl.dc, 1, 0, 1, &attrib, &extensionCount)) {
            _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to retrieve pixel format attribute");
            return 0;
        }

        nativeCount = _glfw_min(nativeCount, extensionCount);

        addAttrib(WGL_SUPPORT_OPENGL_ARB);
        addAttrib(WGL_DRAW_TO_WINDOW_ARB);
        addAttrib(WGL_PIXEL_TYPE_ARB);
        addAttrib(WGL_ACCELERATION_ARB);
        addAttrib(WGL_RED_BITS_ARB);
        addAttrib(WGL_RED_SHIFT_ARB);
        addAttrib(WGL_GREEN_BITS_ARB);
        addAttrib(WGL_GREEN_SHIFT_ARB);
        addAttrib(WGL_BLUE_BITS_ARB);
        addAttrib(WGL_BLUE_SHIFT_ARB);
        addAttrib(WGL_ALPHA_BITS_ARB);
        addAttrib(WGL_ALPHA_SHIFT_ARB);
        addAttrib(WGL_DEPTH_BITS_ARB);
        addAttrib(WGL_STENCIL_BITS_ARB);
        addAttrib(WGL_ACCUM_BITS_ARB);
        addAttrib(WGL_ACCUM_RED_BITS_ARB);
        addAttrib(WGL_ACCUM_GREEN_BITS_ARB);
        addAttrib(WGL_ACCUM_BLUE_BITS_ARB);
        addAttrib(WGL_ACCUM_ALPHA_BITS_ARB);
        addAttrib(WGL_AUX_BUFFERS_ARB);
        addAttrib(WGL_STEREO_ARB);
        addAttrib(WGL_DOUBLE_BUFFER_ARB);

        if (_glfw.wgl.ARB_multisample) addAttrib(WGL_SAMPLES_ARB);

        if (ctxconfig->client == GLFW_OPENGL_API) {
            if (_glfw.wgl.ARB_framebuffer_sRGB || _glfw.wgl.EXT_framebuffer_sRGB) addAttrib(WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB);
        } else {
            if (_glfw.wgl.EXT_colorspace) addAttrib(WGL_COLORSPACE_EXT);
        }
    }

    usableConfigs = calloc(nativeCount, sizeof(_GLFWfbconfig));

    for (i = 0; i < nativeCount; i++) {
        _GLFWfbconfig *u = usableConfigs + usableCount;
        pixelFormat = i + 1;

        if (_glfw.wgl.ARB_pixel_format) {
            // Get pixel format attributes through "modern" extension

            if (!wglGetPixelFormatAttribivARB(window->context.wgl.dc, pixelFormat, 0, attribCount, attribs, values)) {
                _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to retrieve pixel format attributes");

                free(usableConfigs);
                return 0;
            }

            if (!findAttribValue(WGL_SUPPORT_OPENGL_ARB) || !findAttribValue(WGL_DRAW_TO_WINDOW_ARB)) { continue; }

            if (findAttribValue(WGL_PIXEL_TYPE_ARB) != WGL_TYPE_RGBA_ARB) continue;

            if (!_glfw.wgl.mesa && findAttribValue(WGL_ACCELERATION_ARB) == WGL_NO_ACCELERATION_ARB) continue;

            if (findAttribValue(WGL_DOUBLE_BUFFER_ARB) != fbconfig->doublebuffer) continue;

            u->redBits = findAttribValue(WGL_RED_BITS_ARB);
            u->greenBits = findAttribValue(WGL_GREEN_BITS_ARB);
            u->blueBits = findAttribValue(WGL_BLUE_BITS_ARB);
            u->alphaBits = findAttribValue(WGL_ALPHA_BITS_ARB);

            u->depthBits = findAttribValue(WGL_DEPTH_BITS_ARB);
            u->stencilBits = findAttribValue(WGL_STENCIL_BITS_ARB);

            u->accumRedBits = findAttribValue(WGL_ACCUM_RED_BITS_ARB);
            u->accumGreenBits = findAttribValue(WGL_ACCUM_GREEN_BITS_ARB);
            u->accumBlueBits = findAttribValue(WGL_ACCUM_BLUE_BITS_ARB);
            u->accumAlphaBits = findAttribValue(WGL_ACCUM_ALPHA_BITS_ARB);

            u->auxBuffers = findAttribValue(WGL_AUX_BUFFERS_ARB);

            if (findAttribValue(WGL_STEREO_ARB)) u->stereo = true;
            u->transparent = fbconfig->transparent && u->alphaBits > 0;

            if (_glfw.wgl.ARB_multisample) u->samples = findAttribValue(WGL_SAMPLES_ARB);

            if (ctxconfig->client == GLFW_OPENGL_API) {
                if (_glfw.wgl.ARB_framebuffer_sRGB || _glfw.wgl.EXT_framebuffer_sRGB) {
                    if (findAttribValue(WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB)) u->sRGB = true;
                }
            } else {
                if (_glfw.wgl.EXT_colorspace) {
                    if (findAttribValue(WGL_COLORSPACE_EXT) == WGL_COLORSPACE_SRGB_EXT) u->sRGB = true;
                }
            }
        } else {
            // Get pixel format attributes through legacy PFDs

            PIXELFORMATDESCRIPTOR pfd;

            if (!_glfw.wgl.DescribePixelFormat(window->context.wgl.dc, pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd)) {
                _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to describe pixel format");

                free(usableConfigs);
                return 0;
            }

            if (!(pfd.dwFlags & PFD_DRAW_TO_WINDOW) || !(pfd.dwFlags & PFD_SUPPORT_OPENGL)) { continue; }

            if (!_glfw.wgl.mesa && !(pfd.dwFlags & PFD_GENERIC_ACCELERATED) && (pfd.dwFlags & PFD_GENERIC_FORMAT)) { continue; }

            if (pfd.iPixelType != PFD_TYPE_RGBA) continue;

            if (!!(pfd.dwFlags & PFD_DOUBLEBUFFER) != fbconfig->doublebuffer) continue;

            u->redBits = pfd.cRedBits;
            u->greenBits = pfd.cGreenBits;
            u->blueBits = pfd.cBlueBits;
            u->alphaBits = pfd.cAlphaBits;

            u->depthBits = pfd.cDepthBits;
            u->stencilBits = pfd.cStencilBits;

            u->accumRedBits = pfd.cAccumRedBits;
            u->accumGreenBits = pfd.cAccumGreenBits;
            u->accumBlueBits = pfd.cAccumBlueBits;
            u->accumAlphaBits = pfd.cAccumAlphaBits;

            u->auxBuffers = pfd.cAuxBuffers;

            if (pfd.dwFlags & PFD_STEREO) u->stereo = true;
            u->transparent = fbconfig->transparent && u->alphaBits > 0;
        }

        u->handle = pixelFormat;
        usableCount++;
    }

    if (!usableCount) {
        _glfwInputError(GLFW_API_UNAVAILABLE, "WGL: The driver does not appear to support OpenGL");

        free(usableConfigs);
        return 0;
    }

    closest = _glfwChooseFBConfig(fbconfig, usableConfigs, usableCount);
    if (!closest) {
        _glfwInputError(GLFW_FORMAT_UNAVAILABLE, "WGL: Failed to find a suitable pixel format");

        free(usableConfigs);
        return 0;
    }

    pixelFormat = (int)closest->handle;
    free(usableConfigs);

    return pixelFormat;
}

#undef addAttrib
#undef findAttribValue

static void
makeContextCurrentWGL(_GLFWwindow *window) {
    if (window) {
        if (wglMakeCurrent(window->context.wgl.dc, window->context.wgl.handle)) _glfwPlatformSetTls(&_glfw.contextSlot, window);
        else {
            _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to make context current");
            _glfwPlatformSetTls(&_glfw.contextSlot, NULL);
        }
    } else {
        if (!wglMakeCurrent(NULL, NULL)) { _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to clear current context"); }

        _glfwPlatformSetTls(&_glfw.contextSlot, NULL);
    }
}

static void
swapBuffersWGL(_GLFWwindow *window) {
    if (!window->monitor) {
        // HACK: Use DwmFlush when desktop composition is enabled on Windows 7
        if (!IsWindows8OrGreater()) {
            BOOL enabled = FALSE;

            if (SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled) {
                int count = abs(window->context.wgl.interval);
                while (count--) DwmFlush();
            }
        }
    }

    _glfw.wgl.SwapBuffers(window->context.wgl.dc);
}

static void
swapIntervalWGL(int interval) {
    _GLFWwindow *window = _glfwPlatformGetTls(&_glfw.contextSlot);
    assert(window != NULL);

    window->context.wgl.interval = interval;

    if (!window->monitor) {
        // HACK: Disable WGL swap interval when desktop composition is enabled on
        //       Windows 7 to avoid interfering with DWM vsync
        if (!IsWindows8OrGreater()) {
            BOOL enabled = FALSE;

            if (SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled) interval = 0;
        }
    }

    if (_glfw.wgl.EXT_swap_control) wglSwapIntervalEXT(interval);
}

static int
extensionSupportedWGL(const char *extension) {
    const char *extensions = NULL;

    if (_glfw.wgl.GetExtensionsStringARB) extensions = wglGetExtensionsStringARB(wglGetCurrentDC());
    else if (_glfw.wgl.GetExtensionsStringEXT) extensions = wglGetExtensionsStringEXT();

    if (!extensions) return false;

    return _glfwStringInExtensionString(extension, extensions);
}

static GLFWglproc
getProcAddressWGL(const char *procname) {
    GLFWglproc proc = NULL;
    *(void **)&proc = _glfw_win32_proc_to_ptr(wglGetProcAddress(procname));
    if (proc) return proc;
    *(void **)&proc = _glfw_dlsym(_glfw.wgl.instance, procname);
    return proc;
}

static void
destroyContextWGL(_GLFWwindow *window) {
    if (window->context.wgl.handle) {
        wglDeleteContext(window->context.wgl.handle);
        window->context.wgl.handle = NULL;
    }
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW internal API                      //////
//////////////////////////////////////////////////////////////////////////

static HWND
createProbeWindowWGL(void) {
    return CreateWindowExW(0, _GLFW_WNDCLASSNAME, L"Kitsu OpenGL probe", WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 1, 1, NULL, NULL, _glfw.win32.instance, NULL);
}

static bool
initWGL(void) {
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC prc, rc = NULL;
    HDC pdc, dc;
    HWND helper;
    bool ok = false;

    glfw_dlsym(_glfw.wgl.CreateContext, _glfw.wgl.instance, "wglCreateContext");
    glfw_dlsym(_glfw.wgl.DeleteContext, _glfw.wgl.instance, "wglDeleteContext");
    glfw_dlsym(_glfw.wgl.GetProcAddress, _glfw.wgl.instance, "wglGetProcAddress");
    glfw_dlsym(_glfw.wgl.GetCurrentDC, _glfw.wgl.instance, "wglGetCurrentDC");
    glfw_dlsym(_glfw.wgl.GetCurrentContext, _glfw.wgl.instance, "wglGetCurrentContext");
    glfw_dlsym(_glfw.wgl.MakeCurrent, _glfw.wgl.instance, "wglMakeCurrent");
    glfw_dlsym(_glfw.wgl.ShareLists, _glfw.wgl.instance, "wglShareLists");
    glfw_dlsym(_glfw.wgl.ChoosePixelFormat, _glfw.wgl.instance, "wglChoosePixelFormat");
    glfw_dlsym(_glfw.wgl.DescribePixelFormat, _glfw.wgl.instance, "wglDescribePixelFormat");
    glfw_dlsym(_glfw.wgl.SetPixelFormat, _glfw.wgl.instance, "wglSetPixelFormat");
    glfw_dlsym(_glfw.wgl.SwapBuffers, _glfw.wgl.instance, "wglSwapBuffers");
    if (!_glfw.wgl.CreateContext || !_glfw.wgl.DeleteContext || !_glfw.wgl.GetProcAddress || !_glfw.wgl.GetCurrentDC || !_glfw.wgl.GetCurrentContext ||
        !_glfw.wgl.MakeCurrent || !_glfw.wgl.ShareLists || !_glfw.wgl.ChoosePixelFormat || !_glfw.wgl.DescribePixelFormat || !_glfw.wgl.SetPixelFormat ||
        !_glfw.wgl.SwapBuffers)
        return false;

    // NOTE: A dummy context has to be created for opengl32.dll to load the
    //       OpenGL ICD, from which we can then query WGL extensions
    // NOTE: This code will accept the Microsoft GDI ICD; accelerated context
    //       creation failure occurs during manual pixel format enumeration

    helper = createProbeWindowWGL();
    if (!helper) return false;
    dc = GetDC(helper);
    pdc = wglGetCurrentDC();
    prc = wglGetCurrentContext();

    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;

    if (!_glfw.wgl.SetPixelFormat(dc, _glfw.wgl.ChoosePixelFormat(dc, &pfd), &pfd)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to set pixel format for dummy context");
        goto cleanup;
    }

    rc = wglCreateContext(dc);
    if (!rc) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to create dummy context");
        goto cleanup;
    }

    if (!wglMakeCurrent(dc, rc)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to make dummy context current");
        goto cleanup;
    }

    // NOTE: Functions must be loaded first as they're needed to retrieve the
    //       extension string that tells us whether the functions are supported
    *(void **)&_glfw.wgl.GetExtensionsStringEXT = _glfw_win32_proc_to_ptr(wglGetProcAddress("wglGetExtensionsStringEXT"));
    *(void **)&_glfw.wgl.GetExtensionsStringARB = _glfw_win32_proc_to_ptr(wglGetProcAddress("wglGetExtensionsStringARB"));
    *(void **)&_glfw.wgl.CreateContextAttribsARB = _glfw_win32_proc_to_ptr(wglGetProcAddress("wglCreateContextAttribsARB"));
    *(void **)&_glfw.wgl.SwapIntervalEXT = _glfw_win32_proc_to_ptr(wglGetProcAddress("wglSwapIntervalEXT"));
    *(void **)&_glfw.wgl.GetPixelFormatAttribivARB = _glfw_win32_proc_to_ptr(wglGetProcAddress("wglGetPixelFormatAttribivARB"));

    // NOTE: WGL_ARB_extensions_string and WGL_EXT_extensions_string are not
    //       checked below as we are already using them
    _glfw.wgl.ARB_multisample = extensionSupportedWGL("WGL_ARB_multisample");
    _glfw.wgl.ARB_framebuffer_sRGB = extensionSupportedWGL("WGL_ARB_framebuffer_sRGB");
    _glfw.wgl.EXT_framebuffer_sRGB = extensionSupportedWGL("WGL_EXT_framebuffer_sRGB");
    _glfw.wgl.ARB_create_context = extensionSupportedWGL("WGL_ARB_create_context");
    _glfw.wgl.ARB_create_context_profile = extensionSupportedWGL("WGL_ARB_create_context_profile");
    _glfw.wgl.EXT_create_context_es2_profile = extensionSupportedWGL("WGL_EXT_create_context_es2_profile");
    _glfw.wgl.ARB_create_context_robustness = extensionSupportedWGL("WGL_ARB_create_context_robustness");
    _glfw.wgl.ARB_create_context_no_error = extensionSupportedWGL("WGL_ARB_create_context_no_error");
    _glfw.wgl.EXT_swap_control = extensionSupportedWGL("WGL_EXT_swap_control");
    _glfw.wgl.EXT_colorspace = extensionSupportedWGL("WGL_EXT_colorspace");
    _glfw.wgl.ARB_pixel_format = extensionSupportedWGL("WGL_ARB_pixel_format");
    _glfw.wgl.ARB_context_flush_control = extensionSupportedWGL("WGL_ARB_context_flush_control");

    ok = true;
cleanup:
    wglMakeCurrent(pdc, prc);
    if (rc) wglDeleteContext(rc);
    ReleaseDC(helper, dc);
    DestroyWindow(helper);
    return ok;
}

static bool
probeWGL(const _GLFWctxconfig *ctxconfig, const _GLFWfbconfig *fbconfig) {
    _GLFWwindow probe = {0};
    PFNGLGETSTRINGPROC getString;
    const char *version, *renderer;
    int major = 0, minor = 0;
    bool ok = false;

    // SetPixelFormat is permanent for an HWND. Never probe on the real window.
    probe.win32.handle = createProbeWindowWGL();
    if (!probe.win32.handle) return false;
    if (_glfwCreateContextWGL(&probe, ctxconfig, fbconfig) && wglMakeCurrent(probe.context.wgl.dc, probe.context.wgl.handle)) {
        *(void **)&getString = _glfw_dlsym(_glfw.wgl.instance, "glGetString");
        if (getString) {
            version = (const char *)getString(GL_VERSION);
            renderer = (const char *)getString(GL_RENDERER);
            if (version) sscanf(version, "%d.%d", &major, &minor);
            ok = renderer && strcmp(renderer, "GDI Generic") != 0 && (major > ctxconfig->major || (major == ctxconfig->major && minor >= ctxconfig->minor));
        }
    }
    wglMakeCurrent(NULL, NULL);
    destroyContextWGL(&probe);
    if (probe.context.wgl.dc) ReleaseDC(probe.win32.handle, probe.context.wgl.dc);
    DestroyWindow(probe.win32.handle);
    return ok;
}

bool
_glfwInitWGL(const _GLFWctxconfig *ctxconfig, const _GLFWfbconfig *fbconfig) {
    WCHAR driver[32] = {0}, path[32768], *name;
    DWORD length;
    bool software, available;
    GLFWerrorfun callback;

    if (_glfw.wgl.instance) return true;
    length = GetEnvironmentVariableW(L"KITSU_OPENGL_DRIVER", driver, sizeof(driver) / sizeof(driver[0]));
    if (length >= sizeof(driver) / sizeof(driver[0]) || (length && wcscmp(driver, L"auto") && wcscmp(driver, L"native") && wcscmp(driver, L"llvmpipe"))) {
        _glfwInputError(GLFW_INVALID_VALUE, "KITSU_OPENGL_DRIVER must be auto, native or llvmpipe");
        return false;
    }
    software = wcscmp(driver, L"llvmpipe") == 0;
    if (!software) {
        callback = glfwSetErrorCallback(NULL);
        _glfw.wgl.instance = LoadLibraryExW(L"opengl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        available = _glfw.wgl.instance && initWGL() && probeWGL(ctxconfig, fbconfig);
        glfwSetErrorCallback(callback);
        if (available) return true;
        _glfwTerminateWGL();
        if (wcscmp(driver, L"native") == 0) goto unavailable;
    }

    length = GetModuleFileNameW(NULL, path, sizeof(path) / sizeof(path[0]));
    if (!length || length >= sizeof(path) / sizeof(path[0])) goto unavailable;
    name = wcsrchr(path, L'\\');
    if (!name || (size_t)(name - path) + wcslen(L"\\mesa\\opengl32.dll") >= sizeof(path) / sizeof(path[0])) goto unavailable;
    wcscpy(name, L"\\mesa\\opengl32.dll");
    // Mesa's software path does not require D3D12, DXIL or a GPU driver.
    SetEnvironmentVariableW(L"GALLIUM_DRIVER", L"llvmpipe");
    _putenv("GALLIUM_DRIVER=llvmpipe");
    _glfw.wgl.mesa = true;
    _glfw.wgl.instance = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (_glfw.wgl.instance && initWGL() && probeWGL(ctxconfig, fbconfig)) {
        glfwGetError(NULL);
        return true;
    }
    _glfwTerminateWGL();
unavailable:
    _glfwInputError(
        GLFW_API_UNAVAILABLE,
        "WGL: OpenGL %d.%d is unavailable. Restore bin\\mesa from the Kitsu package, update the graphics driver, "
        "or install Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack. KITSU_OPENGL_DRIVER=llvmpipe forces software rendering.",
        ctxconfig->major,
        ctxconfig->minor);
    return false;
}

// Terminate WGL
//
void
_glfwTerminateWGL(void) {
    if (_glfw.wgl.instance) FreeLibrary(_glfw.wgl.instance);
    memset(&_glfw.wgl, 0, sizeof(_glfw.wgl));
}

#define setAttrib(a, v)                                                     \
    {                                                                       \
        assert(((size_t)index + 1) < sizeof(attribs) / sizeof(attribs[0])); \
        attribs[index++] = a;                                               \
        attribs[index++] = v;                                               \
    }

// Create the OpenGL or OpenGL ES context
//
bool
_glfwCreateContextWGL(_GLFWwindow *window, const _GLFWctxconfig *ctxconfig, const _GLFWfbconfig *fbconfig) {
    int attribs[40];
    int pixelFormat;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC share = NULL;

    if (ctxconfig->share) share = ctxconfig->share->context.wgl.handle;

    window->context.wgl.dc = GetDC(window->win32.handle);
    if (!window->context.wgl.dc) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "WGL: Failed to retrieve DC for window");
        return false;
    }

    pixelFormat = choosePixelFormat(window, ctxconfig, fbconfig);
    if (!pixelFormat) return false;

    if (!_glfw.wgl.DescribePixelFormat(window->context.wgl.dc, pixelFormat, sizeof(pfd), &pfd)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to retrieve PFD for selected pixel format");
        return false;
    }

    if (!_glfw.wgl.SetPixelFormat(window->context.wgl.dc, pixelFormat, &pfd)) {
        _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to set selected pixel format");
        return false;
    }

    if (ctxconfig->client == GLFW_OPENGL_API) {
        if (ctxconfig->forward) {
            if (!_glfw.wgl.ARB_create_context) {
                _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: A forward compatible OpenGL context requested but WGL_ARB_create_context is unavailable");
                return false;
            }
        }

        if (ctxconfig->profile) {
            if (!_glfw.wgl.ARB_create_context_profile) {
                _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: OpenGL profile requested but WGL_ARB_create_context_profile is unavailable");
                return false;
            }
        }
    } else {
        if (!_glfw.wgl.ARB_create_context || !_glfw.wgl.ARB_create_context_profile || !_glfw.wgl.EXT_create_context_es2_profile) {
            _glfwInputError(GLFW_API_UNAVAILABLE, "WGL: OpenGL ES requested but WGL_ARB_create_context_es2_profile is unavailable");
            return false;
        }
    }

    if (_glfw.wgl.ARB_create_context) {
        int index = 0, mask = 0, flags = 0;

        if (ctxconfig->client == GLFW_OPENGL_API) {
            if (ctxconfig->forward) flags |= WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;

            if (ctxconfig->profile == GLFW_OPENGL_CORE_PROFILE) mask |= WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
            else if (ctxconfig->profile == GLFW_OPENGL_COMPAT_PROFILE) mask |= WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        } else mask |= WGL_CONTEXT_ES2_PROFILE_BIT_EXT;

        if (ctxconfig->debug) flags |= WGL_CONTEXT_DEBUG_BIT_ARB;

        if (ctxconfig->robustness) {
            if (_glfw.wgl.ARB_create_context_robustness) {
                if (ctxconfig->robustness == GLFW_NO_RESET_NOTIFICATION) {
                    setAttrib(WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB, WGL_NO_RESET_NOTIFICATION_ARB);
                } else if (ctxconfig->robustness == GLFW_LOSE_CONTEXT_ON_RESET) {
                    setAttrib(WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB, WGL_LOSE_CONTEXT_ON_RESET_ARB);
                }

                flags |= WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB;
            }
        }

        if (ctxconfig->release) {
            if (_glfw.wgl.ARB_context_flush_control) {
                if (ctxconfig->release == GLFW_RELEASE_BEHAVIOR_NONE) {
                    setAttrib(WGL_CONTEXT_RELEASE_BEHAVIOR_ARB, WGL_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB);
                } else if (ctxconfig->release == GLFW_RELEASE_BEHAVIOR_FLUSH) {
                    setAttrib(WGL_CONTEXT_RELEASE_BEHAVIOR_ARB, WGL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB);
                }
            }
        }

        if (ctxconfig->noerror) {
            if (_glfw.wgl.ARB_create_context_no_error) setAttrib(WGL_CONTEXT_OPENGL_NO_ERROR_ARB, true);
        }

        // NOTE: Only request an explicitly versioned context when necessary, as
        //       explicitly requesting version 1.0 does not always return the
        //       highest version supported by the driver
        if (ctxconfig->major != 1 || ctxconfig->minor != 0) {
            setAttrib(WGL_CONTEXT_MAJOR_VERSION_ARB, ctxconfig->major);
            setAttrib(WGL_CONTEXT_MINOR_VERSION_ARB, ctxconfig->minor);
        }

        if (flags) setAttrib(WGL_CONTEXT_FLAGS_ARB, flags);

        if (mask) setAttrib(WGL_CONTEXT_PROFILE_MASK_ARB, mask);

        setAttrib(0, 0);

        window->context.wgl.handle = wglCreateContextAttribsARB(window->context.wgl.dc, share, attribs);
        if (!window->context.wgl.handle) {
            const DWORD error = GetLastError();

            if (error == (0xc0070000 | ERROR_INVALID_VERSION_ARB)) {
                if (ctxconfig->client == GLFW_OPENGL_API) {
                    _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: Driver does not support OpenGL version %i.%i", ctxconfig->major, ctxconfig->minor);
                } else {
                    _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: Driver does not support OpenGL ES version %i.%i", ctxconfig->major, ctxconfig->minor);
                }
            } else if (error == (0xc0070000 | ERROR_INVALID_PROFILE_ARB)) {
                _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: Driver does not support the requested OpenGL profile");
            } else if (error == (0xc0070000 | ERROR_INCOMPATIBLE_DEVICE_CONTEXTS_ARB)) {
                _glfwInputError(GLFW_INVALID_VALUE, "WGL: The share context is not compatible with the requested context");
            } else {
                if (ctxconfig->client == GLFW_OPENGL_API) {
                    _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: Failed to create OpenGL context");
                } else {
                    _glfwInputError(GLFW_VERSION_UNAVAILABLE, "WGL: Failed to create OpenGL ES context");
                }
            }

            return false;
        }
    } else {
        window->context.wgl.handle = wglCreateContext(window->context.wgl.dc);
        if (!window->context.wgl.handle) {
            _glfwInputErrorWin32(GLFW_VERSION_UNAVAILABLE, "WGL: Failed to create OpenGL context");
            return false;
        }

        if (share) {
            if (!wglShareLists(share, window->context.wgl.handle)) {
                _glfwInputErrorWin32(GLFW_PLATFORM_ERROR, "WGL: Failed to enable sharing with specified OpenGL context");
                return false;
            }
        }
    }

    window->context.makeCurrent = makeContextCurrentWGL;
    window->context.swapBuffers = swapBuffersWGL;
    window->context.swapInterval = swapIntervalWGL;
    window->context.extensionSupported = extensionSupportedWGL;
    window->context.getProcAddress = getProcAddressWGL;
    window->context.destroy = destroyContextWGL;

    return true;
}

#undef setAttrib


//////////////////////////////////////////////////////////////////////////
//////                        GLFW native API                       //////
//////////////////////////////////////////////////////////////////////////

GLFWAPI HGLRC
glfwGetWGLContext(GLFWwindow *handle) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    _GLFW_REQUIRE_INIT_OR_RETURN(NULL);

    if (window->context.source != GLFW_NATIVE_CONTEXT_API) {
        _glfwInputError(GLFW_NO_WINDOW_CONTEXT, NULL);
        return NULL;
    }

    return window->context.wgl.handle;
}
