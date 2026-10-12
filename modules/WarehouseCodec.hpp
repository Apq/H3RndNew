#pragma once
#include "WarehouseCore.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace warehouse::codec {
constexpr std::array<std::uint8_t,8> kHeaderMagic={'H','3','W','H','B','G','N','1'};
constexpr std::array<std::uint8_t,8> kPayloadMagic={'H','3','W','H','D','A','T','1'};
constexpr std::array<std::uint8_t,8> kFooterMagic={'H','3','W','H','F','I','N','1'};
constexpr std::array<std::uint8_t,16> kOwner={'H','3','R','n','d','N','e','w','.','W','H',0,0,0,0,0};
constexpr std::uint32_t kVersion=1;
constexpr std::size_t kHeaderBytes=96, kFooterBytes=104;
constexpr std::size_t kRecordBytes=8, kHeroDirectoryBytes=12;
constexpr std::size_t kEmptyPayloadBytes=8+kHeroCount*kHeroDirectoryBytes;
// Adjustable resource budgets, NOT gameplay capacity. Exceeding them is failure.
struct Limits {
    std::size_t maxScanBytes=64u*1024u*1024u;
    std::size_t maxPayloadBytes=64u*1024u*1024u;
};
enum class DecodeStatus { NoAttachment, Ok, Invalid };
enum class Error {
    None, InvalidBoundary, ResourceLimit, Truncated, OwnerMismatch,
    BadHeaderCrc, BadFooterCrc, BadVersion, BadOffset, BadPayloadLength,
    BadPrefixHash, BadPayloadCrc, BadPayloadMagic, BadHeroId,
    BadRecordCount, BadRecordBytes, Ambiguous, ForeignSuffix,
    OrphanMarker, LegacyProbe, AllocationFailure
};
struct DecodeResult { DecodeStatus status=DecodeStatus::NoAttachment; Error error=Error::None; };
enum class AttachmentKind { None, Valid, Damaged };
struct AttachmentLocation {
    AttachmentKind kind=AttachmentKind::None;
    Error error=Error::None;
    std::size_t blockStart=0, footerOffset=0, blockEnd=0;
};

namespace detail {

inline std::uint32_t RotR(std::uint32_t value, unsigned count) noexcept {
    return (value >> count) | (value << (32u - count));
}

inline std::uint32_t Read32BE(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24)
        | (static_cast<std::uint32_t>(data[1]) << 16)
        | (static_cast<std::uint32_t>(data[2]) << 8)
        | static_cast<std::uint32_t>(data[3]);
}

inline void Write32BE(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

inline std::uint32_t Read32LE(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8)
        | (static_cast<std::uint32_t>(data[2]) << 16)
        | (static_cast<std::uint32_t>(data[3]) << 24);
}

inline std::uint64_t Read64LE(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8u);
    return value;
}

inline void Write32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

inline void Write64LE(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8u)));
}

inline std::uint32_t Crc32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

class Sha256 {
public:
    Sha256() noexcept {
        state_[0] = 0x6A09E667u; state_[1] = 0xBB67AE85u;
        state_[2] = 0x3C6EF372u; state_[3] = 0xA54FF53Au;
        state_[4] = 0x510E527Fu; state_[5] = 0x9B05688Cu;
        state_[6] = 0x1F83D9ABu; state_[7] = 0x5BE0CD19u;
    }

    void Update(std::span<const std::uint8_t> data) noexcept {
        for (const std::uint8_t byte : data) {
            block_[blockBytes_++] = byte;
            if (blockBytes_ == block_.size()) {
                Transform();
                blockBytes_ = 0;
            }
            ++totalBytes_;
        }
    }

    std::array<std::uint8_t, 32> Finish() noexcept {
        const std::uint64_t bitLength = totalBytes_ * 8u;
        block_[blockBytes_++] = 0x80;
        if (blockBytes_ > 56) {
            while (blockBytes_ < block_.size()) block_[blockBytes_++] = 0;
            Transform();
            blockBytes_ = 0;
        }
        while (blockBytes_ < 56) block_[blockBytes_++] = 0;
        for (unsigned i = 0; i < 8; ++i)
            block_[blockBytes_++] = static_cast<std::uint8_t>(bitLength >> (56u - i * 8u));
        Transform();

        std::array<std::uint8_t, 32> digest = {};
        for (unsigned i = 0; i < 8; ++i) Write32BE(digest.data() + i * 4, state_[i]);
        return digest;
    }

private:
    void Transform() noexcept {
        static constexpr std::uint32_t k[64] = {
            0x428A2F98u,0x71374491u,0xB5C0FBCFu,0xE9B5DBA5u,
            0x3956C25Bu,0x59F111F1u,0x923F82A4u,0xAB1C5ED5u,
            0xD807AA98u,0x12835B01u,0x243185BEu,0x550C7DC3u,
            0x72BE5D74u,0x80DEB1FEu,0x9BDC06A7u,0xC19BF174u,
            0xE49B69C1u,0xEFBE4786u,0x0FC19DC6u,0x240CA1CCu,
            0x2DE92C6Fu,0x4A7484AAu,0x5CB0A9DCu,0x76F988DAu,
            0x983E5152u,0xA831C66Du,0xB00327C8u,0xBF597FC7u,
            0xC6E00BF3u,0xD5A79147u,0x06CA6351u,0x14292967u,
            0x27B70A85u,0x2E1B2138u,0x4D2C6DFCu,0x53380D13u,
            0x650A7354u,0x766A0ABBu,0x81C2C92Eu,0x92722C85u,
            0xA2BFE8A1u,0xA81A664Bu,0xC24B8B70u,0xC76C51A3u,
            0xD192E819u,0xD6990624u,0xF40E3585u,0x106AA070u,
            0x19A4C116u,0x1E376C08u,0x2748774Cu,0x34B0BCB5u,
            0x391C0CB3u,0x4ED8AA4Au,0x5B9CCA4Fu,0x682E6FF3u,
            0x748F82EEu,0x78A5636Fu,0x84C87814u,0x8CC70208u,
            0x90BEFFFAu,0xA4506CEBu,0xBEF9A3F7u,0xC67178F2u
        };
        std::uint32_t w[64] = {};
        for (unsigned i = 0; i < 16; ++i) w[i] = Read32BE(block_.data() + i * 4);
        for (unsigned i = 16; i < 64; ++i) {
            const std::uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
        std::uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (unsigned i = 0; i < 64; ++i) {
            const std::uint32_t s1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }

    std::array<std::uint8_t, 64> block_ = {};
    std::array<std::uint32_t, 8> state_ = {};
    std::size_t blockBytes_ = 0;
    std::uint64_t totalBytes_ = 0;
};

inline std::array<std::uint8_t, 32> Sha256Digest(std::span<const std::uint8_t> data) noexcept {
    Sha256 sha;
    sha.Update(data);
    return sha.Finish();
}

inline bool EqualBytes(const std::uint8_t* left, const std::uint8_t* right,
                       std::size_t count) noexcept {
    return std::memcmp(left, right, count) == 0;
}

inline bool MagicAt(std::span<const std::uint8_t> data, std::size_t offset,
                    const std::array<std::uint8_t, 8>& magic) noexcept {
    return offset <= data.size() && data.size() - offset >= magic.size()
        && EqualBytes(data.data() + offset, magic.data(), magic.size());
}

} // namespace detail

// Header/footer offsets are wire offsets, never sizeof(C++ struct).
// Both: magic8, owner16, version4, size4, blockStart8, payloadBytes8,
// nativeEnd8, prefixSHA256[32] (0..87).
// Header: reserved4, CRC32[0,92). Footer: payloadCRC4, headerCRC4,
// reserved4, CRC32[0,100).
inline Error ValidatePayload(std::span<const std::uint8_t> payload) noexcept {
    if (payload.size()<kEmptyPayloadBytes) return Error::BadPayloadLength;
    if (!detail::MagicAt(payload,0,kPayloadMagic)) return Error::BadPayloadMagic;
    std::size_t pos=8;
    for (std::size_t hero=0; hero<kHeroCount; ++hero) {
        if (payload.size()-pos<kHeroDirectoryBytes) return Error::BadRecordBytes;
        if (detail::Read32LE(payload.data()+pos)!=hero) return Error::BadHeroId;
        const auto count=detail::Read64LE(payload.data()+pos+4);
        pos+=kHeroDirectoryBytes;
        const auto directories=(kHeroCount-hero-1)*kHeroDirectoryBytes;
        if (payload.size()-pos<directories || count>(payload.size()-pos-directories)/kRecordBytes)
            return Error::BadRecordCount;
        pos+=static_cast<std::size_t>(count)*kRecordBytes;
    }
    return pos==payload.size()?Error::None:Error::BadRecordBytes;
}

// nativeEnd must be supplied by a VERIFIED native stream boundary reader.
// Do not guess it from a magic match or standard gzip CRC acceptance.
inline AttachmentLocation FindAttachment(std::span<const std::uint8_t> file,
    std::size_t nativeEnd, Limits limits={}) noexcept {
    auto fail=[](Error e){return AttachmentLocation{AttachmentKind::Damaged,e};};
    if (!nativeEnd || nativeEnd>file.size()) return fail(Error::InvalidBoundary);
    if (file.size()-nativeEnd>limits.maxScanBytes) return fail(Error::ResourceLimit);
    AttachmentLocation found;
    constexpr std::array<std::uint8_t,8> oldHead={'H','3','W','H','B','L','K','1'};
    constexpr std::array<std::uint8_t,8> oldFoot={'H','3','W','H','E','N','D','1'};
    for (std::size_t pos=nativeEnd; pos<file.size();) {
        if (!detail::MagicAt(file,pos,kHeaderMagic)) {
            if (detail::MagicAt(file,pos,kFooterMagic)) return fail(Error::OrphanMarker);
            if (detail::MagicAt(file,pos,oldHead)||detail::MagicAt(file,pos,oldFoot))
                return fail(Error::LegacyProbe);
            const auto remaining=file.size()-pos;
            if (remaining>=4 && remaining<8
                && (detail::EqualBytes(file.data()+pos,kHeaderMagic.data(),remaining)
                    ||detail::EqualBytes(file.data()+pos,kFooterMagic.data(),remaining)))
                return fail(Error::Truncated);
            ++pos;
            continue;
        }
        // Collision is not ownership: refuse rather than silently treat a
        // damaged own block as an old save. Never delete collision bytes.
        if (found.kind==AttachmentKind::Valid) return fail(Error::Ambiguous);
        if (file.size()-pos<kHeaderBytes) return fail(Error::Truncated);
        const auto* h=file.data()+pos;
        if (!detail::EqualBytes(h+8,kOwner.data(),kOwner.size())) return fail(Error::OwnerMismatch);
        if (detail::Crc32({h,92})!=detail::Read32LE(h+92)) return fail(Error::BadHeaderCrc);
        if (detail::Read32LE(h+24)!=kVersion || detail::Read32LE(h+28)!=kHeaderBytes
            ||detail::Read32LE(h+88)!=0) return fail(Error::BadVersion);
        if (detail::Read64LE(h+32)!=pos || detail::Read64LE(h+48)!=nativeEnd) return fail(Error::BadOffset);
        const auto payloadBytes64=detail::Read64LE(h+40);
        if (payloadBytes64>limits.maxPayloadBytes) return fail(Error::ResourceLimit);
        const auto available=file.size()-pos-kHeaderBytes;
        if (available<kFooterBytes || payloadBytes64>available-kFooterBytes) return fail(Error::Truncated);
        const auto payloadBytes=static_cast<std::size_t>(payloadBytes64);
        const auto footerOffset=pos+kHeaderBytes+payloadBytes;
        const auto* f=file.data()+footerOffset;
        if (!detail::MagicAt(file,footerOffset,kFooterMagic)) return fail(Error::OrphanMarker);
        if (!detail::EqualBytes(f+8,kOwner.data(),kOwner.size())) return fail(Error::OwnerMismatch);
        if (detail::Crc32({f,100})!=detail::Read32LE(f+100)) return fail(Error::BadFooterCrc);
        if (detail::Read32LE(f+24)!=kVersion || detail::Read32LE(f+28)!=kFooterBytes
            ||detail::Read32LE(f+96)!=0) return fail(Error::BadVersion);
        if (!detail::EqualBytes(h+32,f+32,56) || detail::Read32LE(h+92)!=detail::Read32LE(f+92))
            return fail(Error::BadOffset);
        const auto hash=detail::Sha256Digest(file.first(pos));
        if (!detail::EqualBytes(h+56,hash.data(),hash.size())) return fail(Error::BadPrefixHash);
        const auto payload=file.subspan(pos+kHeaderBytes,payloadBytes);
        if (detail::Crc32(payload)!=detail::Read32LE(f+88)) return fail(Error::BadPayloadCrc);
        const auto error=ValidatePayload(payload);
        if (error!=Error::None) return fail(error);
        found={AttachmentKind::Valid,Error::None,pos,footerOffset,footerOffset+kFooterBytes};
        pos=found.blockEnd; // do NOT scan magic-like record bytes inside our payload
    }
    return found;
}

inline DecodeResult Decode(std::span<const std::uint8_t> file,std::size_t nativeEnd,
    Inventory& output,Limits limits={}) noexcept {
    const auto loc=FindAttachment(file,nativeEnd,limits);
    if (loc.kind==AttachmentKind::None) return {DecodeStatus::NoAttachment,Error::None};
    if (loc.kind==AttachmentKind::Damaged) return {DecodeStatus::Invalid,loc.error};
    Inventory candidate;
    std::size_t pos=loc.blockStart+kHeaderBytes+8;
    try {
        for (auto& hero:candidate) {
            const auto count=static_cast<std::size_t>(detail::Read64LE(file.data()+pos+4));
            pos+=kHeroDirectoryBytes;
            hero.reserve(count);
            for (std::size_t i=0;i<count;++i) {
                ArtifactRecord record;
                const auto a=detail::Read32LE(file.data()+pos);
                const auto b=detail::Read32LE(file.data()+pos+4);
                std::memcpy(&record.artifactId,&a,4); std::memcpy(&record.subtype,&b,4);
                hero.push_back(record); pos+=kRecordBytes;
            }
        }
    } catch (const std::bad_alloc&) { return {DecodeStatus::Invalid,Error::AllocationFailure}; }
      catch (const std::length_error&) { return {DecodeStatus::Invalid,Error::AllocationFailure}; }
    output.swap(candidate);
    return {DecodeStatus::Ok,Error::None};
}

// Pure buffer builder, no I/O. Game integration must separately validate
// operation token/file identity and record semantics before applying inventory.
inline std::vector<std::uint8_t> SerializeAttachment(std::span<const std::uint8_t> prefix,
    std::size_t nativeEnd,const Inventory& inventories,Limits limits={}) {
    if (!nativeEnd||nativeEnd>prefix.size()) throw std::invalid_argument("native boundary");
    std::size_t payloadBytes=kEmptyPayloadBytes;
    const auto maxSize=(std::numeric_limits<std::size_t>::max)();
    for (const auto& hero:inventories) {
        if (hero.size()>(maxSize-payloadBytes)/kRecordBytes) throw std::length_error("payload overflow");
        payloadBytes+=hero.size()*kRecordBytes;
    }
    if (payloadBytes>limits.maxPayloadBytes || payloadBytes>maxSize-kHeaderBytes-kFooterBytes)
        throw std::length_error("payload resource budget");
    const auto blockBytes=kHeaderBytes+payloadBytes+kFooterBytes;
    if (prefix.size()>maxSize-blockBytes || prefix.size()-nativeEnd>limits.maxScanBytes
        ||blockBytes>limits.maxScanBytes-(prefix.size()-nativeEnd)) throw std::length_error("appendage budget");
    std::vector<std::uint8_t> blob;
    blob.reserve(blockBytes);
    const auto hash=detail::Sha256Digest(prefix);
    auto common=[&](const auto& magic,std::uint32_t bytes) {
        blob.insert(blob.end(),magic.begin(),magic.end());
        blob.insert(blob.end(),kOwner.begin(),kOwner.end());
        detail::Write32LE(blob,kVersion); detail::Write32LE(blob,bytes);
        detail::Write64LE(blob,prefix.size()); detail::Write64LE(blob,payloadBytes);
        detail::Write64LE(blob,nativeEnd);
        blob.insert(blob.end(),hash.begin(),hash.end());
    };
    common(kHeaderMagic,static_cast<std::uint32_t>(kHeaderBytes));
    detail::Write32LE(blob,0);
    const auto headerCrc=detail::Crc32(blob);
    detail::Write32LE(blob,headerCrc);
    blob.insert(blob.end(),kPayloadMagic.begin(),kPayloadMagic.end());
    for (std::size_t hero=0;hero<kHeroCount;++hero) {
        detail::Write32LE(blob,static_cast<std::uint32_t>(hero));
        detail::Write64LE(blob,inventories[hero].size());
        for (const auto& record:inventories[hero]) {
            detail::Write32LE(blob,static_cast<std::uint32_t>(record.artifactId));
            detail::Write32LE(blob,static_cast<std::uint32_t>(record.subtype));
        }
    }
    const auto payloadCrc=detail::Crc32({blob.data()+kHeaderBytes,payloadBytes});
    const auto footer=blob.size();
    common(kFooterMagic,static_cast<std::uint32_t>(kFooterBytes));
    detail::Write32LE(blob,payloadCrc); detail::Write32LE(blob,headerCrc);
    detail::Write32LE(blob,0);
    const auto footerCrc=detail::Crc32({blob.data()+footer,100});
    detail::Write32LE(blob,footerCrc);
    return blob;
}

// Fail closed: a foreign suffix may bind preceding bytes/absolute offsets.
// Never edit a middle block, move/delete suffixes, append another generation
// behind them, or choose arbitrarily between duplicate own blocks.
inline bool StoreAttachment(std::vector<std::uint8_t>& file,std::size_t nativeEnd,
    const Inventory& inventories,Error& error,Limits limits={}) noexcept {
    error=Error::None;
    const auto loc=FindAttachment(file,nativeEnd,limits);
    if (loc.kind==AttachmentKind::Damaged) {error=loc.error;return false;}
    if (loc.kind==AttachmentKind::Valid&&loc.blockEnd!=file.size()) {error=Error::ForeignSuffix;return false;}
    const auto prefixLength=loc.kind==AttachmentKind::Valid?loc.blockStart:file.size();
    try {
        const auto blob=SerializeAttachment(std::span<const std::uint8_t>(file).first(prefixLength),
                                            nativeEnd,inventories,limits);
        std::vector<std::uint8_t> updated;
        updated.reserve(prefixLength+blob.size());
        updated.insert(updated.end(),file.begin(),file.begin()+static_cast<std::ptrdiff_t>(prefixLength));
        updated.insert(updated.end(),blob.begin(),blob.end());
        file.swap(updated); return true;
    } catch (const std::bad_alloc&) {error=Error::AllocationFailure;}
      catch (const std::length_error&) {error=Error::ResourceLimit;}
      catch (const std::invalid_argument&) {error=Error::InvalidBoundary;}
    return false;
}
inline std::vector<std::uint8_t> Encode(std::span<const std::uint8_t> prefix,
    std::size_t nativeEnd,const Inventory& inventories,Limits limits={}) {
    std::vector<std::uint8_t> file(prefix.begin(),prefix.end()); Error error;
    if (!StoreAttachment(file,nativeEnd,inventories,error,limits)) throw std::runtime_error("unsafe attachment update");
    return file;
}
} // namespace warehouse::codec
