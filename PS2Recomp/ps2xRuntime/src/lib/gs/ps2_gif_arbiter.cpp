#include "runtime/ps2_state_io.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
{
    setProcessPacketFn(std::move(processFn));
}

// GOW-Port: compatibilidad con clientes que no necesitan el identificador del PATH.
void GifArbiter::setProcessPacketFn(ProcessPacketFn fn)
{
    if (!fn) { m_processFn = {}; return; }
    m_processFn = [fn = std::move(fn)](GifPathId, const uint8_t *data, uint32_t size) { fn(data, size); };
}

bool GifArbiter::trackPath3Packet(const uint8_t *data, uint32_t sizeBytes)
{
    // GOW-Port: recorrer solo las etiquetas; saltar registros, pixels y el padding REGLIST.
    // El cursor sigue el FIFO PATH3 entre submit/drain sin retener sus payloads.
    auto &input = m_path3Input;
    bool containsImage = false;
    uint32_t offset = 0u;
    while (offset < sizeBytes)
    {
        if (input.payloadBytes)
        {
            containsImage |= input.format >= 2u; // GOW-Port: IMAGE e IMAGE2.
            const uint32_t bytes = std::min(input.payloadBytes, sizeBytes - offset);
            input.payloadBytes -= bytes;
            offset += bytes;
            continue;
        }

        const uint8_t *tag = data + offset;
        if (input.tagBytes || sizeBytes - offset < 16u)
        {
            const uint32_t bytes = std::min(16u - input.tagBytes, sizeBytes - offset);
            std::memcpy(input.tag.data() + input.tagBytes, data + offset, bytes);
            input.tagBytes += bytes;
            offset += bytes;
            if (input.tagBytes < 16u) break;
            tag = input.tag.data();
            input.tagBytes = 0u;
        }
        else
            offset += 16u;

        uint64_t lo = 0u;
        std::memcpy(&lo, tag, sizeof(lo));
        const uint32_t loops = static_cast<uint32_t>(lo & 0x7FFFu);
        input.format = static_cast<uint8_t>((lo >> 58u) & 3u);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60u) & 15u);
        if (!nreg) nreg = 16u;
        if (input.format == 0u)
            input.payloadBytes = loops * nreg * 16u;
        else if (input.format == 1u)
            input.payloadBytes = ((loops * nreg + 1u) / 2u) * 16u;
        else
            input.payloadBytes = loops * 16u;
        containsImage |= input.format >= 2u && loops != 0u;
    }
    return containsImage;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn || pathId < GifPathId::Path1 || pathId > GifPathId::Path3)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && trackPath3Packet(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::submitAndDrain(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!m_queue.empty() || !data || sizeBytes < 16 || !m_processFn || pathId < GifPathId::Path1 || pathId > GifPathId::Path3)
    {
        submit(pathId, data, sizeBytes, path2DirectHl);
        drain();
        return;
    }
    if (pathId == GifPathId::Path3)
        trackPath3Packet(data, sizeBytes); // the PATH3 tag cursor follows every packet
    m_processFn(pathId, data, sizeBytes);
    drain(); // packets the callback submitted meanwhile
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    while (!m_queue.empty())
    {
        // GOW-Port: la excepción DIRECTHL/IMAGE no es un orden débil estricto y no puede
        // usarse como comparador de stable_sort. Agrupar solo por path conserva sus FIFO.
        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                         {
                             return pathPriority(a.pathId) < pathPriority(b.pathId);
                         });
        const size_t count = m_queue.size();
        size_t path2Begin = 0u;
        while (path2Begin < count && m_queue[path2Begin].pathId == GifPathId::Path1)
            ++path2Begin;
        size_t path2End = path2Begin;
        while (path2End < count && m_queue[path2End].pathId == GifPathId::Path2)
            ++path2End;

        for (size_t i = 0u; i < path2Begin; ++i)
        {
            auto &pkt = m_queue[i];
            if (!pkt.data.empty())
                m_processFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
        size_t path2 = path2Begin, path3 = path2End;
        while (path2 < path2End || path3 < count)
        {
            // GOW-Port: decidir entre las cabeceras, nunca adelantar un paquete dentro del path.
            // DIRECTHL espera a IMAGE en la cabecera PATH3; DIRECT mantiene prioridad normal.
            const bool takePath2 = path2 < path2End &&
                (path3 == count || !m_queue[path2].path2DirectHl || !m_queue[path3].path3Image);
            auto &pkt = m_queue[takePath2 ? path2++ : path3++];
            if (!pkt.data.empty())
                m_processFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
        // GOW-Port: conservar los paquetes que el callback haya añadido; se procesan en otra tanda.
        m_queue.erase(m_queue.begin(), m_queue.begin() + count);
    }
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}

// GOW-Port: save states
void GifArbiter::saveState(ps2_state::Writer &w) const
{
    w.begin("GIFA");
    w.u32(static_cast<uint32_t>(m_queue.size()));
    for (const GifArbiterPacket &p : m_queue)
    {
        w.pod(p.pathId), w.boolean(p.path2DirectHl), w.boolean(p.path3Image);
        w.vector(p.data);
    }
    w.pod(m_path3Input);
    w.end();
}

bool GifArbiter::loadState(ps2_state::Reader &r)
{
    if (!r.begin("GIFA"))
        return false;
    std::vector<GifArbiterPacket> queue;
    for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
    {
        GifArbiterPacket p{};
        r.pod(p.pathId);
        p.path2DirectHl = r.boolean(), p.path3Image = r.boolean();
        p.data = r.vector<uint8_t>();
        queue.push_back(std::move(p));
    }
    Path3InputState path3{};
    r.pod(path3);
    if (!r.end())
        return false;
    m_queue = std::move(queue);
    m_path3Input = path3;
    return true;
}
