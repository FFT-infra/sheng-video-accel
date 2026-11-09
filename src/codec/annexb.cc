// SPDX-License-Identifier: MIT

#include "annexb.h"

namespace iris::codec {

namespace {

// The units that have to travel with a keyframe. H.264: SEI, SPS, PPS, access
// unit delimiter, sequence end, stream end, filler. HEVC: VPS, SPS, PPS and
// the two SEI kinds. Everything else is coded picture data.
bool is_parameter_set(uint8_t type, Codec codec)
{
    if (codec == Codec::H264)
        return type >= 6 && type <= 12;
    return type == 32 || type == 33 || type == 34 || type == 39 || type == 40;
}

// Whether the access unit already carries the codec's two essential sets.
bool carries_parameter_set(const std::vector<NalUnit>& units, Codec codec)
{
    const uint8_t sps = codec == Codec::H264 ? 7 : 33;
    const uint8_t pps = codec == Codec::H264 ? 8 : 34;
    bool has_sps = false;
    bool has_pps = false;
    for (const NalUnit& unit : units) {
        has_sps = has_sps || unit.type == sps;
        has_pps = has_pps || unit.type == pps;
    }
    return has_sps && has_pps;
}

uint8_t nal_type(uint8_t byte, Codec codec)
{
    // H.264 keeps the type in the low five bits; HEVC in bits 1..6, with the
    // layer and temporal ids in the header's later bytes.
    return codec == Codec::H264 ? static_cast<uint8_t>(byte & 0x1f) : static_cast<uint8_t>((byte >> 1) & 0x3f);
}

} // namespace

std::vector<NalUnit> split_annexb(const uint8_t* data, size_t size, Codec codec)
{
    // Find each 00 00 01 from where the previous unit ended, so the three
    // bytes that close a four-byte code are never considered again: 00 00 00
    // 01 is one code whose extra zero belongs to its own unit.
    auto find_code = [data, size](size_t from) -> size_t {
        for (size_t i = from; i + 3 < size; ++i)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                return i;
        return size;
    };

    std::vector<NalUnit> units;
    size_t search = 0;
    size_t code = find_code(search);
    while (code != size) {
        const size_t header = code + 3;
        // A four-byte code starts at `code`, which `find_code` matched on
        // its last three bytes; the leading zero is part of this unit and is
        // never the last byte of the previous one. `search` is where the
        // previous unit ended, which is the same check without an off-by-one
        // at the start of the buffer.
        const size_t begin = code > search && data[code - 1] == 0 ? code - 1 : code;
        const size_t next = find_code(header + 1);
        NalUnit unit;
        unit.type = nal_type(data[header], codec);
        unit.bytes.assign(data + begin, data + next);
        units.push_back(std::move(unit));
        search = next;
        code = next;
    }
    return units;
}

std::vector<uint8_t> prepend_parameter_sets(
    const std::vector<uint8_t>& access_unit, std::vector<NalUnit>& last, Codec codec)
{
    const std::vector<NalUnit> units = split_annexb(access_unit.data(), access_unit.size(), codec);
    if (units.empty())
        return access_unit;

    std::vector<uint8_t> out;
    if (!carries_parameter_set(units, codec)) {
        for (const NalUnit& unit : last)
            out.insert(out.end(), unit.bytes.begin(), unit.bytes.end());
    }
    out.insert(out.end(), access_unit.begin(), access_unit.end());

    // Remember what this access unit carried, in order, so a later one can
    // reuse it. Only a set that has both SPS and PPS can stand in for the
    // previous one: an access unit carrying an SEI or an AUD but no
    // parameter sets would otherwise replace the cache with something a later
    // forced IDR cannot be decoded from.
    std::vector<NalUnit> carried;
    for (const NalUnit& unit : units)
        if (is_parameter_set(unit.type, codec))
            carried.push_back(unit);
    if (carries_parameter_set(carried, codec))
        last = std::move(carried);
    return out;
}

} // namespace iris::codec
