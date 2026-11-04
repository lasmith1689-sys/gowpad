// GOW-Port: save-state serialisation helpers (ps2_savestate.h). A state is a sequence of tagged sections; each subsystem
// writes and reads its own. Plain data only: trivially copyable values, byte blocks (written page-sparse: all-zero 4 KB
// pages cost one bit), strings and vectors of plain values. Reading checks sizes and tags; any mismatch makes the
// reader fail (ok() false) and the load is refused before anything is changed.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ps2_state
{
    class Writer
    {
    public:
        void raw(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            m_data.insert(m_data.end(), p, p + size);
        }
        template <typename T>
        void pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "plain data only");
            raw(&value, sizeof(T));
        }
        void u32(uint32_t v) { pod(v); }
        void u64(uint64_t v) { pod(v); }
        void boolean(bool v) { pod(static_cast<uint8_t>(v ? 1 : 0)); }
        void string(const std::string &s)
        {
            u32(static_cast<uint32_t>(s.size()));
            raw(s.data(), s.size());
        }
        template <typename T>
        void vector(const std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "plain data only");
            u64(v.size());
            if (!v.empty())
                raw(v.data(), v.size() * sizeof(T));
        }
        // pairs of plain values (a map's entries)
        template <typename A, typename B>
        void pairs(const std::vector<std::pair<A, B>> &v)
        {
            static_assert(std::is_trivially_copyable_v<A> && std::is_trivially_copyable_v<B>, "plain data only");
            u64(v.size());
            for (const auto &kv : v)
                pod(kv.first), pod(kv.second);
        }
        // a memory block of a known size, all-zero 4 KB pages skipped
        void block(const void *data, size_t size)
        {
            constexpr size_t kPage = 4096;
            const auto *p = static_cast<const uint8_t *>(data);
            const size_t pages = (size + kPage - 1) / kPage;
            u64(size);
            std::vector<uint8_t> used((pages + 7) / 8, 0);
            for (size_t i = 0; i < pages; ++i)
            {
                const size_t n = std::min(kPage, size - i * kPage);
                bool zero = true;
                for (size_t j = 0; j < n && zero; ++j)
                    zero = p[i * kPage + j] == 0;
                if (!zero)
                    used[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            }
            raw(used.data(), used.size());
            for (size_t i = 0; i < pages; ++i)
                if (used[i / 8] & (1u << (i % 8)))
                    raw(p + i * kPage, std::min(kPage, size - i * kPage));
        }
        // sections: begin("EECP") ... end(); the size is patched in at end()
        void begin(const char (&tag)[5])
        {
            raw(tag, 4);
            m_open.push_back(m_data.size());
            u64(0);
        }
        void end()
        {
            const size_t at = m_open.back();
            m_open.pop_back();
            const uint64_t size = m_data.size() - at - sizeof(uint64_t);
            std::memcpy(m_data.data() + at, &size, sizeof(size));
        }
        const std::vector<uint8_t> &data() const { return m_data; }
        std::vector<uint8_t> &data() { return m_data; }

    private:
        std::vector<uint8_t> m_data;
        std::vector<size_t> m_open;
    };

    class Reader
    {
    public:
        Reader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}
        bool ok() const { return m_ok; }
        void fail(const char *why)
        {
            if (m_ok)
                m_error = why;
            m_ok = false;
        }
        const std::string &error() const { return m_error; }
        bool raw(void *out, size_t size)
        {
            if (!m_ok || m_at + size > m_size)
            {
                fail("truncated");
                return false;
            }
            std::memcpy(out, m_data + m_at, size);
            m_at += size;
            return true;
        }
        template <typename T>
        bool pod(T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "plain data only");
            return raw(&value, sizeof(T));
        }
        uint32_t u32()
        {
            uint32_t v = 0;
            pod(v);
            return v;
        }
        uint64_t u64()
        {
            uint64_t v = 0;
            pod(v);
            return v;
        }
        bool boolean()
        {
            uint8_t v = 0;
            pod(v);
            return v != 0;
        }
        std::string string()
        {
            const uint32_t n = u32();
            if (!m_ok || m_at + n > m_size)
            {
                fail("truncated string");
                return {};
            }
            std::string s(reinterpret_cast<const char *>(m_data + m_at), n);
            m_at += n;
            return s;
        }
        template <typename T>
        std::vector<T> vector()
        {
            static_assert(std::is_trivially_copyable_v<T>, "plain data only");
            const uint64_t n = u64();
            if (!m_ok || n > (m_size - m_at) / std::max<size_t>(sizeof(T), 1))
            {
                fail("truncated vector");
                return {};
            }
            std::vector<T> v(static_cast<size_t>(n));
            if (n)
                raw(v.data(), static_cast<size_t>(n) * sizeof(T));
            return v;
        }
        template <typename A, typename B>
        std::vector<std::pair<A, B>> pairs()
        {
            static_assert(std::is_trivially_copyable_v<A> && std::is_trivially_copyable_v<B>, "plain data only");
            const uint64_t n = u64();
            if (!m_ok || n > (m_size - m_at) / (sizeof(A) + sizeof(B)))
            {
                fail("truncated pairs");
                return {};
            }
            std::vector<std::pair<A, B>> v(static_cast<size_t>(n));
            for (auto &kv : v)
                pod(kv.first), pod(kv.second);
            return v;
        }
        // a block written by Writer::block into `size` bytes at out (the sizes must match)
        bool block(void *out, size_t size)
        {
            constexpr size_t kPage = 4096;
            if (u64() != size)
            {
                fail("block size");
                return false;
            }
            const size_t pages = (size + kPage - 1) / kPage;
            std::vector<uint8_t> used((pages + 7) / 8, 0);
            if (!raw(used.data(), used.size()))
                return false;
            auto *p = static_cast<uint8_t *>(out);
            for (size_t i = 0; i < pages; ++i)
            {
                const size_t n = std::min(kPage, size - i * kPage);
                if (used[i / 8] & (1u << (i % 8)))
                {
                    if (!raw(p + i * kPage, n))
                        return false;
                }
                else
                    std::memset(p + i * kPage, 0, n);
            }
            return true;
        }
        // a section: the tag must be next; returns its end offset (for skip / checks)
        bool begin(const char (&tag)[5])
        {
            char got[4] = {};
            if (!raw(got, 4) || std::memcmp(got, tag, 4) != 0)
            {
                if (m_ok)
                    fail("section order");
                m_error = std::string("section ") + tag;
                return false;
            }
            const uint64_t size = u64();
            if (!m_ok || size > m_size - m_at)
            {
                fail("section size");
                return false;
            }
            m_ends.push_back(m_at + static_cast<size_t>(size));
            return true;
        }
        // the section must have been read exactly
        bool end()
        {
            if (m_ends.empty())
            {
                fail("section end");
                return false;
            }
            const size_t want = m_ends.back();
            m_ends.pop_back();
            if (m_ok && m_at != want)
                fail("section length");
            return m_ok;
        }

    private:
        const uint8_t *m_data;
        size_t m_size;
        size_t m_at = 0;
        bool m_ok = true;
        std::string m_error;
        std::vector<size_t> m_ends;
    };
}
