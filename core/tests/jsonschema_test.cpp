// The JSON Schema validator (maic/jsonschema.hpp): every keyword it implements with known answers, then OpenAI's
// pinned subset (protocol/openai/subset.json) against a Responses stream shaped as the engine will send it and a
// llama-server b11284 chat-completions stream (fixtures, written by hand from the spec and llama.cpp's source), and
// the OpenAI-compatible client's adapter rules on the shapes llama.cpp sends that the schema refuses.
#include "check.hpp"

#include "maic/jsonschema.hpp"
#include "maic/llm.hpp"

#include <fstream>
#include <sstream>

using namespace maic;
using nlohmann::json;

namespace {

json load(const std::string& path) {
    std::ifstream in(path);
    return json::parse(in);
}

// Every `data:` line of a server-sent event stream but [DONE], as the client reads them.
std::vector<json> sse_data(const std::string& path) {
    std::vector<json> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("data: ", 0) == 0 && line != "data: [DONE]") out.push_back(json::parse(line.substr(6)));
    }
    return out;
}

std::vector<json> jsonl(const std::string& path) {
    std::vector<json> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) out.push_back(json::parse(line));
    }
    return out;
}

// Checks `value` against `schema` standing alone and expects `want` ("" for a pass).
void known(const json& schema, const json& value, const std::string& want, const std::string& what) {
    std::string got = schema_error(schema, schema, value);
    expect(got == want, what + (got == want ? "" : ": got \"" + got + "\", want \"" + want + "\""));
}

}  // namespace

int main() {
    section("keywords, known answers");
    {
        known({{"type", "string"}}, "x", "", "type: a string is a string");
        known({{"type", "string"}}, 3, "(root): must be string, not integer", "type: an integer is not a string");
        known({{"type", "integer"}}, 2.0, "", "type: a whole float is an integer");
        known({{"type", "integer"}}, 2.5, "(root): must be integer, not number", "type: a fraction is not an integer");
        known({{"type", "number"}}, 7, "", "type: an integer is a number");
        known({{"type", {"string", "null"}}}, nullptr, "", "type list: null where null is listed");
        known({{"type", {"string", "null"}}}, true, "(root): must be string or null, not boolean", "type list: refused with every name");
        known({{"enum", {"stop", "length", nullptr}}}, nullptr, "", "enum: null as a member");
        known({{"enum", {"stop", "length"}}}, "steered", "(root): must be one of \"stop\", \"length\", not \"steered\"", "enum: a value outside the list");
        known({{"enum", json::array({1, 2, 3, 4, 5, 6, 7, 8, 9})}}, 0, "(root): must be one of 9 allowed values, not 0", "enum: a long list is counted, not printed");
        known({{"const", "response"}}, "response", "", "const: equal");
        known({{"const", "response"}}, "responses", "(root): must be \"response\"", "const: different");
        known({{"type", "string"}, {"minLength", 2}, {"maxLength", 3}}, "\xc3\xa9t\xc3\xa9", "", "minLength and maxLength count code points, not bytes");
        known({{"type", "string"}, {"maxLength", 2}}, "abc", "(root): is longer than 2 characters", "maxLength");
        known({{"type", "string"}, {"minLength", 1}}, "", "(root): is shorter than 1 characters", "minLength");
        known({{"pattern", "^tunnel_[a-z0-9]{4}$"}}, "tunnel_ab12", "", "pattern: matches");
        known({{"pattern", "^[a-zA-Z0-9_-]+$"}}, "read file", "(root): does not match the pattern \"^[a-zA-Z0-9_-]+$\"", "pattern: refused");
        known({{"pattern", "^ab"}}, 5, "", "pattern applies to strings only");
        known({{"minimum", 0}, {"maximum", 2}}, 2, "", "maximum is inclusive");
        known({{"minimum", 0}, {"maximum", 2}}, 2.5, "(root): is more than the maximum 2", "maximum");
        known({{"minimum", 0}}, -1, "(root): is less than the minimum 0", "minimum");
        known({{"items", {{"type", "integer"}}}, {"minItems", 1}, {"maxItems", 2}}, {1, 2}, "", "items, minItems, maxItems");
        known({{"items", {{"type", "integer"}}}}, {1, "x"}, "/1: must be integer, not string", "items: the failing index is named");
        known({{"minItems", 1}}, json::array(), "(root): has fewer than 1 items", "minItems");
        known({{"maxItems", 1}}, {1, 2}, "(root): has more than 1 items", "maxItems");
        json obj = {{"type", "object"}, {"properties", {{"a", {{"type", "string"}}}, {"b/c", {{"type", "integer"}}}}}, {"required", {"a"}}};
        known(obj, {{"a", "x"}, {"other", 1}}, "", "properties: extra properties pass without additionalProperties");
        known(obj, {{"b/c", 1}}, "/a: is required", "required: the missing property is named");
        known(obj, {{"a", "x"}, {"b/c", "1"}}, "/b~1c: must be integer, not string", "properties: the pointer escapes / as ~1");
        json closed = obj;
        closed["additionalProperties"] = false;
        known(closed, {{"a", "x"}, {"z", 1}}, "/z: is not allowed (additionalProperties is false)", "additionalProperties false");
        known({{"additionalProperties", {{"type", "string"}}}}, {{"k", "v"}, {"n", 1}}, "/n: must be string, not integer", "additionalProperties as a schema (a map)");
        known({{"propertyNames", {{"maxLength", 2}}}, {"maxProperties", 2}}, {{"ab", 1}}, "", "propertyNames and maxProperties pass");
        known({{"propertyNames", {{"maxLength", 2}}}}, {{"abc", 1}}, "/abc: the name is longer than 2 characters", "propertyNames");
        known({{"maxProperties", 1}}, {{"a", 1}, {"b", 2}}, "(root): has more than 1 properties", "maxProperties");
        known(true, 42, "", "the schema true takes anything");
        known(false, 42, "(root): no value is allowed here", "the schema false takes nothing");
        known({{"items", true}}, {1, "x", nullptr}, "", "items: true");
        known({{"allOf", {{{"type", "object"}}, {{"required", {"id"}}}}}}, {{"id", 1}}, "", "allOf: all hold");
        known({{"allOf", {{{"type", "object"}}, {{"required", {"id"}}}}}}, json::object(), "/id: is required", "allOf: one fails");
        json nullable = {{"anyOf", {{{"type", "string"}}, {{"type", "null"}}}}};
        known(nullable, nullptr, "", "anyOf with {type: null}: OpenAI's nullable");
        known(nullable, 1, "(root): must be string, not integer", "anyOf: every branch fails");
        known({{"oneOf", {{{"type", "string"}}, {{"type", "integer"}}}}}, 1, "", "oneOf: exactly one");
        known({{"oneOf", {{{"type", "number"}}, {{"type", "integer"}}}}}, 1, "(root): matches 2 branches of oneOf (#0 and #1); exactly one must match", "oneOf: two match");
        known({{"description", "x"}, {"format", "unixtime"}, {"discriminator", {{"propertyName", "type"}}}, {"x-stainless-const", true}, {"default", 1}},
              "anything", "", "annotations are not checked");
    }

    section("$ref, $defs and unions");
    {
        json doc = {{"$defs", {{"name", {{"type", "string"}}}, {"loop", {{"$ref", "#/$defs/loop"}}}}},
                    {"type", "object"}, {"properties", {{"n", {{"$ref", "#/$defs/name"}}}}}};
        known(doc, {{"n", "x"}}, "", "$ref into $defs");
        known(doc, {{"n", 1}}, "/n: must be string, not integer", "$ref: the referenced schema applies");
        expect(schema_error(doc, {{"$ref", "#/$defs/nothing"}}, 1) == "(root): $ref \"#/$defs/nothing\" names nothing in the document", "$ref to nothing fails, naming it");
        expect(schema_error(doc, {{"$ref", "#/$defs/loop"}}, 1).find("loops without reaching a schema") != std::string::npos, "a $ref loop fails instead of recursing forever");
        json sibling = {{"$defs", {{"obj", {{"type", "object"}}}}}, {"$ref", "#/$defs/obj"}, {"required", {"id"}}};
        known(sibling, json::object(), "/id: is required", "keywords beside $ref apply too (2020-12)");
        // A union discriminated by `type`, as ResponseStreamEvent is: the problem reported is the one inside the
        // branch the value's type picked, not the type mismatch of every other branch.
        json events = {{"$defs",
                        {{"a", {{"type", "object"}, {"properties", {{"type", {{"enum", {"a"}}}}, {"n", {{"type", "integer"}}}}}, {"required", {"type", "n"}}}},
                         {"b", {{"type", "object"}, {"properties", {{"type", {{"enum", {"b"}}}}, {"s", {{"type", "string"}}}}}, {"required", {"type", "s"}}}}}},
                       {"anyOf", {{{"$ref", "#/$defs/a"}}, {{"$ref", "#/$defs/b"}}}}};
        known(events, {{"type", "b"}, {"s", "x"}}, "", "union: the second branch");
        known(events, {{"type", "b"}}, "/s: is required", "union: a missing field is reported from the branch `type` picked");
        known(events, {{"type", "b"}, {"s", 1}}, "/s: must be string, not integer", "union: a wrong field likewise");
        known(events, {{"type", "c"}}, "/type: must be one of \"a\", not \"c\"", "union: an unknown type is a type problem");
    }

    section("the keyword check");
    {
        expect(schema_unsupported({{"type", "object"}, {"properties", {{"a", {{"if", true}}}}}}) == "/properties/a: if", "an unsupported keyword is found inside properties");
        expect(schema_unsupported({{"anyOf", {{{"type", "string"}}, {{"nullable", true}}}}}) == "/anyOf/1: nullable", "OpenAPI 3.0's nullable is not a 2020-12 keyword");
        expect(schema_unsupported({{"items", {{"uniqueItems", true}}}}) == "/items: uniqueItems", "inside items");
        expect(schema_unsupported({{"items", json::array({true})}}) == "/items: not a schema", "the draft-4 tuple form of items is refused");
        expect(schema_unsupported({{"x-anything", {{"if", 1}}}, {"description", "d"}, {"type", "string"}}).empty(), "x- keys and annotations are skipped whole");
    }

    const std::string protocol = MAIC_PROTOCOL;
    const std::string fixtures = MAIC_FIXTURES;
    json subset = load(protocol + "/openai/subset.json");
    auto against = [&](const std::string& name, const json& value) { return schema_error(subset, {{"$ref", "#/components/schemas/" + name}}, value); };

    section("the pinned OpenAI subset");
    {
        expect(subset["x-maic-subset"]["commit"] == "de3a025c40f84b99d1401ee1c5fe69fbf8de789b", "subset.json is extracted at the pinned commit");
        expect(subset["info"]["version"] == "2.3.0" && subset["openapi"] == "3.1.0", "API version 2.3.0, OpenAPI 3.1.0");
        std::string first;
        for (const auto& [name, schema] : subset["components"]["schemas"].items()) {
            if (std::string e = schema_unsupported(schema); !e.empty() && first.empty()) first = name + e;
        }
        expect(first.empty(), "every pinned schema uses only keywords the validator implements" + (first.empty() ? "" : ": " + first));
        expect(subset["components"]["schemas"]["ResponseStreamEvent"]["anyOf"].size() == 59, "ResponseStreamEvent has its 59 members");
    }

    section("a Responses stream, shaped as the engine will send it");
    {
        auto events = jsonl(fixtures + "/responses-stream.jsonl");
        expect(events.size() == 17, "the fixture has its 17 events");
        for (const auto& e : events) {
            std::string err = against("ResponseStreamEvent", e);
            expect(err.empty(), e["type"].get<std::string>() + " #" + std::to_string(e["sequence_number"].get<int>()) + " fits ResponseStreamEvent" + (err.empty() ? "" : ": " + err));
        }
        json e = events[5];  // response.output_text.delta
        e.erase("sequence_number");
        expect(against("ResponseStreamEvent", e) == "/sequence_number: is required", "an event without sequence_number is refused, naming it");
        e = events[5];
        e["delta"] = 4;
        expect(against("ResponseStreamEvent", e) == "/delta: must be string, not integer", "a delta that is not text is refused");
        e = events[0];
        e["response"]["status"] = "denied";
        expect(against("ResponseStreamEvent", e).rfind("/response/status: must be one of", 0) == 0, "a closed enum is not extended: status \"denied\" is refused (it goes in maic.status)");
        e = events[0];
        e["type"] = "maic.review.started";
        expect(!against("ResponseStreamEvent", e).empty(), "a maic.* event is not an OpenAI event");
        e = events[14];  // the function call's output_item.done
        e["item"].erase("call_id");
        expect(against("ResponseStreamEvent", e) == "/item/call_id: is required", "a function_call item without call_id is refused, inside the union of unions");
        expect(against("ResponseUsage", events[15]["response"]["usage"]).empty(), "usage on the completed response fits ResponseUsage");
        expect(against("ResponseUsage", {{"input_tokens", 1}, {"output_tokens", 2}, {"total_tokens", 3}}) == "/input_tokens_details: is required",
               "ResponseUsage needs both details objects");
    }

    section("llama-server b11284's chat-completions stream");
    {
        auto chunks = sse_data(fixtures + "/llamacpp-b11284-chat.sse");
        expect(chunks.size() == 11, "the fixture has its 11 chunks");
        for (size_t i = 0; i < chunks.size(); ++i) {
            std::string err = against("CreateChatCompletionStreamResponse", chunks[i]);
            expect(err.empty(), "chunk " + std::to_string(i) + " fits CreateChatCompletionStreamResponse" + (err.empty() ? "" : ": " + err));
        }
        expect(chunks.front()["choices"][0]["delta"]["content"].is_null() && chunks.back()["choices"].empty() && chunks.back().contains("timings"),
               "the fixture covers the opening null content, the empty-choices usage chunk and llama.cpp's timings");
        // Shapes the schema refuses: what MAIC's client tolerates and what llama.cpp sends that OpenAI does not define.
        expect(against("CreateChatCompletionStreamResponse", {{"choices", {{{"delta", {{"content", "ok"}}}}}}}) == "/choices/0/finish_reason: is required",
               "a bare {choices:[{delta}]} chunk is not OpenAI's shape (the client accepts it; llm_test's fakes send it)");
        json usage = chunks.back();
        usage["usage"].erase("total_tokens");
        expect(against("CreateChatCompletionStreamResponse", usage) == "/usage/total_tokens: is required", "usage without total_tokens is refused");
        expect(against("ErrorResponse", {{"error", {{"code", nullptr}, {"message", "m"}, {"param", nullptr}, {"type", "server_error"}}}}).empty(), "OpenAI's own error shape fits");
        for (size_t i = 0; i < chunks.size(); ++i) {
            json n = chunks[i];
            expect(normalize_openai(n, "llamacpp").empty() && n == chunks[i], "chunk " + std::to_string(i) + ": no adapter rule touches a chunk that already fits");
        }
    }

    section("the adapter normalizations (normalize_openai) on llama-server b11284's oddities");
    {
        // Each case: the shape as llama.cpp sends it fails OpenAI's schema with `raw_error`; normalized, it fits, and
        // exactly `rules` applied. One case per rule alone and one with both error rules, so each is necessary and
        // together they are sufficient.
        auto normalized_case = [&](const json& raw, const std::vector<std::string>& rules, const std::string& raw_error, const std::string& what) {
            const char* schema = raw.contains("error") ? "ErrorResponse" : "CreateChatCompletionStreamResponse";
            json n = raw;
            std::vector<std::string> applied = normalize_openai(n, "llamacpp");
            std::string before = against(schema, raw), after = against(schema, n);
            expect(before == raw_error, what + ": as sent, refused (" + before + ")");
            expect(applied == rules && after.empty(), what + ": normalized, it fits" + (after.empty() ? "" : ": " + after));
            return n;
        };
        auto odd = sse_data(fixtures + "/llamacpp-b11284-oddities.sse");
        expect(odd.size() == 3, "the fixture has its opening chunk, a logprobs chunk and a mid-stream error");
        json first = odd[0];
        expect(normalize_openai(first, "llamacpp").empty() && against("CreateChatCompletionStreamResponse", first).empty(), "the opening chunk needs no rule");

        json lp = normalized_case(odd[1], {"logprobs_refusal_null"}, "/choices/0/logprobs/refusal: is required", "logprobs without refusal");
        expect(lp["choices"][0]["logprobs"]["refusal"].is_null() && lp["choices"][0]["logprobs"]["content"] == odd[1]["choices"][0]["logprobs"]["content"],
               "logprobs_refusal_null adds refusal: null and leaves the tokens as they were");

        json err = normalized_case(odd[2], {"error_code_string", "error_param_null"}, "/error/code: must be string, not integer", "llama.cpp's error");
        json want = {{"error", {{"code", "maic_llamacpp_500"}, {"message", "the model crashed"}, {"type", "server_error"}, {"param", nullptr},
                                {"maic", {{"upstream", {{"provider", "llamacpp"}, {"code", 500}, {"rules", {"error_code_string", "error_param_null"}}}}}}}}};
        expect(err == want, "the error keeps its original code and the rules' names under maic.upstream: " + err.dump());

        json code_only = odd[2];
        code_only["error"]["param"] = nullptr;
        normalized_case(code_only, {"error_code_string"}, "/error/code: must be string, not integer", "an integer code alone");
        json param_only = odd[2];
        param_only["error"]["code"] = "server_error";
        normalized_case(param_only, {"error_param_null"}, "/error/param: is required", "a missing param alone");
    }

    return finish();
}
