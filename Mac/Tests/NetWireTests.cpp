// What the extender puts on the wire, compiled with Norbyte's definitions and
// his protocol as this build has them, against the bytes the protocol's
// definition (Extender/Shared/ExtenderProtocol.proto) gives: the message's
// id, the protocol's version, each message as his code fills it in, and the
// build's name the client sends. The bytes of the protocol's earlier
// definition (proto3, which leaves out what is zero) are parsed too.

#include <stdafx.h>
#include <Extender/Client/ClientNetworking.h>
#include <Extender/Server/ServerNetworking.h>
#pragma warning(push)
#pragma warning(disable:4267)
#include <Extender/Shared/ExtenderProtocol.pb.h>
#pragma warning(pop)

#include <Mac/Layout/GameFiles.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// What his string and his assertions take from the extender, which the tests
// do not link: the game's allocator, here the C library's, and the function
// his assertions fail through.
BEGIN_SE()

void* GameAllocRaw(std::size_t size)
{
    return malloc(size);
}

void GameFree(void* ptr) noexcept
{
    free(ptr);
}

void AssertionFailed(char const* expr)
{
    fprintf(stderr, "assertion of Norbyte's code failed: %s\n", expr);
    abort();
}

END_SE()

namespace {

namespace net = bg3se::net;
namespace ecl = bg3se::ecl;
namespace esv = bg3se::esv;
namespace mac = bg3se::mac;
using bg3se::STDString;

int gFailures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
            gFailures++; \
        } \
    } while (0)

std::string Hex(std::string const& bytes)
{
    static char const digits[] = "0123456789abcdef";
    std::string text;
    for (unsigned char c : bytes) {
        text += digits[c >> 4];
        text += digits[c & 0xf];
    }
    return text;
}

std::string Bytes(char const* hex)
{
    std::string bytes;
    for (size_t n = 0; hex[n] && hex[n + 1]; n += 2) {
        bytes += (char)std::stoi(std::string(hex + n, 2), nullptr, 16);
    }
    return bytes;
}

bool Encodes(net::MessageWrapper const& msg, std::string const& expected)
{
    std::string bytes;
    if (!msg.SerializeToString(&bytes)) return false;
    if (Hex(bytes) != expected) {
        fprintf(stderr, "  wrote %s, expected %s\n", Hex(bytes).c_str(), expected.c_str());
        return false;
    }
    return msg.ByteSizeLong() == bytes.size();
}

// The message's id and version and the extender's protocols, as the other
// side has them. The game's own messages end at 325 on the Mac game.
static_assert((uint32_t)net::ExtenderMessage::MessageId == 400);
static_assert((uint32_t)net::ProtoVersion::Current == 2 && (uint32_t)net::ProtoVersion::VerBinSerializer == 2);
static_assert(ecl::ExtenderProtocol::ProtocolId == 100 && esv::ExtenderProtocol::ProtocolId == 101);
static_assert(net::ExtenderMessage::MaxPayloadLength == 0xff000 && net::ExtenderMessage::MaxTotalMessageLength == 0xfffff);

// The message is its length, 32 bits in the writer's byte order, then the
// wrapper's bytes (ExtenderMessage::Serialize). Windows on x64 is little-endian.
static_assert(std::endian::native == std::endian::little);

void a_lua_message_is_written_as_the_protocol_defines_it()
{
    // As Lua/Libs/Net.inl fills it in: the channel and the payload, then the
    // serializer, always; the module, a request and a reply when there are.
    net::MessageWrapper json;
    auto post = json.mutable_post_lua();
    post->set_channel_name("Ch");
    post->set_payload("{}");
    post->set_serializer(net::SerializerType::SERIALIZER_JSON);
    // 1: post_lua, 10 bytes; 1: "Ch"; 2: "{}"; 6: 0, written because it was set.
    CHECK(Encodes(json, "0a0a" "0a024368" "12027b7d" "3000"));

    net::MessageWrapper binary;
    post = binary.mutable_post_lua();
    post->set_channel_name("Ch");
    post->set_payload("{}");
    post->set_module("9a6e6d5a-56e0-4d3b-8b32-27f7c3e0a0f1");
    post->set_request_id(3);
    post->set_reply_id(0);
    post->set_serializer(net::SerializerType::SERIALIZER_BINARY);
    // 3: the module's 36 characters; 4: 3; 5: 0; 6: 1.
    CHECK(Encodes(binary, "0a34" "0a024368" "12027b7d"
        "1a24" "39613665366435612d353665302d346433622d386233322d323766376333653061306631"
        "2003" "2800" "3001"));
}

void the_other_messages_are_written_as_the_protocol_defines_them()
{
    net::MessageWrapper reset;
    reset.mutable_s2c_reset_lua()->set_bootstrap_scripts(true);
    CHECK(Encodes(reset, "1202" "0801"));

    net::MessageWrapper hello;
    hello.mutable_c2s_extender_hello()->set_version((uint32_t)net::ProtoVersion::Current);
    CHECK(Encodes(hello, "2a02" "0802"));

    net::MessageWrapper kick;
    kick.mutable_s2c_kick()->set_message("bye");
    CHECK(Encodes(kick, "3a05" "0a03627965"));
}

void user_variables_are_written_as_the_protocol_defines_them()
{
    // As Extender/Shared/UserVariables.inl fills them in: the type, the
    // entity, the key, then one value.
    auto var = [](net::MessageWrapper& msg) {
        auto v = msg.mutable_user_vars()->add_vars();
        v->set_type(net::UserVarType::ENTITY_VAR);
        v->set_uuid1(1);
        v->set_uuid2(2);
        v->set_key("K");
        return v;
    };
    // 1: 1; 2: 2; 3: "K"; the value; 8: 0. Fields go in their numbers' order.
    char const* head = "080110021a014b";

    net::MessageWrapper intval;
    var(intval)->set_intval(0);
    CHECK(Encodes(intval, std::string("420d0a0b") + head + "2000" "4000"));

    net::MessageWrapper dblval;
    var(dblval)->set_dblval(1.5);
    CHECK(Encodes(dblval, std::string("42140a12") + head + "29000000000000f83f" "4000"));

    net::MessageWrapper strval;
    var(strval)->set_strval("s");
    CHECK(Encodes(strval, std::string("420e0a0c") + head + "320173" "4000"));

    net::MessageWrapper luaval;
    var(luaval)->set_luaval("x");
    CHECK(Encodes(luaval, std::string("420e0a0c") + head + "3a0178" "4000"));

    net::MessageWrapper boolval;
    var(boolval)->set_boolval(true);
    CHECK(Encodes(boolval, std::string("420d0a0b") + head + "4000" "4801"));

    net::MessageWrapper binaryval;
    auto module = var(binaryval);
    module->set_type(net::UserVarType::MODULE_VAR);
    module->set_binaryval("x");
    CHECK(Encodes(binaryval, std::string("420e0a0c") + head + "4001" "520178"));
}

void the_earlier_definitions_bytes_are_read()
{
    // proto3 leaves out a field that is zero: the serializer, the reply, the
    // type. Read here, they are zero.
    net::MessageWrapper post;
    CHECK(post.ParseFromString(Bytes("0a080a02436812027b7d")));
    CHECK(post.has_post_lua() && post.post_lua().channel_name() == "Ch" && post.post_lua().payload() == "{}");
    CHECK(post.post_lua().serializer() == net::SerializerType::SERIALIZER_JSON && post.post_lua().reply_id() == 0);
    CHECK(post.post_lua().module().empty() && post.post_lua().request_id() == 0);

    net::MessageWrapper reply;
    CHECK(reply.ParseFromString(Bytes("0a0f0a02436812027b7d1a016d20033001")));
    CHECK(reply.post_lua().module() == "m" && reply.post_lua().request_id() == 3 && reply.post_lua().reply_id() == 0);
    CHECK(reply.post_lua().serializer() == net::SerializerType::SERIALIZER_BINARY);

    net::MessageWrapper reset;
    CHECK(reset.ParseFromString(Bytes("1200")));
    CHECK(reset.has_s2c_reset_lua() && !reset.s2c_reset_lua().bootstrap_scripts());

    net::MessageWrapper vars;
    CHECK(vars.ParseFromString(Bytes("420b0a09080110021a014b2000")));
    CHECK(vars.has_user_vars() && vars.user_vars().vars_size() == 1);
    if (vars.user_vars().vars_size() == 1) {
        auto const& v = vars.user_vars().vars(0);
        CHECK(v.uuid1() == 1 && v.uuid2() == 2 && v.key() == "K");
        CHECK(v.val_case() == net::UserVar::kIntval && v.intval() == 0 && v.type() == net::UserVarType::ENTITY_VAR);
    }
}

void text_that_is_not_utf8_is_read()
{
    // utf8_validation = NONE: a channel's name is taken as bytes.
    net::MessageWrapper post;
    CHECK(post.ParseFromString(Bytes("0a050a01ff3000")));
    CHECK(post.post_lua().channel_name() == "\xff");
}

void the_client_announces_the_extender_in_the_build()
{
    // What NetworkManager::OnClientConnectMessage does to the game's "Gold":
    // the game reads it as a 15-character string held outside.
    STDString build = "Gold";
    build += " Extender_0";
    CHECK(build == "Gold Extender_0");

    unsigned char bytes[sizeof(STDString)];
    static_assert(sizeof(STDString) == mac::kGameStringBytes);
    memcpy(bytes, (void const*)&build, sizeof(bytes));
    char const* text;
    uint32_t length;
    memcpy(&text, bytes, sizeof(text));
    memcpy(&length, bytes + mac::kGameStringLongLength, sizeof(length));
    CHECK((signed char)bytes[mac::kGameStringFlag] < 0);
    CHECK(length == 15 && text == build.data() && memcmp(text, "Gold Extender_0", 16) == 0);
}

}

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    a_lua_message_is_written_as_the_protocol_defines_it();
    the_other_messages_are_written_as_the_protocol_defines_them();
    user_variables_are_written_as_the_protocol_defines_them();
    the_earlier_definitions_bytes_are_read();
    text_that_is_not_utf8_is_read();
    the_client_announces_the_extender_in_the_build();

    if (gFailures) {
        fprintf(stderr, "%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("all network wire checks passed\n");
    return 0;
}
