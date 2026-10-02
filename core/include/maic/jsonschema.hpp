#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace maic {

// JSON Schema 2020-12, the keywords MAIC's protocol schemas and OpenAI's pinned description (protocol/openai/) use:
// type (a name or a list), enum, const, properties, required, additionalProperties, propertyNames, maxProperties,
// items, minItems, maxItems, minLength, maxLength (in code points), pattern (POSIX ERE through regcomp), minimum,
// maximum, anyOf, oneOf, allOf, $defs, and $ref to a JSON pointer in the same document; true and false are schemas.
// description, title, default, example, examples, deprecated, format, discriminator, readOnly, writeOnly, $comment,
// $schema, $id and every x- key are annotations: allowed, never checked. Null is spelled as the schemas spell it:
// "null" in a type list, an anyOf branch {"type": "null"}, or null in an enum.

// "" when `value` fits `schema`, else the first problem as "<JSON pointer into value>: <what is wrong>". `$ref`s
// resolve against `document` (the schema itself when it stands alone), so a pinned schema is checked with
// schema_error(subset, {{"$ref", "#/components/schemas/ResponseStreamEvent"}}, event). When every branch of an
// anyOf or oneOf fails, the branch that got furthest is reported, so a union discriminated by `type` names the
// problem inside the branch the value meant.
std::string schema_error(const nlohmann::json& document, const nlohmann::json& schema, const nlohmann::json& value);

// The first keyword in `schema` or a schema inside it that schema_error neither checks nor reads as an annotation,
// as "<JSON pointer into schema>: <keyword>"; "" when there is none. A schema relying on such a keyword would pass
// values it means to refuse, so the tests run every pinned schema through this.
std::string schema_unsupported(const nlohmann::json& schema);

}  // namespace maic
