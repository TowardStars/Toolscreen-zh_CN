#include "shared_contexts.h"
#include "utils.h"

#include <GL/glew.h>
#include <GL/wglew.h>

// Pre-created shared contexts
std::atomic<HGLRC> g_sharedRenderContext{ nullptr };
std::atomic<HGLRC> g_sharedMirrorContext{ nullptr };
std::atomic<HDC> g_sharedContextDC{ nullptr };
std::atomic<bool> g_sharedContextsReady{ false };

// Use a dedicated hidden dummy window/DC for worker contexts.
// Using the game's window HDC across multiple threads/contexts can trigger flicker/black frames on some drivers.
static HWND g_sharedDummyHwnd = NULL;
static HDC g_sharedDummyDC = NULL;

static bool CreateSharedDummyWindowWithMatchingPixelFormat(HDC gameHdc) {
    if (g_sharedDummyHwnd && g_sharedDummyDC) { return true; }
    if (!gameHdc) { return false; }

    // Get the game's pixel format.
    int gamePf = GetPixelFormat(gameHdc);
    if (gamePf == 0) {
        Log("SharedContexts: GetPixelFormat(gameHdc) returned 0");
        return false;
    }

    PIXELFORMATDESCRIPTOR gamePfd = {};
    gamePfd.nSize = sizeof(gamePfd);
    gamePfd.nVersion = 1;
    if (DescribePixelFormat(gameHdc, gamePf, sizeof(gamePfd), &gamePfd) == 0) {
        Log("SharedContexts: DescribePixelFormat(gameHdc) failed");
        return false;
    }

    // Register a tiny hidden window class (once).
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ToolscreenSharedGLDummy";
        s_atom = RegisterClassExW(&wc);
        if (!s_atom) {
            // Class might already exist (e.g. DLL reinjection) - try to continue.
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                Log("SharedContexts: RegisterClassExW failed (error " + std::to_string(err) + ")");
                return false;
            }
        }
    }

    g_sharedDummyHwnd = CreateWindowExW(0, L"ToolscreenSharedGLDummy", L"", WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL,
                                        GetModuleHandleW(NULL), NULL);
    if (!g_sharedDummyHwnd) {
        Log("SharedContexts: CreateWindowExW(dummy) failed (error " + std::to_string(GetLastError()) + ")");
        return false;
    }

    g_sharedDummyDC = GetDC(g_sharedDummyHwnd);
    if (!g_sharedDummyDC) {
        Log("SharedContexts: GetDC(dummy) failed");
        DestroyWindow(g_sharedDummyHwnd);
        g_sharedDummyHwnd = NULL;
        return false;
    }

    // Try to set the exact same pixel format index.
    if (!SetPixelFormat(g_sharedDummyDC, gamePf, &gamePfd)) {
        // Fallback: choose closest pixel format based on the game's PFD.
        int chosen = ChoosePixelFormat(g_sharedDummyDC, &gamePfd);
        if (chosen == 0 || !SetPixelFormat(g_sharedDummyDC, chosen, &gamePfd)) {
            Log("SharedContexts: Failed to SetPixelFormat(dummy) (error " + std::to_string(GetLastError()) + ")");
            ReleaseDC(g_sharedDummyHwnd, g_sharedDummyDC);
            DestroyWindow(g_sharedDummyHwnd);
            g_sharedDummyDC = NULL;
            g_sharedDummyHwnd = NULL;
            return false;
        }
        Log("SharedContexts: Dummy DC pixel format differs from game (fallback chosen=" + std::to_string(chosen) + ")");
    }

    return true;
}

// Helper: temporarily unbind the current context so wglShareLists can succeed.
// Per WGL docs, neither context may be current when calling wglShareLists.
struct ScopedWglUnbind {
    HDC prevDC = NULL;
    HGLRC prevRC = NULL;
    bool unbound = false;
    ScopedWglUnbind() {
        prevRC = wglGetCurrentContext();
        prevDC = wglGetCurrentDC();
        if (prevRC) {
            // Unbind only if there is a current context.
            if (wglMakeCurrent(NULL, NULL)) { unbound = true; }
        }
    }
    ~ScopedWglUnbind() {
        if (unbound && prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }
    }
};

bool InitializeSharedContexts(void* gameGLContext, HDC hdc) {
    if (g_sharedContextsReady.load()) {
        return true; // Already initialized
    }

    if (!gameGLContext || !hdc) {
        Log("SharedContexts: Invalid game context or DC");
        return false;
    }

    HGLRC gameContext = (HGLRC)gameGLContext;

    Log("SharedContexts: Initializing all shared contexts...");

    // Prefer a dedicated dummy window/DC for worker contexts.
    // If this fails, fall back to using the game's DC (legacy behavior).
    HDC contextHdc = hdc;
    if (CreateSharedDummyWindowWithMatchingPixelFormat(hdc) && g_sharedDummyDC) {
        contextHdc = g_sharedDummyDC;
        Log("SharedContexts: Using dummy DC for worker contexts");
    } else {
        Log("SharedContexts: Using game DC for worker contexts (dummy DC unavailable)");
    }

    // Store the DC for later use by worker threads
    g_sharedContextDC.store(contextHdc);

    // Preferred: Create contexts with WGL_ARB_create_context and share-at-create.
    // This is more reliable with modern Minecraft (1.21+) contexts (core profiles, newer versions).
    HGLRC renderContext = NULL;
    HGLRC mirrorContext = NULL;

    // Query current game context version/profile (best effort; defaults to 3.3 compat).
    GLint major = 3, minor = 3;
    GLint profileMask = 0;
    GLint flags = 0;
    // These queries require the game context to be current (it is, since we're in SwapBuffers).
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    // GL_CONTEXT_PROFILE_MASK exists in 3.2+; if not present, it will return 0.
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    while (glGetError() != GL_NO_ERROR) {
        // Swallow any errors from older contexts that don't support these enums.
    }

    if (wglCreateContextAttribsARB) {
        int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, major, WGL_CONTEXT_MINOR_VERSION_ARB, minor, WGL_CONTEXT_FLAGS_ARB, flags,
                          // If profileMask is 0 (unknown), request compatibility (most permissive).
                          WGL_CONTEXT_PROFILE_MASK_ARB, (profileMask != 0) ? profileMask : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0 };

        renderContext = wglCreateContextAttribsARB(contextHdc, gameContext, attribs);
        mirrorContext = wglCreateContextAttribsARB(contextHdc, gameContext, attribs);
        if (renderContext && mirrorContext) {
            Log("SharedContexts: Created shared contexts via wglCreateContextAttribsARB (" + std::to_string(major) + "." +
                std::to_string(minor) + ")");
        } else {
            DWORD err = GetLastError();
            Log("SharedContexts: wglCreateContextAttribsARB failed (error " + std::to_string(err) +
                "), falling back to wglCreateContext + wglShareLists");
            if (renderContext) {
                wglDeleteContext(renderContext);
                renderContext = NULL;
            }
            if (mirrorContext) {
                wglDeleteContext(mirrorContext);
                mirrorContext = NULL;
            }
        }
    }

    if (!renderContext || !mirrorContext) {
        // Fallback: Create legacy contexts then share.
        renderContext = wglCreateContext(contextHdc);
        if (!renderContext) {
            Log("SharedContexts: Failed to create render context (error " + std::to_string(GetLastError()) + ")");
            return false;
        }

        mirrorContext = wglCreateContext(contextHdc);
        if (!mirrorContext) {
            Log("SharedContexts: Failed to create mirror context (error " + std::to_string(GetLastError()) + ")");
            wglDeleteContext(renderContext);
            return false;
        }

        Log("SharedContexts: Created 2 contexts (legacy), now sharing with game...");

        // Now share all contexts with the game context.
        // IMPORTANT: wglShareLists requires neither context to be current.
        ScopedWglUnbind unbind;

        // Share render context with game
        SetLastError(0);
        if (!wglShareLists(gameContext, renderContext)) {
            DWORD err = GetLastError();
            // Try reverse order
            if (!wglShareLists(renderContext, gameContext)) {
                Log("SharedContexts: Failed to share render context (error " + std::to_string(err) + ", " + std::to_string(GetLastError()) +
                    ")");
                wglDeleteContext(renderContext);
                wglDeleteContext(mirrorContext);
                return false;
            }
        }
        Log("SharedContexts: Render context shared with game");

        // Share mirror context with game
        SetLastError(0);
        if (!wglShareLists(gameContext, mirrorContext)) {
            DWORD err = GetLastError();
            if (!wglShareLists(mirrorContext, gameContext)) {
                Log("SharedContexts: Failed to share mirror context (error " + std::to_string(err) + ", " + std::to_string(GetLastError()) +
                    ")");
                wglDeleteContext(renderContext);
                wglDeleteContext(mirrorContext);
                return false;
            }
        }
        Log("SharedContexts: Mirror context shared with game");
    }

    // All contexts created and shared successfully!
    g_sharedRenderContext.store(renderContext);
    g_sharedMirrorContext.store(mirrorContext);
    g_sharedContextsReady.store(true);

    Log("SharedContexts: All contexts initialized and shared successfully");
    return true;
}

void CleanupSharedContexts() {
    g_sharedContextsReady.store(false);

    HGLRC render = g_sharedRenderContext.exchange(nullptr);
    HGLRC mirror = g_sharedMirrorContext.exchange(nullptr);

    // Only delete if not already deleted by their respective threads
    // Note: Threads should set these to nullptr when they clean up
    if (render) { wglDeleteContext(render); }
    if (mirror) { wglDeleteContext(mirror); }

    g_sharedContextDC.store(nullptr);

    // Cleanup dummy window/DC
    if (g_sharedDummyHwnd && g_sharedDummyDC) {
        ReleaseDC(g_sharedDummyHwnd, g_sharedDummyDC);
        g_sharedDummyDC = NULL;
    }
    if (g_sharedDummyHwnd) {
        DestroyWindow(g_sharedDummyHwnd);
        g_sharedDummyHwnd = NULL;
    }
    Log("SharedContexts: Cleaned up");
}

HGLRC GetSharedRenderContext() { return g_sharedRenderContext.load(); }

HGLRC GetSharedMirrorContext() { return g_sharedMirrorContext.load(); }

HDC GetSharedContextDC() { return g_sharedContextDC.load(); }

bool AreSharedContextsReady() { return g_sharedContextsReady.load(); }
