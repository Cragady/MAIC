// Skeletons, canonical JSON and the protocol hash (docs/design/engine-protocol.md section 17), and the checker
// reading an event of an unknown type by its declared skeleton.

#include "check.hpp"

#include "maic/protocol.hpp"
#include "maic/skeleton.hpp"

using maic::canonical_json;
using maic::kCanonical;
using maic::sha256_digest;
using maic::skeleton_hash;
using maic::skeleton_of;
using json = nlohmann::json;

int main() {
    section("canonical JSON, RFC 8785");
    expect(canonical_json(json::parse(R"({"b":1,"a":2})")) == R"({"a":2,"b":1})", "object keys are sorted");
    expect(canonical_json(json::parse("[3,1,2]")) == "[3,1,2]", "array order is kept");
    expect(canonical_json(json(1.5)) == "1.5", "a simple float");
    expect(canonical_json(json(1000000000000000000000.0)) == "1e+21", "a large float goes to exponent form at 1e21");
    expect(canonical_json(json(0)) == "0" && canonical_json(json(-0.0)) == "0", "zero has one form");
    expect(canonical_json(json("a\"\\\n")) == "\"a\\\"\\\\\\n\"", "strings escape the control set minimally");
    expect(canonical_json(json::parse("{\"\xc3\xa9\":1,\"a\":2}")) == "{\"a\":2,\"\xc3\xa9\":1}", "keys sort by code unit, ascii before non-ascii");

    section("the skeleton of a value");
    json ev = json::parse(R"({"type":"maic.notice","sequence_number":7,"stream_id":"s","text":"hi","ok":true,"n":null,"items":[{"a":1},{"a":2,"b":"x"}]})");
    json sk = skeleton_of(ev);
    expect(sk["type"] == "" && sk["sequence_number"] == 0 && sk["ok"] == false && sk["n"].is_null(), "names kept, values emptied, null stays null");
    expect(sk["items"].size() == 2, "an array keeps one skeleton per distinct element shape");
    json same = skeleton_of(json::parse(R"({"stream_id":"other","type":"maic.notice","sequence_number":999,"text":"bye","ok":false,"n":null,"items":[{"a":9}]})"));
    expect(skeleton_hash(sk) != skeleton_hash(same), "a different array shape is a different skeleton");
    json same2 = skeleton_of(json::parse(R"({"type":"x","sequence_number":1,"stream_id":"z","text":"t","ok":true,"n":null,"items":[{"a":2},{"b":"y","a":1}]})"));
    expect(skeleton_hash(sk) == skeleton_hash(same2), "two values of the same shape hash alike whatever their contents or key order");

    section("the digest form");
    std::string d = sha256_digest("");
    expect(d.rfind("sha256:", 0) == 0 && d.size() == 7 + 64, "a digest is sha256: and 64 hex");
    expect(sha256_digest("abc") == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "the known SHA-256 of abc");

    section("the protocol hash is stable and prose does not move it");
    expect(maic::protocol::protocol_hash() == maic::protocol::protocol_hash(), "the same build hashes the same");
    expect(maic::protocol::protocol_hash().rfind("sha256:", 0) == 0, "in digest form");

    section("the checker reads an unknown event type by its skeleton");
    std::vector<json> recs;
    auto ev_record = [&](const json& params) { return json{{"dir", "out"}, {"conn", "c1"}, {"msg", {{"jsonrpc", "2.0"}, {"method", "maic.event"}, {"params", params}}}}; };
    // A subscribe so the connection knows the session, then a made-up event type nobody has a schema for.
    recs.push_back({{"dir", "in"}, {"conn", "c1"}, {"msg", {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "maic.session.subscribe"}, {"params", {{"session", "s"}}}}}});
    recs.push_back({{"dir", "out"}, {"conn", "c1"}, {"msg", {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"epoch", "e1"}, {"sequence_number", 0}, {"activity", "idle"}, {"replay_from", 0}}}}}});
    json invented = {{"type", "maic.future.widget"}, {"sequence_number", 0}, {"stream_id", "s"}, {"shape", {{"k", 1}}}};
    json skline = {{"dir", "skeleton"}, {"of", "maic.future.widget"}, {"hash", skeleton_hash(skeleton_of(invented))}, {"canonical", kCanonical}, {"skeleton", skeleton_of(invented)}};

    {
        maic::protocol::Conformance c;
        std::optional<maic::protocol::Violation> v;
        for (const auto& r : recs) if (!v) v = c.feed(r);
        // The file describes itself (another shape declared), so an event with no skeleton of its own is caught.
        json other = {{"x", 1}};
        if (!v) v = c.feed(json{{"dir", "skeleton"}, {"of", "maic.other"}, {"hash", skeleton_hash(skeleton_of(other))}, {"canonical", kCanonical}, {"skeleton", skeleton_of(other)}});
        if (!v) v = c.feed(ev_record(invented));
        expect(v && v->rule == "skeleton.undeclared", "in a self-describing file, an event with no skeleton declared is skeleton.undeclared");
    }
    {
        maic::protocol::Conformance c;
        std::optional<maic::protocol::Violation> v;
        for (const auto& r : recs) if (!v) v = c.feed(r);
        if (!v) v = c.feed(skline);
        if (!v) v = c.feed(ev_record(invented));
        if (!v) v = c.finish();
        expect(!v, std::string("a skeleton declared first, the event reads by it") + (v ? ": " + maic::protocol::describe(*v) : ""));
    }
    {
        json bad = skline;
        bad["hash"] = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        maic::protocol::Conformance c;
        std::optional<maic::protocol::Violation> v;
        for (const auto& r : recs) if (!v) v = c.feed(r);
        if (!v) v = c.feed(bad);
        expect(v && v->rule == "skeleton.hash", "a skeleton that does not hash to its own hash is skeleton.hash");
    }
    {
        json must = skline;
        must["must_understand"] = true;
        maic::protocol::Conformance c;
        std::optional<maic::protocol::Violation> v;
        for (const auto& r : recs) if (!v) v = c.feed(r);
        if (!v) v = c.feed(must);
        if (!v) v = c.feed(ev_record(invented));
        expect(v && v->rule == "skeleton.must_understand", "a must_understand type this build does not know is refused");
    }

    return finish();
}
