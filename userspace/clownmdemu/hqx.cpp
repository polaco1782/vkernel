#include "hqx.h"

namespace {

int abs_i32(int value)
{
	return value < 0 ? -value : value;
}

} // namespace

HQx::HQx()
{
}

HQx::~HQx()
{
}

uint32_t HQx::ARGBtoAYUV(uint32_t value)
{
	const int a = static_cast<int>((value >> 24) & 0xFFU);
	const int r = static_cast<int>((value >> 16) & 0xFFU);
	const int g = static_cast<int>((value >> 8) & 0xFFU);
	const int b = static_cast<int>(value & 0xFFU);

	const int y = ((r * 299) + (g * 587) + (b * 114)) / 1000;
	const int u = (((b - y) * 492) / 1000) + 128;
	const int v = (((r - y) * 877) / 1000) + 128;

	return (static_cast<uint32_t>(a) << 24) |
		(static_cast<uint32_t>(y & 0xFF) << 16) |
		(static_cast<uint32_t>(u & 0xFF) << 8) |
		static_cast<uint32_t>(v & 0xFF);
}

bool HQx::isDifferent(
	uint32_t yuv1,
	uint32_t yuv2,
	uint32_t trY,
	uint32_t trU,
	uint32_t trV,
	uint32_t trA)
{
	const uint32_t ayuv1 = ARGBtoAYUV(yuv1);
	const uint32_t ayuv2 = ARGBtoAYUV(yuv2);

	return abs_i32(static_cast<int>((ayuv1 >> 24) & 0xFFU) - static_cast<int>((ayuv2 >> 24) & 0xFFU)) > static_cast<int>(trA) ||
		abs_i32(static_cast<int>((ayuv1 >> 16) & 0xFFU) - static_cast<int>((ayuv2 >> 16) & 0xFFU)) > static_cast<int>(trY) ||
		abs_i32(static_cast<int>((ayuv1 >> 8) & 0xFFU) - static_cast<int>((ayuv2 >> 8) & 0xFFU)) > static_cast<int>(trU) ||
		abs_i32(static_cast<int>(ayuv1 & 0xFFU) - static_cast<int>(ayuv2 & 0xFFU)) > static_cast<int>(trV);
}
