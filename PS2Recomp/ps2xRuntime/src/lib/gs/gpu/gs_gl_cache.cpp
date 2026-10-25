// GOW-Port: GL state cache for the GS renderer (InstallStateCache, gs_gl.h). Each GS batch sets 20-30 pieces of GL state
// (bindings, texture filters, blend, masks, viewport, scissor), most of them to what is already set. Desktop drivers
// filter those cheaply; on iOS the renderer's GL thread spent ~2/3 of its time in Apple's GL driver submitting batches
// while the GPU kept up (on-screen readout, 2026-10-25). The wrappers keep a per-thread shadow of that state (one GL
// context per thread) and skip a call that would set what is already set. The shadow is always updated, also while
// skipping is off (SetStateCacheEnabled), so switching it on at any time is safe. Deleting an object forgets it.
//
// PS2X_GS_STATE_CACHE_VERIFY=1 (PC, tests): before every skipped call the driver's real state is read back and compared
// with the shadow. No mismatch means every skipped call was a no-op, i.e. the picture is the one without the cache.
#include "gs_gl.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace GSGL
{
    namespace
    {
        Api g_real; // the driver's entry points
        std::atomic<bool> g_enabled{false};

        constexpr GLenum kReadFb = 0x8CA8, kDrawFb = 0x8CA9, kArrayBuffer = 0x8892, kTexture0 = 0x84C0;
        constexpr GLenum kWrapS = 0x2802, kWrapT = 0x2803;
        constexpr GLenum kPackAlignment = 0x0D05, kUnpackAlignment = 0x0CF5;
        constexpr int kUnits = 16;

        // capabilities tracked by Enable/Disable: blend, depth test, scissor test, cull face, stencil test
        constexpr GLenum kCaps[] = {0x0BE2, 0x0B71, 0x0C11, 0x0B44, 0x0B90};
        int capIndex(GLenum cap)
        {
            for (int i = 0; i < 5; ++i)
                if (kCaps[i] == cap)
                    return i;
            return -1;
        }

        template <typename T, size_t N>
        struct Known
        {
            std::array<T, N> value{};
            bool known = false;
            // true when the call sets what is already set; records the value either way
            bool same(const std::array<T, N> &next)
            {
                const bool was = known && value == next;
                value = next;
                known = true;
                return was;
            }
        };

        struct State
        {
            Known<GLuint, 1> program, vao, arrayBuffer, readFb, drawFb;
            Known<GLenum, 1> activeUnit;
            std::array<Known<GLuint, 1>, kUnits> tex2d;
            std::unordered_map<GLuint, std::array<GLint, 4>> texParams; // min, mag, wrap s, wrap t (INT32_MIN unknown)
            std::array<Known<GLboolean, 1>, 5> caps;
            Known<GLenum, 4> blendFunc;
            Known<GLenum, 2> blendEquation;
            Known<float, 4> blendColor;
            Known<GLboolean, 4> colorMask;
            Known<GLboolean, 1> depthMask;
            Known<GLenum, 1> depthFunc;
            Known<GLint, 4> viewport, scissor;
            Known<GLint, 1> packAlignment, unpackAlignment;
        };
        thread_local State t_state;

        struct Verify
        {
            void (*getIntegerv)(GLenum, GLint *) = nullptr;
            void (*getFloatv)(GLenum, float *) = nullptr;
            void (*getBooleanv)(GLenum, GLboolean *) = nullptr;
            GLboolean (*isEnabled)(GLenum) = nullptr;
            void (*getTexParameteriv)(GLenum, GLenum, GLint *) = nullptr;
            bool on = false;
            std::atomic<uint64_t> checks{0}, mismatches{0};
        } g_verify;

        void mismatch(const char *what, double shadow, double real)
        {
            if (g_verify.mismatches.fetch_add(1) < 20)
                std::fprintf(stderr, "[gl-cache-verify] MISMATCH %s shadow=%g real=%g\n", what, shadow, real);
        }
        void verifyInts(const char *what, GLenum pname, const GLint *shadow, int n)
        {
            GLint real[4] = {};
            g_verify.getIntegerv(pname, real);
            ++g_verify.checks;
            for (int i = 0; i < n; ++i)
                if (real[i] != shadow[i])
                    mismatch(what, shadow[i], real[i]);
        }
        void verifyInt(const char *what, GLenum pname, GLint shadow) { verifyInts(what, pname, &shadow, 1); }

        // the call may be skipped (the shadow says it sets what is set); checks the claim first in verify mode
        bool skip(bool same)
        {
            return same && g_enabled.load(std::memory_order_relaxed);
        }

        void useProgram(GLuint p)
        {
            if (!skip(t_state.program.same({p})))
                return g_real.UseProgram(p);
            if (g_verify.on)
                verifyInt("program", 0x8B8D, static_cast<GLint>(p));
        }
        void bindVertexArray(GLuint v)
        {
            if (!skip(t_state.vao.same({v})))
                return g_real.BindVertexArray(v);
            if (g_verify.on)
                verifyInt("vertex array", 0x85B5, static_cast<GLint>(v));
        }
        void bindBuffer(GLenum target, GLuint b)
        {
            if (target != kArrayBuffer)
                return g_real.BindBuffer(target, b);
            if (!skip(t_state.arrayBuffer.same({b})))
                return g_real.BindBuffer(target, b);
            if (g_verify.on)
                verifyInt("array buffer", 0x8894, static_cast<GLint>(b));
        }
        void bindFramebuffer(GLenum target, GLuint fb)
        {
            bool same = true;
            if (target == kReadFb || target == kFramebuffer)
                same = t_state.readFb.same({fb}) && same;
            if (target == kDrawFb || target == kFramebuffer)
                same = t_state.drawFb.same({fb}) && same;
            if (!skip(same))
                return g_real.BindFramebuffer(target, fb);
            if (g_verify.on)
            {
                if (target != kDrawFb)
                    verifyInt("read framebuffer", 0x8CAA, static_cast<GLint>(fb));
                if (target != kReadFb)
                    verifyInt("draw framebuffer", 0x8CA6, static_cast<GLint>(fb));
            }
        }
        void activeTexture(GLenum unit)
        {
            if (!skip(t_state.activeUnit.same({unit})))
                return g_real.ActiveTexture(unit);
            if (g_verify.on)
                verifyInt("active texture", 0x84E0, static_cast<GLint>(unit));
        }
        int currentUnit()
        {
            if (!t_state.activeUnit.known)
                return -1;
            const int unit = static_cast<int>(t_state.activeUnit.value[0]) - static_cast<int>(kTexture0);
            return unit >= 0 && unit < kUnits ? unit : -1;
        }
        void bindTexture(GLenum target, GLuint t)
        {
            const int unit = currentUnit();
            if (target != kTexture2D || unit < 0)
            {
                if (target == kTexture2D)
                    for (auto &bound : t_state.tex2d)
                        bound.known = false; // a unit we do not track: forget all
                return g_real.BindTexture(target, t);
            }
            if (!skip(t_state.tex2d[unit].same({t})))
                return g_real.BindTexture(target, t);
            if (g_verify.on)
                verifyInt("texture binding", 0x8069, static_cast<GLint>(t));
        }
        void texParameteri(GLenum target, GLenum pname, GLint value)
        {
            const int unit = currentUnit();
            const int slot = pname == kTextureMinFilter ? 0 : pname == kTextureMagFilter ? 1 : pname == kWrapS ? 2 : pname == kWrapT ? 3 : -1;
            if (target != kTexture2D || unit < 0 || slot < 0 || !t_state.tex2d[unit].known || t_state.tex2d[unit].value[0] == 0)
                return g_real.TexParameteri(target, pname, value);
            const GLuint texture = t_state.tex2d[unit].value[0];
            auto it = t_state.texParams.find(texture);
            if (it == t_state.texParams.end())
                it = t_state.texParams.emplace(texture, std::array<GLint, 4>{INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN}).first;
            const bool same = it->second[slot] == value;
            it->second[slot] = value;
            if (!skip(same))
                return g_real.TexParameteri(target, pname, value);
            if (g_verify.on && g_verify.getTexParameteriv)
            {
                GLint real = 0;
                g_verify.getTexParameteriv(target, pname, &real);
                ++g_verify.checks;
                if (real != value)
                    mismatch("texture parameter", value, real);
            }
        }
        void setCap(GLenum cap, bool on)
        {
            const int i = capIndex(cap);
            if (i < 0 || !skip(t_state.caps[i].same({static_cast<GLboolean>(on ? 1 : 0)})))
                return on ? g_real.Enable(cap) : g_real.Disable(cap);
            if (g_verify.on && g_verify.isEnabled)
            {
                ++g_verify.checks;
                const bool real = g_verify.isEnabled(cap) != 0;
                if (real != on)
                    mismatch("capability", on, real);
            }
        }
        void enable(GLenum cap) { setCap(cap, true); }
        void disable(GLenum cap) { setCap(cap, false); }
        void blendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
        {
            if (!skip(t_state.blendFunc.same({a, b, c, d})))
                return g_real.BlendFuncSeparate(a, b, c, d);
            if (g_verify.on)
            {
                verifyInt("blend src rgb", 0x80C9, static_cast<GLint>(a));
                verifyInt("blend dst rgb", 0x80C8, static_cast<GLint>(b));
                verifyInt("blend src alpha", 0x80CB, static_cast<GLint>(c));
                verifyInt("blend dst alpha", 0x80CA, static_cast<GLint>(d));
            }
        }
        void blendEquationSeparate(GLenum a, GLenum b)
        {
            if (!skip(t_state.blendEquation.same({a, b})))
                return g_real.BlendEquationSeparate(a, b);
            if (g_verify.on)
            {
                verifyInt("blend equation rgb", 0x8009, static_cast<GLint>(a));
                verifyInt("blend equation alpha", 0x883D, static_cast<GLint>(b));
            }
        }
        void blendColor(float r, float g, float b, float a)
        {
            if (!skip(t_state.blendColor.same({r, g, b, a})))
                return g_real.BlendColor(r, g, b, a);
            if (g_verify.on && g_verify.getFloatv)
            {
                float real[4] = {};
                g_verify.getFloatv(0x8005, real);
                ++g_verify.checks;
                const float shadow[4] = {r, g, b, a};
                for (int i = 0; i < 4; ++i)
                    if (real[i] != shadow[i])
                        mismatch("blend color", shadow[i], real[i]);
            }
        }
        void colorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
        {
            if (!skip(t_state.colorMask.same({r, g, b, a})))
                return g_real.ColorMask(r, g, b, a);
            if (g_verify.on && g_verify.getBooleanv)
            {
                GLboolean real[4] = {};
                g_verify.getBooleanv(0x0C23, real);
                ++g_verify.checks;
                const GLboolean shadow[4] = {r, g, b, a};
                for (int i = 0; i < 4; ++i)
                    if ((real[i] != 0) != (shadow[i] != 0))
                        mismatch("color mask", shadow[i], real[i]);
            }
        }
        void depthMask(GLboolean m)
        {
            if (!skip(t_state.depthMask.same({m})))
                return g_real.DepthMask(m);
            if (g_verify.on && g_verify.getBooleanv)
            {
                GLboolean real = 0;
                g_verify.getBooleanv(0x0B72, &real);
                ++g_verify.checks;
                if ((real != 0) != (m != 0))
                    mismatch("depth mask", m, real);
            }
        }
        void depthFunc(GLenum f)
        {
            if (!skip(t_state.depthFunc.same({f})))
                return g_real.DepthFunc(f);
            if (g_verify.on)
                verifyInt("depth func", 0x0B74, static_cast<GLint>(f));
        }
        void viewport(GLint x, GLint y, GLsizei w, GLsizei h)
        {
            if (!skip(t_state.viewport.same({x, y, w, h})))
                return g_real.Viewport(x, y, w, h);
            if (g_verify.on)
            {
                const GLint shadow[4] = {x, y, w, h};
                verifyInts("viewport", 0x0BA2, shadow, 4);
            }
        }
        void scissor(GLint x, GLint y, GLsizei w, GLsizei h)
        {
            if (!skip(t_state.scissor.same({x, y, w, h})))
                return g_real.Scissor(x, y, w, h);
            if (g_verify.on)
            {
                const GLint shadow[4] = {x, y, w, h};
                verifyInts("scissor", 0x0C10, shadow, 4);
            }
        }
        void pixelStorei(GLenum pname, GLint value)
        {
            Known<GLint, 1> *known = pname == kPackAlignment ? &t_state.packAlignment : pname == kUnpackAlignment ? &t_state.unpackAlignment : nullptr;
            if (!known || !skip(known->same({value})))
                return g_real.PixelStorei(pname, value);
            if (g_verify.on)
                verifyInt("pixel store", pname, value);
        }

        // deleting a bound object resets that binding to 0 (programs stay current until unbound)
        void deleteTextures(GLsizei n, const GLuint *names)
        {
            for (GLsizei i = 0; i < n; ++i)
            {
                t_state.texParams.erase(names[i]);
                for (auto &bound : t_state.tex2d)
                    if (bound.known && bound.value[0] == names[i])
                        bound.value[0] = 0;
            }
            g_real.DeleteTextures(n, names);
        }
        void deleteFramebuffers(GLsizei n, const GLuint *names)
        {
            for (GLsizei i = 0; i < n; ++i)
            {
                if (t_state.readFb.known && t_state.readFb.value[0] == names[i])
                    t_state.readFb.value[0] = 0;
                if (t_state.drawFb.known && t_state.drawFb.value[0] == names[i])
                    t_state.drawFb.value[0] = 0;
            }
            g_real.DeleteFramebuffers(n, names);
        }
        void deleteVertexArrays(GLsizei n, const GLuint *names)
        {
            for (GLsizei i = 0; i < n; ++i)
                if (t_state.vao.known && t_state.vao.value[0] == names[i])
                    t_state.vao.value[0] = 0;
            g_real.DeleteVertexArrays(n, names);
        }
        void deleteBuffers(GLsizei n, const GLuint *names)
        {
            for (GLsizei i = 0; i < n; ++i)
                if (t_state.arrayBuffer.known && t_state.arrayBuffer.value[0] == names[i])
                    t_state.arrayBuffer.value[0] = 0;
            g_real.DeleteBuffers(n, names);
        }

        void setUpVerify()
        {
            const char *setting = std::getenv("PS2X_GS_STATE_CACHE_VERIFY");
            if (!(setting && *setting == '1'))
                return;
#if defined(_WIN32)
            HMODULE gl = GetModuleHandleA("opengl32.dll");
            if (!gl)
                return;
            g_verify.getIntegerv = reinterpret_cast<decltype(g_verify.getIntegerv)>(GetProcAddress(gl, "glGetIntegerv"));
            g_verify.getFloatv = reinterpret_cast<decltype(g_verify.getFloatv)>(GetProcAddress(gl, "glGetFloatv"));
            g_verify.getBooleanv = reinterpret_cast<decltype(g_verify.getBooleanv)>(GetProcAddress(gl, "glGetBooleanv"));
            g_verify.isEnabled = reinterpret_cast<decltype(g_verify.isEnabled)>(GetProcAddress(gl, "glIsEnabled"));
            g_verify.getTexParameteriv = reinterpret_cast<decltype(g_verify.getTexParameteriv)>(GetProcAddress(gl, "glGetTexParameteriv"));
            g_verify.on = g_verify.getIntegerv != nullptr;
#endif
            std::fprintf(stderr, "[gl-cache-verify] %s\n", g_verify.on ? "checking every skipped call against the driver" : "unavailable here");
        }
    }

    void InstallStateCache(Api &api)
    {
        g_real = api;
        setUpVerify();
        const auto wrap = [](auto &slot, auto wrapper) {
            if (slot)
                slot = wrapper;
        };
        wrap(api.UseProgram, &useProgram);
        wrap(api.BindVertexArray, &bindVertexArray);
        wrap(api.BindBuffer, &bindBuffer);
        wrap(api.BindFramebuffer, &bindFramebuffer);
        wrap(api.ActiveTexture, &activeTexture);
        wrap(api.BindTexture, &bindTexture);
        wrap(api.TexParameteri, &texParameteri);
        wrap(api.Enable, &enable);
        wrap(api.Disable, &disable);
        wrap(api.BlendFuncSeparate, &blendFuncSeparate);
        wrap(api.BlendEquationSeparate, &blendEquationSeparate);
        wrap(api.BlendColor, &blendColor);
        wrap(api.ColorMask, &colorMask);
        wrap(api.DepthMask, &depthMask);
        wrap(api.DepthFunc, &depthFunc);
        wrap(api.Viewport, &viewport);
        wrap(api.Scissor, &scissor);
        wrap(api.PixelStorei, &pixelStorei);
        wrap(api.DeleteTextures, &deleteTextures);
        wrap(api.DeleteFramebuffers, &deleteFramebuffers);
        wrap(api.DeleteVertexArrays, &deleteVertexArrays);
        wrap(api.DeleteBuffers, &deleteBuffers);
    }

    void SetStateCacheEnabled(bool enabled)
    {
        g_enabled.store(enabled, std::memory_order_relaxed);
    }

    void ReportStateCacheVerify()
    {
        if (g_verify.on)
            std::fprintf(stderr, "[gl-cache-verify] checks=%llu mismatches=%llu\n", static_cast<unsigned long long>(g_verify.checks.load()),
                         static_cast<unsigned long long>(g_verify.mismatches.load()));
    }
}
