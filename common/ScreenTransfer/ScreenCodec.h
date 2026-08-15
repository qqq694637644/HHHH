#pragma once

#include "ScreenProtocol.h"

#include <vector>

namespace screen2
{
    struct RawFrame
    {
        int virtualX;
        int virtualY;
        int screenWidth;
        int screenHeight;
        int width;
        int height;
        int stride;
        std::vector<BYTE> pixels; // top-down 24bpp BGR, stride aligned to 4 bytes

        RawFrame()
            : virtualX(0), virtualY(0), screenWidth(0), screenHeight(0),
              width(0), height(0), stride(0)
        {
        }
    };

    struct DecodedFrame
    {
        int width;
        int height;
        int stride;
        std::vector<BYTE> pixels; // top-down 24bpp BGR

        DecodedFrame()
            : width(0), height(0), stride(0)
        {
        }
    };

    struct EncodedFrameInfo
    {
        bool hasFrame;
        bool isKeyframe;
        bool saturated;
        DWORD originalSize;
        DWORD compressedSize;
        size_t totalBytes;
        int rectCount;
        int quality;
        unsigned long long sequence;
        unsigned long long baseSequence;

        EncodedFrameInfo()
            : hasFrame(false), isKeyframe(false), saturated(false), originalSize(0),
              compressedSize(0), totalBytes(0), rectCount(0), quality(DEFAULT_JPEG_QUALITY),
              sequence(0), baseSequence(0)
        {
        }
    };

    int CalcStride24(int width);

    bool BuildScreenSizePacket(const RawFrame& frame, std::vector<BYTE>& packet);
    bool TryParseScreenSizePacket(const BYTE* packet, size_t packetSize, SCREEN_SIZE_MSG& msg);
    bool IsScreenFrameV2Packet(const BYTE* packet, size_t packetSize);

    bool CompressZlib(const BYTE* input, DWORD inputSize, std::vector<BYTE>& output);
    bool DecompressZlib(const BYTE* input, DWORD inputSize, DWORD expectedSize, std::vector<BYTE>& output);

    bool EncodeJpegBgr(const BYTE* pixels, int width, int height, int stride, int quality, std::vector<BYTE>& output);
    bool DecodeJpegBgr(const BYTE* jpeg, size_t jpegSize, DecodedFrame& output);

    class ScreenFrameEncoder
    {
    public:
        explicit ScreenFrameEncoder(int quality = DEFAULT_JPEG_QUALITY);
        void Reset();
        void ForceKeyframe();
        void SetQuality(int quality);
        int GetQuality() const;
        bool BuildFrame(const RawFrame& raw, std::vector<BYTE>& packet);
        bool BuildFrame(const RawFrame& raw, std::vector<BYTE>& packet, EncodedFrameInfo* info);
        void CommitLastBuiltFrame();

    private:
        bool BuildFullFrame(const RawFrame& raw, std::vector<BYTE>& packet, EncodedFrameInfo* info);
        bool BuildDiffFrame(const RawFrame& raw, std::vector<BYTE>& packet, EncodedFrameInfo* info);
        bool BuildPacket(unsigned int frameType,
                         unsigned long long sequence,
                         unsigned long long baseSequence,
                         const BYTE* payload,
                         DWORD originalSize,
                         DWORD payloadSize,
                         int rectCount,
                         std::vector<BYTE>& packet);
        bool ScanChangedRects(const RawFrame& raw, std::vector<RECT>& rects) const;

        int m_quality;
        bool m_hasBaseline;
        bool m_forceKeyframe;
        int m_width;
        int m_height;
        int m_stride;
        unsigned long long m_nextSequence;
        unsigned long long m_lastCommittedSequence;
        std::vector<BYTE> m_baseline;
        std::vector<BYTE> m_pendingBaseline;
        unsigned long long m_pendingSequence;
    };

    class ScreenFrameDecoder
    {
    public:
        ScreenFrameDecoder();
        void Reset();
        bool DecodePacket(const BYTE* packet, size_t packetSize, DecodedFrame& frame, bool& needsKeyframe);

    private:
        bool DecodeFullFrame(const SCREEN_FRAME_V2& header, const BYTE* compressed, DecodedFrame& frame, bool& needsKeyframe);
        bool DecodeDiffFrame(const SCREEN_FRAME_V2& header, const BYTE* compressed, DecodedFrame& frame, bool& needsKeyframe);

        bool m_hasBaseline;
        unsigned long long m_lastSequence;
        int m_width;
        int m_height;
        int m_stride;
        std::vector<BYTE> m_baseline;
    };
}
