/* MSB-first bitstream writer shared by the generated parameter sets. */

#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

// H.264 and HEVC both synthesise parameter sets from VA metadata because the
// long-format API does not carry the original SPS/PPS/VPS, and AV1 has to
// rebuild whole OBUs for the same reason. All three need the same MSB-first
// writer, so it lives here instead of being copied per codec.
class BitWriter {
public:
    void bit(unsigned value)
    {
        if (bit_offset_ == 0)
            data_.push_back(0);
        data_.back() |= (value & 1u) << (7u - bit_offset_);
        bit_offset_ = (bit_offset_ + 1u) & 7u;
    }

    void bits(uint64_t value, unsigned count)
    {
        for (unsigned i = count; i > 0; --i)
            bit(static_cast<unsigned>(value >> (i - 1)));
    }

    // Exp-Golomb. H.264/HEVC call this ue(v); AV1 calls the identical coding
    // uvlc(). Both spellings are provided so each codec reads like its spec.
    void ue(uint32_t value)
    {
        const uint32_t code_num = value + 1;
        unsigned width = 0;
        for (uint32_t n = code_num; n > 0; n >>= 1)
            ++width;
        for (unsigned i = 1; i < width; ++i)
            bit(0);
        bits(code_num, width);
    }

    void se(int32_t value)
    {
        ue(value > 0 ? static_cast<uint32_t>(value * 2 - 1) : static_cast<uint32_t>(-value * 2));
    }

    void uvlc(uint32_t value) { ue(value); }

    // AV1 su(n): n-bit two's complement.
    void su(int32_t value, unsigned count)
    {
        bits(static_cast<uint64_t>(static_cast<uint32_t>(value)) & ((1ull << count) - 1), count);
    }

    // AV1 ns(n): non-symmetric unsigned, spec 4.10.7. Values below the
    // threshold m use one fewer bit than the rest.
    void ns(uint32_t value, uint32_t n)
    {
        if (n <= 1)
            return;
        unsigned w = 0;
        for (uint32_t v = n; v > 0; v >>= 1)
            ++w;
        const uint32_t m = (1u << w) - n;
        if (value < m) {
            bits(value, w - 1);
            return;
        }
        bits(value + m, w);
    }

    // AV1 le(n): n little-endian bytes. Only valid at a byte boundary.
    void le(uint64_t value, unsigned bytes)
    {
        for (unsigned i = 0; i < bytes; ++i)
            bits((value >> (i * 8)) & 0xffu, 8);
    }

    // AV1 leb128(): 7 payload bits per byte, high bit continues.
    void leb128(uint64_t value)
    {
        do {
            uint8_t byte = value & 0x7fu;
            value >>= 7;
            if (value != 0)
                byte |= 0x80u;
            bits(byte, 8);
        } while (value != 0);
    }

    // Trailing one bit followed by zero padding to the next byte.
    void trailing_bits()
    {
        bit(1);
        while (bit_offset_ != 0)
            bit(0);
    }

    // AV1 byte_alignment(): zero padding only, no trailing one.
    void byte_align()
    {
        while (bit_offset_ != 0)
            bit(0);
    }

    void bytes(const uint8_t* source, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            bits(source[i], 8);
    }

    // Whole bytes, used by BitReader::copy for slice payloads that can run to
    // hundreds of kilobytes at 4K. Avoids a call and a bounds check per bit.
    void append(const uint8_t* source, size_t count)
    {
        if (count == 0)
            return;
        if (bit_offset_ == 0) {
            data_.insert(data_.end(), source, source + count);
            return;
        }
        bytes(source, count);
    }

    bool byte_aligned() const { return bit_offset_ == 0; }

    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::vector<uint8_t> data_;
    unsigned bit_offset_ = 0;
};

// MSB-first reader over one NAL unit with emulation prevention removed.
// Used to rewrite slice headers whose reference syntax cannot be
// reproduced by a generated SPS/PPS.
class BitReader {
public:
    // `nal` starts at the NAL header; `header_bytes` of it are copied
    // verbatim, the rest has emulation prevention removed.
    BitReader(const uint8_t* nal, size_t size, size_t header_bytes)
    {
        bytes_.reserve(size);
        unsigned zeros = 0;
        for (size_t i = 0; i < size; ++i) {
            const uint8_t byte = nal[i];
            if (i >= header_bytes && zeros == 2 && byte == 3) {
                zeros = 0;
                continue;
            }
            bytes_.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
    }
    size_t position() const { return position_; }
    size_t size_bits() const { return bytes_.size() * 8; }
    bool exhausted() const { return position_ >= size_bits(); }
    void seek(size_t bit) { position_ = bit; }
    void skip(size_t count) { position_ += count; }
    unsigned bit()
    {
        const size_t p = position_++;
        if (p >= size_bits())
            return 0;
        return (bytes_[p / 8] >> (7 - p % 8)) & 1u;
    }
    uint32_t bits(unsigned count)
    {
        uint32_t value = 0;
        while (count--)
            value = (value << 1) | bit();
        return value;
    }
    uint32_t ue()
    {
        unsigned zeros = 0;
        while (!bit() && zeros < 32)
            ++zeros;
        return ((1u << zeros) - 1) + bits(zeros);
    }
    int32_t se()
    {
        const uint32_t code = ue();
        return (code & 1) ? static_cast<int32_t>((code + 1) / 2) : -static_cast<int32_t>(code / 2);
    }
    // Copy bits [position, end) into the writer. Slice data is by far the
    // largest thing this moves, and it usually starts and ends on byte
    // boundaries, so take the whole-byte path whenever both sides allow it.
    void copy(BitWriter& writer, size_t end)
    {
        if (end > size_bits())
            end = size_bits();
        if (position_ >= end)
            return;
        if (writer.byte_aligned() && position_ % 8 == 0) {
            const size_t stop = end / 8;
            const size_t from = position_ / 8;
            writer.append(bytes_.data() + from, stop - from);
            position_ = stop * 8;
        }
        while (position_ < end)
            writer.bit(bit());
    }
    const uint8_t* data() const { return bytes_.data(); }
    size_t size() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
    size_t position_ = 0;
};

inline unsigned nal_ceil_log2(unsigned n)
{
    unsigned bits = 0;
    for (unsigned v = n ? n - 1 : 0; v; v >>= 1)
        ++bits;
    return bits;
}

// Emit an Annex-B start code, the codec's NAL header bytes, and the payload
// with emulation prevention applied.
inline void append_escaped_nal(
    std::vector<uint8_t>& stream, std::initializer_list<uint8_t> nal_header, const BitWriter& writer)
{
    stream.insert(stream.end(), { 0, 0, 0, 1 });
    stream.insert(stream.end(), nal_header);
    unsigned zero_count = 0;
    for (const auto byte : writer.data()) {
        if (zero_count >= 2 && byte <= 3) {
            stream.push_back(3);
            zero_count = 0;
        }
        stream.push_back(byte);
        zero_count = byte == 0 ? zero_count + 1 : 0;
    }
}
