#include "include/zlib.h"
#include "../Utils/LightArray.h"

LightArray<BYTE> DeflateDecompress(const BYTE* compressedData, UINT32 compressedSize)
{
	LightArray<BYTE> result;

	// 估算解压后大小（通常是压缩大小的3-5倍）
	uLongf destLen = compressedSize * 4;
	result.resize(destLen);

	// 使用 zlib 的 uncompress 函数尝试解压
	int ret = uncompress(result.data(), &destLen, compressedData, compressedSize);

	if (ret == Z_OK)
	{
		result.resize(destLen);
		return result;
	}
	else
	{
		// 如果失败，尝试更大的缓冲区
		if (ret == Z_BUF_ERROR)
		{
			destLen = compressedSize * 8;  // 尝试更大的缓冲区
			result.resize(destLen);
			ret = uncompress(result.data(), &destLen, compressedData, compressedSize);

			if (ret == Z_OK)
			{
				result.resize(destLen);
				return result;
			}
		}
	}
	return {};
}