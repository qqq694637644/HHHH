#include "ScreenCodec.h"

#include <algorithm>
#include <cwchar>
#include <cstring>
#include <turbojpeg.h>
#include <zlib.h>

namespace
{
    bool MarkEquals(const WCHAR* mark, const WCHAR* expected)
    {
        return ::wcsncmp(mark, expected, SCREEN2_SETTING_MARK_SIZE) == 0;
    }

    bool AppendBytes(std::vector<BYTE>& dst, const void* data, size_t size)
    {
        const BYTE* bytes = static_cast<const BYTE*>(data);
        dst.insert(dst.end(), bytes, bytes + size);
        return true;
    }

    bool IsTileChanged(const screen2::RawFrame& raw, const std::vector<BYTE>& base, int left, int top, int right, int bottom)
    {
        for(int y = top; y < bottom; ++y)
        {
            const BYTE* a = raw.pixels.data() + static_cast<size_t>(y) * raw.stride + left * 3;
            const BYTE* b = base.data() + static_cast<size_t>(y) * raw.stride + left * 3;
            if(std::memcmp(a, b, static_cast<size_t>(right - left) * 3) != 0)
                return true;
        }
        return false;
    }
}

namespace screen2
{
    int CalcStride24(int width)
    {
        return ((width * 3) + 3) & ~3;
    }

    bool BuildScreenSizePacket(const RawFrame& frame, std::vector<BYTE>& packet)
    {
        SCREEN_SIZE_MSG msg = {};
        ::wcscpy_s(msg.mark, L"SCREEN2_RET_SIZE");
        msg.virtualX = frame.virtualX;
        msg.virtualY = frame.virtualY;
        msg.width = frame.screenWidth;
        msg.height = frame.screenHeight;
        packet.resize(sizeof(msg));
        std::memcpy(packet.data(), &msg, sizeof(msg));
        return true;
    }

    bool TryParseScreenSizePacket(const BYTE* packet, size_t packetSize, SCREEN_SIZE_MSG& msg)
    {
        if(!packet || packetSize != sizeof(SCREEN_SIZE_MSG))
            return false;
        std::memcpy(&msg, packet, sizeof(msg));
        return MarkEquals(msg.mark, L"SCREEN2_RET_SIZE");
    }

    bool IsScreenFrameV2Packet(const BYTE* packet, size_t packetSize)
    {
        if(!packet || packetSize < sizeof(SCREEN_FRAME_V2))
            return false;
        const SCREEN_FRAME_V2* header = reinterpret_cast<const SCREEN_FRAME_V2*>(packet);
        return MarkEquals(header->mark, L"SCREEN2_RET_FRAME_V2");
    }

    bool CompressZlib(const BYTE* input, DWORD inputSize, std::vector<BYTE>& output)
    {
        output.clear();
        if(!input || inputSize == 0)
            return false;
        uLongf destLen = ::compressBound(inputSize);
        if(destLen == 0 || destLen > MAX_WIRE_PACKET_SIZE)
            return false;
        output.resize(static_cast<size_t>(destLen));
        int result = ::compress2(output.data(), &destLen, input, inputSize, Z_BEST_SPEED);
        if(result != Z_OK || destLen == 0)
        {
            output.clear();
            return false;
        }
        output.resize(static_cast<size_t>(destLen));
        return true;
    }

    bool DecompressZlib(const BYTE* input, DWORD inputSize, DWORD expectedSize, std::vector<BYTE>& output)
    {
        output.clear();
        if(!input || inputSize == 0 || expectedSize == 0 || expectedSize > MAX_ORIGINAL_PAYLOAD_SIZE)
            return false;
        output.resize(expectedSize);
        uLongf destLen = expectedSize;
        int result = ::uncompress(output.data(), &destLen, input, inputSize);
        if(result != Z_OK || destLen != expectedSize)
        {
            output.clear();
            return false;
        }
        return true;
    }

    bool EncodeJpegBgr(const BYTE* pixels, int width, int height, int stride, int quality, std::vector<BYTE>& output)
    {
        output.clear();
        if(!pixels || width <= 0 || height <= 0 || stride < width * 3)
            return false;

        tjhandle handle = ::tjInitCompress();
        if(!handle)
            return false;

        unsigned char* jpegBuffer = nullptr;
        unsigned long jpegSize = 0;
        int flags = TJFLAG_FASTDCT;
        int result = ::tjCompress2(handle,
            pixels,
            width,
            stride,
            height,
            TJPF_BGR,
            &jpegBuffer,
            &jpegSize,
            TJSAMP_420,
            std::max(1, std::min(100, quality)),
            flags);

        bool ok = false;
        if(result == 0 && jpegBuffer && jpegSize > 0 && jpegSize <= MAX_WIRE_PACKET_SIZE)
        {
            output.assign(jpegBuffer, jpegBuffer + jpegSize);
            ok = true;
        }
        if(jpegBuffer)
            ::tjFree(jpegBuffer);
        ::tjDestroy(handle);
        return ok;
    }

    bool DecodeJpegBgr(const BYTE* jpeg, size_t jpegSize, DecodedFrame& output)
    {
        output = DecodedFrame();
        if(!jpeg || jpegSize == 0 || jpegSize > MAX_WIRE_PACKET_SIZE)
            return false;

        tjhandle handle = ::tjInitDecompress();
        if(!handle)
            return false;

        int width = 0;
        int height = 0;
        int subsamp = 0;
        int colorspace = 0;
        if(::tjDecompressHeader3(handle, jpeg, static_cast<unsigned long>(jpegSize), &width, &height, &subsamp, &colorspace) != 0)
        {
            ::tjDestroy(handle);
            return false;
        }
        if(width <= 0 || height <= 0)
        {
            ::tjDestroy(handle);
            return false;
        }
        int stride = CalcStride24(width);
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);

        if(::tjDecompress2(handle,
            jpeg,
            static_cast<unsigned long>(jpegSize),
            pixels.data(),
            width,
            stride,
            height,
            TJPF_BGR,
            TJFLAG_FASTUPSAMPLE | TJFLAG_FASTDCT) != 0)
        {
            ::tjDestroy(handle);
            return false;
        }

        ::tjDestroy(handle);
        output.width = width;
        output.height = height;
        output.stride = stride;
        output.pixels.swap(pixels);
        return true;
    }

    ScreenFrameEncoder::ScreenFrameEncoder(int quality)
        : m_quality(std::max(1, std::min(100, quality))), m_hasBaseline(false), m_forceKeyframe(true),
          m_width(0), m_height(0), m_stride(0), m_nextSequence(1), m_lastCommittedSequence(0), m_pendingSequence(0)
    {
    }

    void ScreenFrameEncoder::Reset()
    {
        m_hasBaseline = false;
        m_forceKeyframe = true;
        m_width = m_height = m_stride = 0;
        m_nextSequence = 1;
        m_lastCommittedSequence = 0;
        m_baseline.clear();
        m_pendingBaseline.clear();
        m_pendingSequence = 0;
    }

    void ScreenFrameEncoder::ForceKeyframe()
    {
        m_forceKeyframe = true;
    }

    void ScreenFrameEncoder::SetQuality(int quality)
    {
        m_quality = std::max(1, std::min(100, quality));
    }

    bool ScreenFrameEncoder::BuildFrame(const RawFrame& raw, std::vector<BYTE>& packet)
    {
        packet.clear();
        if(raw.width <= 0 || raw.height <= 0 || raw.stride < raw.width * 3 || raw.pixels.empty())
            return false;
        if(m_forceKeyframe || !m_hasBaseline || raw.width != m_width || raw.height != m_height || raw.stride != m_stride)
            return BuildFullFrame(raw, packet);
        return BuildDiffFrame(raw, packet);
    }

    void ScreenFrameEncoder::CommitLastBuiltFrame()
    {
        if(m_pendingBaseline.empty() || m_pendingSequence == 0)
            return;
        m_baseline.swap(m_pendingBaseline);
        m_pendingBaseline.clear();
        m_hasBaseline = true;
        m_lastCommittedSequence = m_pendingSequence;
        if(m_nextSequence <= m_pendingSequence)
            m_nextSequence = m_pendingSequence + 1;
        m_pendingSequence = 0;
        m_forceKeyframe = false;
    }

    bool ScreenFrameEncoder::BuildFullFrame(const RawFrame& raw, std::vector<BYTE>& packet)
    {
        std::vector<BYTE> jpeg;
        std::vector<BYTE> compressed;
        if(!EncodeJpegBgr(raw.pixels.data(), raw.width, raw.height, raw.stride, m_quality, jpeg) ||
           !CompressZlib(jpeg.data(), static_cast<DWORD>(jpeg.size()), compressed))
            return false;
        while(compressed.size() > MAX_COMPRESSED_FIRST_SIZE && m_quality > MIN_ADAPTIVE_JPEG_QUALITY)
        {
            m_quality = std::max(MIN_ADAPTIVE_JPEG_QUALITY, m_quality - ADAPTIVE_JPEG_QUALITY_STEP);
            jpeg.clear();
            compressed.clear();
            if(!EncodeJpegBgr(raw.pixels.data(), raw.width, raw.height, raw.stride, m_quality, jpeg) ||
               !CompressZlib(jpeg.data(), static_cast<DWORD>(jpeg.size()), compressed))
                return false;
        }
        const unsigned long long seq = m_nextSequence;
        if(!BuildPacket(SCREEN2_FRAME_TYPE_FULL, seq, 0, compressed.data(), static_cast<DWORD>(jpeg.size()), static_cast<DWORD>(compressed.size()), 0, packet))
            return false;
        m_width = raw.width;
        m_height = raw.height;
        m_stride = raw.stride;
        m_pendingBaseline = raw.pixels;
        m_pendingSequence = seq;
        return true;
    }

    bool ScreenFrameEncoder::BuildDiffFrame(const RawFrame& raw, std::vector<BYTE>& packet)
    {
        std::vector<RECT> rects;
        if(!ScanChangedRects(raw, rects))
            return BuildFullFrame(raw, packet);
        if(rects.empty())
            return false;

        std::vector<BYTE> payload;
        for(size_t i = 0; i < rects.size(); ++i)
        {
            const RECT& r = rects[i];
            int rw = r.right - r.left;
            int rh = r.bottom - r.top;
            int rs = CalcStride24(rw);
            std::vector<BYTE> rectPixels(static_cast<size_t>(rs) * rh);
            for(int y = 0; y < rh; ++y)
            {
                const BYTE* src = raw.pixels.data() + static_cast<size_t>(r.top + y) * raw.stride + r.left * 3;
                std::memcpy(rectPixels.data() + static_cast<size_t>(y) * rs, src, rw * 3);
            }
            std::vector<BYTE> jpeg;
            if(!EncodeJpegBgr(rectPixels.data(), rw, rh, rs, m_quality, jpeg) || jpeg.empty())
                return BuildFullFrame(raw, packet);
            CHANGED_RECT_DATA rd = {};
            rd.jpegSize = static_cast<int>(jpeg.size());
            rd.rect = r;
            AppendBytes(payload, &rd, sizeof(rd));
            AppendBytes(payload, jpeg.data(), jpeg.size());
            if(payload.size() > MAX_DIFF_FRAME_SIZE)
                return BuildFullFrame(raw, packet);
        }

        std::vector<BYTE> compressed;
        if(!CompressZlib(payload.data(), static_cast<DWORD>(payload.size()), compressed) || compressed.size() > MAX_COMPRESSED_DIFF_SIZE)
            return BuildFullFrame(raw, packet);
        const unsigned long long seq = m_nextSequence;
        if(!BuildPacket(SCREEN2_FRAME_TYPE_DIFF, seq, m_lastCommittedSequence, compressed.data(), static_cast<DWORD>(payload.size()), static_cast<DWORD>(compressed.size()), static_cast<int>(rects.size()), packet))
            return false;
        m_pendingBaseline = raw.pixels;
        m_pendingSequence = seq;
        return true;
    }

    bool ScreenFrameEncoder::BuildPacket(unsigned int frameType, unsigned long long sequence, unsigned long long baseSequence,
        const BYTE* payload, DWORD originalSize, DWORD payloadSize, int rectCount, std::vector<BYTE>& packet)
    {
        if(!payload || originalSize == 0 || payloadSize == 0)
            return false;
        size_t totalSize = sizeof(SCREEN_FRAME_V2) + payloadSize;
        if(totalSize > MAX_FRAME_PACKET_SIZE)
            return false;
        packet.assign(totalSize, 0);
        SCREEN_FRAME_V2* h = reinterpret_cast<SCREEN_FRAME_V2*>(packet.data());
        ::wcscpy_s(h->mark, L"SCREEN2_RET_FRAME_V2");
        h->version = SCREEN2_PROTOCOL_VERSION_V2;
        h->headerSize = static_cast<unsigned int>(sizeof(SCREEN_FRAME_V2));
        h->frameType = frameType;
        h->sequence = sequence;
        h->baseSequence = baseSequence;
        h->originalSize = static_cast<int>(originalSize);
        h->payloadSize = static_cast<int>(payloadSize);
        h->rectCount = rectCount;
        h->compression = SCREEN2_COMPRESSION_ZLIB;
        h->flags = frameType == SCREEN2_FRAME_TYPE_FULL ? SCREEN2_FRAME_FLAG_KEYFRAME : 0;
        std::memcpy(packet.data() + sizeof(SCREEN_FRAME_V2), payload, payloadSize);
        return true;
    }

    bool ScreenFrameEncoder::ScanChangedRects(const RawFrame& raw, std::vector<RECT>& rects) const
    {
        rects.clear();
        if(!m_hasBaseline || m_baseline.size() != raw.pixels.size())
            return false;
        int tilesX = (raw.width + DIRTY_TILE_WIDTH - 1) / DIRTY_TILE_WIDTH;
        int tilesY = (raw.height + DIRTY_TILE_HEIGHT - 1) / DIRTY_TILE_HEIGHT;
        for(int ty = 0; ty < tilesY; ++ty)
        {
            for(int tx = 0; tx < tilesX; ++tx)
            {
                int left = tx * DIRTY_TILE_WIDTH;
                int top = ty * DIRTY_TILE_HEIGHT;
                int right = std::min(left + DIRTY_TILE_WIDTH, raw.width);
                int bottom = std::min(top + DIRTY_TILE_HEIGHT, raw.height);
                if(IsTileChanged(raw, m_baseline, left, top, right, bottom))
                {
                    RECT r = { left, top, right, bottom };
                    rects.push_back(r);
                    if(rects.size() > MAX_FRAME_RECTS)
                        return false;
                }
            }
        }
        return true;
    }

    ScreenFrameDecoder::ScreenFrameDecoder()
        : m_hasBaseline(false), m_lastSequence(0), m_width(0), m_height(0), m_stride(0)
    {
    }

    void ScreenFrameDecoder::Reset()
    {
        m_hasBaseline = false;
        m_lastSequence = 0;
        m_width = m_height = m_stride = 0;
        m_baseline.clear();
    }

    bool ScreenFrameDecoder::DecodePacket(const BYTE* packet, size_t packetSize, DecodedFrame& frame, bool& needsKeyframe)
    {
        frame = DecodedFrame();
        needsKeyframe = false;
        if(!IsScreenFrameV2Packet(packet, packetSize))
        {
            needsKeyframe = true;
            return false;
        }
        const SCREEN_FRAME_V2* h = reinterpret_cast<const SCREEN_FRAME_V2*>(packet);
        if(h->version != SCREEN2_PROTOCOL_VERSION_V2 || h->headerSize != sizeof(SCREEN_FRAME_V2) ||
           h->compression != SCREEN2_COMPRESSION_ZLIB || h->payloadSize <= 0 || h->originalSize <= 0 ||
           sizeof(SCREEN_FRAME_V2) + static_cast<size_t>(h->payloadSize) != packetSize)
        {
            needsKeyframe = true;
            return false;
        }
        const BYTE* compressed = packet + sizeof(SCREEN_FRAME_V2);
        if(h->frameType == SCREEN2_FRAME_TYPE_FULL)
            return DecodeFullFrame(*h, compressed, frame, needsKeyframe);
        if(h->frameType == SCREEN2_FRAME_TYPE_DIFF)
            return DecodeDiffFrame(*h, compressed, frame, needsKeyframe);
        needsKeyframe = true;
        return false;
    }

    bool ScreenFrameDecoder::DecodeFullFrame(const SCREEN_FRAME_V2& h, const BYTE* compressed, DecodedFrame& frame, bool& needsKeyframe)
    {
        if(h.sequence == 0 || h.baseSequence != 0 || h.rectCount != 0)
        {
            needsKeyframe = true;
            return false;
        }
        std::vector<BYTE> jpeg;
        if(!DecompressZlib(compressed, static_cast<DWORD>(h.payloadSize), static_cast<DWORD>(h.originalSize), jpeg) ||
           !DecodeJpegBgr(jpeg.data(), jpeg.size(), frame))
        {
            needsKeyframe = true;
            return false;
        }
        m_width = frame.width;
        m_height = frame.height;
        m_stride = frame.stride;
        m_baseline = frame.pixels;
        m_lastSequence = h.sequence;
        m_hasBaseline = true;
        return true;
    }

    bool ScreenFrameDecoder::DecodeDiffFrame(const SCREEN_FRAME_V2& h, const BYTE* compressed, DecodedFrame& frame, bool& needsKeyframe)
    {
        if(!m_hasBaseline || h.sequence == 0 || h.sequence <= m_lastSequence || h.baseSequence != m_lastSequence || h.rectCount <= 0 || h.rectCount > MAX_FRAME_RECTS)
        {
            needsKeyframe = true;
            return false;
        }
        std::vector<BYTE> payload;
        if(!DecompressZlib(compressed, static_cast<DWORD>(h.payloadSize), static_cast<DWORD>(h.originalSize), payload))
        {
            needsKeyframe = true;
            return false;
        }
        size_t offset = 0;
        for(int i = 0; i < h.rectCount; ++i)
        {
            if(offset + sizeof(CHANGED_RECT_DATA) > payload.size())
            {
                needsKeyframe = true;
                return false;
            }
            CHANGED_RECT_DATA rd = {};
            std::memcpy(&rd, payload.data() + offset, sizeof(rd));
            offset += sizeof(rd);
            int rw = rd.rect.right - rd.rect.left;
            int rh = rd.rect.bottom - rd.rect.top;
            if(rd.jpegSize <= 0 || offset + static_cast<size_t>(rd.jpegSize) > payload.size() ||
               rw <= 0 || rh <= 0 || rd.rect.left < 0 || rd.rect.top < 0 || rd.rect.right > m_width || rd.rect.bottom > m_height)
            {
                needsKeyframe = true;
                return false;
            }
            DecodedFrame rectFrame;
            if(!DecodeJpegBgr(payload.data() + offset, static_cast<size_t>(rd.jpegSize), rectFrame) || rectFrame.width != rw || rectFrame.height != rh)
            {
                needsKeyframe = true;
                return false;
            }
            offset += static_cast<size_t>(rd.jpegSize);
            for(int y = 0; y < rh; ++y)
            {
                BYTE* dst = m_baseline.data() + static_cast<size_t>(rd.rect.top + y) * m_stride + rd.rect.left * 3;
                const BYTE* src = rectFrame.pixels.data() + static_cast<size_t>(y) * rectFrame.stride;
                std::memcpy(dst, src, rw * 3);
            }
        }
        if(offset != payload.size())
        {
            needsKeyframe = true;
            return false;
        }
        m_lastSequence = h.sequence;
        frame.width = m_width;
        frame.height = m_height;
        frame.stride = m_stride;
        frame.pixels = m_baseline;
        return true;
    }
}
