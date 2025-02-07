#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

namespace ptml {

namespace tags {

inline constexpr auto Div = "div";
inline constexpr auto Span = "span";

} // namespace tags

namespace attributes {

inline constexpr auto Scope = "data-scope";
inline constexpr auto Token = "data-token";
inline constexpr auto LocationDefinition = "data-location-definition";
inline constexpr auto LocationReferences = "data-location-references";
inline constexpr auto ModelEditPath = "data-model-edit-path";

} // namespace attributes

namespace scopes {} // namespace scopes

namespace tokens {

inline constexpr auto Indentation = "indentation";
inline constexpr auto Comment = "comment";

} // namespace tokens

} // namespace ptml
