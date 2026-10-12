#include "../modules/WarehouseCodec.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

extern "C" void WarehouseTestArmAllocationFailure_(std::size_t) noexcept;
extern "C" std::size_t WarehouseTestDisarmAllocationFailure_() noexcept;

namespace {
using namespace warehouse;
using namespace warehouse::codec;
namespace wire=warehouse::codec::detail;
using Bytes=std::vector<std::uint8_t>;
int checks=0;
void Check(bool ok,const char* label) {
    ++checks;
    if (!ok) {std::fprintf(stderr,"FAIL warehouse codec: %s\n",label);std::exit(1);}
}
Bytes Text(const char* s) {return Bytes(s,s+std::strlen(s));}
const Bytes prefix=Text("native-fixture-not-a-real-GM1");
void Put32(Bytes& b,std::size_t at,std::uint32_t v) {
    for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(v>>(8*i));
}
void Put64(Bytes& b,std::size_t at,std::uint64_t v) {
    for(unsigned i=0;i<8;++i)b[at+i]=static_cast<std::uint8_t>(v>>(8*i));
}
std::size_t Footer(const Bytes& b) {return b.size()-kFooterBytes;}
void HeaderCrc(Bytes& b) {Put32(b,prefix.size()+92,wire::Crc32({b.data()+prefix.size(),92}));}
void FooterCrc(Bytes& b) {Put32(b,Footer(b)+100,wire::Crc32({b.data()+Footer(b),100}));}
void AllCrc(Bytes& b) {
    HeaderCrc(b);
    Put32(b,Footer(b)+92,wire::Read32LE(b.data()+prefix.size()+92));
    Put32(b,Footer(b)+88,wire::Crc32({b.data()+prefix.size()+kHeaderBytes,
                                     Footer(b)-prefix.size()-kHeaderBytes}));
    FooterCrc(b);
}
void Reject(const Bytes& b,Error expected,const char* label,Limits limits={}) {
    Inventory out; out[7]={{7,90}};const auto before=out;
    auto r=Decode(b,prefix.size(),out,limits);
    Check(r.status==DecodeStatus::Invalid && r.error==expected,label);
    Check(out==before,"failure never partly changes decoded output");
    auto file=b; Error err;
    Check(!StoreAttachment(file,prefix.size(),out,err,limits)&&err==expected,
          "unsafe file cannot be overwritten");
    Check(file==b,"failed store byte-identical");
}
std::string Hex(std::span<const std::uint8_t> b) {
    const char* digits="0123456789abcdef";std::string s;
    for(auto v:b){s+=digits[v>>4];s+=digits[v&15];} return s;
}
void HashTests() {
    Check(Hex(wire::Sha256Digest({}))=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA empty");
    Check(Hex(wire::Sha256Digest(Text("abc")))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA abc");
    Bytes million(1000000,'a');
    Check(Hex(wire::Sha256Digest(million))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA million a");
    auto longText=Text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    Check(Hex(wire::Sha256Digest(longText))=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","SHA padding boundary");
    wire::Sha256 stream;
    for(auto v:longText)stream.Update({&v,1});
    Check(stream.Finish()==wire::Sha256Digest(longText),"SHA streaming chunks");
    Check(wire::Crc32({})==0 && wire::Crc32(Text("123456789"))==0xcbf43926u,"CRC published vectors");
}
Inventory Stocks() {
    Inventory s;s[0]={{1,7},{1,8},{42,-1},{42,-1}};s[155]={{143,-1}};return s;
}
void CodecTests() {
    const auto stocks=Stocks();const auto good=Encode(prefix,prefix.size(),stocks);
    Check(good.size()==prefix.size()+kHeaderBytes+kEmptyPayloadBytes+5*kRecordBytes+kFooterBytes,"explicit wire size");
    Inventory out;out[8]={{7,3}};
    Check(Decode(good,prefix.size(),out).status==DecodeStatus::Ok && out==stocks,"all heroes round-trip without merge");
    Check(std::equal(prefix.begin(),prefix.end(),good.begin()),"prefix byte identical");
    Inventory empty; const auto blank=Encode(prefix,prefix.size(),empty);
    Check(Decode(blank,prefix.size(),out).status==DecodeStatus::Ok && out==empty,"empty snapshot replaces old stock");
    out=stocks;
    Check(Decode(prefix,prefix.size(),out).status==DecodeStatus::NoAttachment && out==stocks,"no attachment does not auto-clear live state");
    // Codec is an exact transport, semantic validation belongs to core/adapter.
    Inventory raw;raw[1]={{(std::numeric_limits<std::int32_t>::min)(),(std::numeric_limits<std::int32_t>::max)()},{9,12345}};
    Check(Decode(Encode(prefix,prefix.size(),raw),prefix.size(),out).status==DecodeStatus::Ok && out==raw,"all signed bits preserved");
    auto mutation=good; mutation[prefix.size()+8]^=1;
    Reject(mutation,Error::OwnerMismatch,"wrong plugin id never ours");
    mutation=good;mutation[prefix.size()+92]^=1;Reject(mutation,Error::BadHeaderCrc,"header damaged");
    mutation=good;mutation.back()^=1;Reject(mutation,Error::BadFooterCrc,"footer damaged");
    mutation=good;Put32(mutation,prefix.size()+24,99);HeaderCrc(mutation);Reject(mutation,Error::BadVersion,"unknown header version");
    mutation=good;Put32(mutation,Footer(mutation)+24,99);FooterCrc(mutation);Reject(mutation,Error::BadVersion,"unknown footer version");
    mutation=good;Put64(mutation,prefix.size()+32,UINT64_MAX);HeaderCrc(mutation);Reject(mutation,Error::BadOffset,"offset overflow");
    mutation=good;Put64(mutation,prefix.size()+40,UINT64_MAX);HeaderCrc(mutation);Reject(mutation,Error::ResourceLimit,"size bomb");
    mutation=good;mutation[0]^=1;Reject(mutation,Error::BadPrefixHash,"wrong save prefix");
    mutation=good;mutation[prefix.size()+kHeaderBytes+9]^=1;Reject(mutation,Error::BadPayloadCrc,"payload damage");
    mutation=good;mutation[prefix.size()+kHeaderBytes]^=1;AllCrc(mutation);Reject(mutation,Error::BadPayloadMagic,"bad payload magic");
    mutation=good;Put32(mutation,prefix.size()+kHeaderBytes+8,156);AllCrc(mutation);Reject(mutation,Error::BadHeroId,"illegal hero id");
    mutation=good;Put32(mutation,prefix.size()+kHeaderBytes+8+12+32,0);AllCrc(mutation);Reject(mutation,Error::BadHeroId,"duplicate hero id");
    mutation=good;Put64(mutation,prefix.size()+kHeaderBytes+12,UINT64_MAX);AllCrc(mutation);Reject(mutation,Error::BadRecordCount,"count bomb");
    mutation=good;mutation.insert(mutation.end()-static_cast<std::ptrdiff_t>(kFooterBytes),0xab);
    Put64(mutation,prefix.size()+40,kEmptyPayloadBytes+41);Put64(mutation,Footer(mutation)+40,kEmptyPayloadBytes+41);AllCrc(mutation);
    Reject(mutation,Error::BadRecordBytes,"extra payload bytes");
    // Every truncation after a recognizable header must fail closed.
    for(std::size_t n=prefix.size()+8;n<good.size();++n) {
        Bytes cut(good.begin(),good.begin()+static_cast<std::ptrdiff_t>(n));
        Inventory sentinel=stocks; const auto r=Decode(cut,prefix.size(),sentinel);
        Check(r.status==DecodeStatus::Invalid && sentinel==stocks,"truncated block never clears inventory");
    }
    Reject(good,Error::ResourceLimit,"scan limit explicit failure",Limits{1,64000000});
    Reject(good,Error::ResourceLimit,"payload budget explicit failure",Limits{64000000,1});
    Check(Decode(good,0,out).error==Error::InvalidBoundary && Decode(good,good.size()+1,out).error==Error::InvalidBoundary,"boundary required");
    // Independent Python struct/hashlib/zlib generated golden bytes, not this encoder.
    std::ifstream in("tests/fixtures/warehouse_v1.hex");std::string golden;
    in>>golden;
    Check(in.good()||in.eof(),"golden fixture readable");
    Check(golden==Hex(good),"wire equals independent golden fixture");
}
void OwnershipTests() {
    auto stocks=Stocks();auto foreign=Text("OTHER_PLUGIN_uninterpreted_bytes");
    auto before=prefix;before.insert(before.end(),foreign.begin(),foreign.end());
    auto file=Encode(before,prefix.size(),stocks); Inventory out;
    Check(Decode(file,prefix.size(),out).status==DecodeStatus::Ok && out==stocks,"foreign before ours");
    Check(std::equal(before.begin(),before.end(),file.begin()),"foreign prefix preserved");
    Error error;stocks[9]={{20,4}};auto oldLength=file.size();
    Check(StoreAttachment(file,prefix.size(),stocks,error),"EOF own block replace");
    Check(file.size()==oldLength+8 && Decode(file,prefix.size(),out).status==DecodeStatus::Ok && out==stocks,"replace not duplicate");
    Check(std::equal(before.begin(),before.end(),file.begin()),"prefix unchanged on replacement");
    file.insert(file.end(),foreign.begin(),foreign.end());const auto frozen=file;
    Check(Decode(file,prefix.size(),out).status==DecodeStatus::Ok && out==stocks,"foreign after ours readable");
    Check(!StoreAttachment(file,prefix.size(),stocks,error)&&error==Error::ForeignSuffix&&file==frozen,"foreign suffix blocks rewrite/move/append");
    auto second=SerializeAttachment(file,prefix.size(),stocks);file.insert(file.end(),second.begin(),second.end());
    Reject(file,Error::Ambiguous,"multiple valid own blocks rejected");
    auto mixed=Encode(prefix,prefix.size(),stocks);auto badNew=SerializeAttachment(mixed,prefix.size(),stocks);badNew.back()^=1;
    mixed.insert(mixed.end(),badNew.begin(),badNew.end());Reject(mixed,Error::Ambiguous,"never fallback to older snapshot");
    auto fake=before;fake.insert(fake.end(),kHeaderMagic.begin(),kHeaderMagic.end());fake.resize(fake.size()+kHeaderBytes,0);
    Reject(fake,Error::OwnerMismatch,"unknown owner magic refuses overwrite");
    auto unknown=before; Check(Decode(unknown,prefix.size(),out).status==DecodeStatus::NoAttachment,"foreign only is not decoded");
    Check(StoreAttachment(unknown,prefix.size(),stocks,error)&&std::equal(before.begin(),before.end(),unknown.begin()),"foreign only preserved while appending");
    auto legacy=prefix;auto marker=Text("H3WHBLK1");legacy.insert(legacy.end(),marker.begin(),marker.end());Reject(legacy,Error::LegacyProbe,"early probe not production stock");
    // Fake owner strings INSIDE native prefix must not be searched.
    auto native=Text("native_H3WHBGN1_H3RndNew.WH_H3WHFIN1_data");auto b=Encode(native,native.size(),stocks);
    Check(Decode(b,native.size(),out).status==DecodeStatus::Ok&&out==stocks,"no whole native payload scan");
    // Codec allocation failures must be explicit and leave the file unchanged.
    auto allocationFile = Encode(prefix, prefix.size(), stocks);
    const auto allocationBefore = allocationFile;
    Error allocationError = Error::None;
    WarehouseTestArmAllocationFailure_(0);
    const bool allocationStored = StoreAttachment(allocationFile, prefix.size(), stocks,
                                                  allocationError);
    const auto forced = WarehouseTestDisarmAllocationFailure_();
    Check(!allocationStored && allocationError == Error::AllocationFailure && forced != 0
          && allocationFile == allocationBefore, "codec allocation rollback");
    // Decode must also roll back after allocating an earlier hero successfully.
    for (std::size_t budget : {std::size_t(0), std::size_t(1), std::size_t(2)}) {
        Inventory decoded=stocks; const auto snapshot=decoded;
        WarehouseTestArmAllocationFailure_(budget);
        const auto result=Decode(allocationBefore,prefix.size(),decoded);
        const auto failures=WarehouseTestDisarmAllocationFailure_();
        Check(result.status==DecodeStatus::Invalid && result.error==Error::AllocationFailure
              &&failures!=0 && decoded==snapshot,"decode allocation failure atomic output");
    }
    // A reserved marker collision never grants permission to rewrite a suffix.
    auto collision=allocationBefore;
    collision.insert(collision.end(),kHeaderMagic.begin(),kHeaderMagic.end());
    collision.resize(collision.size()+kHeaderBytes,0);
    Reject(collision,Error::Ambiguous,"foreign reserved marker collision safely refuses");
    // Seeded byte fuzz: safe result, output invariant for every failure.
    std::uint32_t seed=3;const auto good=Encode(prefix,prefix.size(),stocks);
    for(int i=0;i<800;++i) {
        auto f=good;seed=seed*1664525u+1013904223u;const auto at=prefix.size()+seed%(good.size()-prefix.size());f[at]^=0x40;
        out=stocks;const auto r=Decode(f,prefix.size(),out);
        Check(r.status!=DecodeStatus::Ok && out==stocks,"single-byte corruption no partial apply");
    }
}
}
void RunWarehouseTests_() {
    HashTests();CodecTests();OwnershipTests();
    std::printf("PASS warehouse codec: %d checks\n",checks);
}
