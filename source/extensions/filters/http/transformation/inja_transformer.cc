#include "source/extensions/filters/http/transformation/inja_transformer.h"

#include <iterator>

#include "absl/strings/str_replace.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/macros.h"
#include "source/common/common/regex.h"
#include "source/common/common/utility.h"
#include "source/common/config/metadata.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/common/random_generator.h"

#include "source/extensions/filters/http/solo_well_known_names.h"

extern char **environ;

// For convenience
using namespace inja;
using json = nlohmann::json;

using namespace std::placeholders;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Transformation {

using TransformationTemplate =
    envoy::api::v2::filter::http::TransformationTemplate;

struct BoolHeaderValues {
  const std::string trueString = "true";
  const std::string falseString = "false";
};
typedef ConstSingleton<BoolHeaderValues> BoolHeader;

// TODO: move to common
namespace {
const Http::HeaderMap::GetResult
getHeader(const Http::RequestOrResponseHeaderMap &header_map,
          const Http::LowerCaseString &key) {
  return header_map.get(key);
}

const Http::HeaderMap::GetResult
getHeader(const Http::RequestOrResponseHeaderMap &header_map,
          const std::string &key) {
  // use explicit constuctor so string is lowered
  auto lowerkey = Http::LowerCaseString(key);
  return getHeader(header_map, lowerkey);
}

} // namespace

Extractor::Extractor(const envoy::api::v2::filter::http::Extraction &extractor)
    : headername_(extractor.header()), body_(extractor.has_body()),
      group_(extractor.subgroup()),
      extract_regex_(Regex::Utility::parseStdRegex(extractor.regex())) {
  // mark count == number of sub groups, and we need to add one for match number
  // 0 so we test for < instead of <= see:
  // http://www.cplusplus.com/reference/regex/basic_regex/mark_count/
  if (extract_regex_.mark_count() < group_) {
    throw EnvoyException(
        fmt::format("group {} requested for regex with only {} sub groups",
                    group_, extract_regex_.mark_count()));
  }
}

absl::string_view
Extractor::extract(Http::StreamFilterCallbacks &callbacks,
                   const Http::RequestOrResponseHeaderMap &header_map,
                   GetBodyFunc &body) const {
  if (body_) {
    const std::string &string_body = body();
    absl::string_view sv(string_body);
    return extractValue(callbacks, sv);
  } else {
    const Http::HeaderMap::GetResult header_entries = getHeader(header_map, headername_);
    if (header_entries.empty()) {
      return "";
    }
    return extractValue(callbacks, header_entries[0]->value().getStringView());
  }
}

absl::string_view
Extractor::extractValue(Http::StreamFilterCallbacks &callbacks,
                        absl::string_view value) const {
  // get and regex
  std::match_results<absl::string_view::const_iterator> regex_result;
  if (std::regex_match(value.begin(), value.end(), regex_result,
                       extract_regex_)) {
    if (group_ >= regex_result.size()) {
      // this should never happen as we test this in the ctor.
      ASSERT("no such group in the regex");
      ENVOY_STREAM_LOG(debug, "invalid group specified for regex", callbacks);
      return "";
    }
    const auto &sub_match = regex_result[group_];
    return absl::string_view(sub_match.first, sub_match.length());
  } else {
    ENVOY_STREAM_LOG(debug, "extractor regex did not match input", callbacks);
  }
  return "";
}

TransformerInstance::TransformerInstance(
    const Http::RequestOrResponseHeaderMap &header_map,
    const Http::RequestHeaderMap *request_headers, GetBodyFunc &body,
    const std::unordered_map<std::string, absl::string_view> &extractions,
    const json &context,
    const std::unordered_map<std::string, std::string> &environ,
    const envoy::config::core::v3::Metadata *cluster_metadata,
    Envoy::Random::RandomGenerator &rng)
    : header_map_(header_map), request_headers_(request_headers), body_(body),
      extractions_(extractions), context_(context), environ_(environ),
      cluster_metadata_(cluster_metadata), rng_(rng) {
  env_.add_callback("header", 1,
                    [this](Arguments &args) { return header_callback(args); });
  env_.add_callback("request_header", 1, [this](Arguments &args) {
    return request_header_callback(args);
  });
  env_.add_callback("extraction", 1, [this](Arguments &args) {
    return extracted_callback(args);
  });
  env_.add_callback("context", 0, [this](Arguments &) { return context_; });
  env_.add_callback("body", 0, [this](Arguments &) { return body_(); });
  env_.add_callback("env", 1, [this](Arguments &args) { return env(args); });
  env_.add_callback("clusterMetadata", 1, [this](Arguments &args) {
    return cluster_metadata_callback(args);
  });
  env_.add_callback("base64_encode", 1, [this](Arguments &args) {
    return base64_encode_callback(args);
  });
  env_.add_callback("base64_decode", 1, [this](Arguments &args) {
    return base64_decode_callback(args);
  });
  // substring can be called with either two or three arguments --
  // the first argument is the string to be modified, the second is the start position
  // of the substring, and the optional third argument is the length of the substring.
  // If the third argument is not provided, the substring will extend to the end of the string.
  env_.add_callback("substring", 2, [this](Arguments &args) {
    return substring_callback(args);
  });
  env_.add_callback("substring", 3, [this](Arguments &args) {
    return substring_callback(args);
  });
  env_.add_callback("replace_with_random", 2, [this](Arguments &args) {
    return replace_with_random_callback(args);
  });
}

json TransformerInstance::header_callback(const inja::Arguments &args) const {
  const std::string &headername = args.at(0)->get_ref<const std::string &>();
  const Http::HeaderMap::GetResult header_entries = getHeader(header_map_, headername);
  if (header_entries.empty()) {
    return "";
  }
  return std::string(header_entries[0]->value().getStringView());
}

json TransformerInstance::request_header_callback(
    const inja::Arguments &args) const {
  if (request_headers_ == nullptr) {
    return "";
  }
  const std::string &headername = args.at(0)->get_ref<const std::string &>();
  const Http::HeaderMap::GetResult header_entries =
      getHeader(*request_headers_, headername);
  if (header_entries.empty()) {
    return "";
  }
  return std::string(header_entries[0]->value().getStringView());
}

json TransformerInstance::extracted_callback(
    const inja::Arguments &args) const {
  const std::string &name = args.at(0)->get_ref<const std::string &>();
  const auto value_it = extractions_.find(name);
  if (value_it != extractions_.end()) {
    return value_it->second;
  }
  return "";
}

json TransformerInstance::env(const inja::Arguments &args) const {
  const std::string &key = args.at(0)->get_ref<const std::string &>();
  auto it = environ_.find(key);
  if (it != environ_.end()) {
    return it->second;
  }
  return "";
}

json TransformerInstance::cluster_metadata_callback(
    const inja::Arguments &args) const {
  const std::string &key = args.at(0)->get_ref<const std::string &>();

  if (!cluster_metadata_) {
    return "";
  }

  const ProtobufWkt::Value &value = Envoy::Config::Metadata::metadataValue(
      cluster_metadata_, SoloHttpFilterNames::get().Transformation, key);

  switch (value.kind_case()) {
  case ProtobufWkt::Value::kStringValue: {
    return value.string_value();
    break;
  }
  case ProtobufWkt::Value::kNumberValue: {
    return value.number_value();
    break;
  }
  case ProtobufWkt::Value::kBoolValue: {
    const std::string &stringval = value.bool_value()
                                       ? BoolHeader::get().trueString
                                       : BoolHeader::get().falseString;
    return stringval;
    break;
  }
  case ProtobufWkt::Value::kListValue: {
    const auto &listval = value.list_value().values();
    if (listval.size() == 0) {
      break;
    }

    // size is not zero, so this will work
    auto it = listval.begin();
    std::stringstream ss;

    auto addValue = [&ss, &it] {
      const ProtobufWkt::Value &value = *it;

      switch (value.kind_case()) {
      case ProtobufWkt::Value::kStringValue: {
        ss << value.string_value();
        break;
      }
      case ProtobufWkt::Value::kNumberValue: {
        ss << value.number_value();
        break;
      }
      case ProtobufWkt::Value::kBoolValue: {
        ss << (value.bool_value() ? BoolHeader::get().trueString
                                  : BoolHeader::get().falseString);
        break;
      }
      default:
        break;
      }
    };

    addValue();

    for (it++; it != listval.end(); it++) {
      ss << ",";
      addValue();
    }
    return ss.str();
  }
  default: {
    break;
  }
  }
  return "";
}

json TransformerInstance::base64_encode_callback(const inja::Arguments &args) const {
  const std::string &input = args.at(0)->get_ref<const std::string &>();
  return Base64::encode(input.c_str(), input.length());
}

json TransformerInstance::base64_decode_callback(const inja::Arguments &args) const {
  const std::string &input = args.at(0)->get_ref<const std::string &>();
  return Base64::decode(input);
}

// return a substring of the input string, starting at the start position
// and extending for length characters. If length is not provided, the
// substring will extend to the end of the string.
json TransformerInstance::substring_callback(const inja::Arguments &args) const {
  // get first argument, which is the string to be modified
  const std::string &input = args.at(0)->get_ref<const std::string &>();

  // try to get second argument (start position) as an int64_t
  int start = 0;
  try {
    start = args.at(1)->get_ref<const int64_t &>();
  } catch (const std::exception &e) {
    // if it can't be converted to an int64_t, return an empty string
    return "";
  }

  // try to get optional third argument (length) as an int64_t
  int64_t substring_len = -1;
  if (args.size() == 3) {
    try {
      substring_len = args.at(2)->get_ref<const int64_t &>();
    } catch (const std::exception &e) {
      // if it can't be converted to an int64_t, return an empty string
      return "";
    }
  }
  const int64_t input_len = input.length();

  // if start is negative, or start is greater than the length of the string, return empty string
  if (start < 0 ||  start >= input_len) {
    return "";
  }

  // if supplied substring_len is <= 0 or start + substring_len is greater than the length of the string,
  // return the substring from start to the end of the string
  if (substring_len <= 0 || start + substring_len > input_len) {
    return input.substr(start);
  }

  // otherwise, return the substring from start to start + len
  return input.substr(start, substring_len);
}

json TransformerInstance::replace_with_random_callback(const inja::Arguments &args) {
    // first argument: string to modify
  const std::string &source = args.at(0)->get_ref<const std::string &>();
    // second argument: pattern to be replaced
  const std::string &to_replace = args.at(1)->get_ref<const std::string &>();

  return
    absl::StrReplaceAll(source, {{to_replace, absl::StrCat(random_for_pattern(to_replace))}});
}

std::string& TransformerInstance::random_for_pattern(const std::string& pattern) {
  auto found = pattern_replacements_.find(pattern);
  if (found == pattern_replacements_.end()) {
    // generate 128 bit long random number
    uint64_t random[2];
    uint64_t high = rng_.random();
    uint64_t low = rng_.random();
    random[0] = low;
    random[1] = high;
    // and convert it to a base64-encoded string with no padding
    pattern_replacements_.insert({pattern, Base64::encode(reinterpret_cast<char *>(random), 16, false)});
    return pattern_replacements_[pattern];
  }
  return found->second;
}

std::string TransformerInstance::render(const inja::Template &input) {
  // inja can't handle context that are not objects correctly, so we give it an
  // empty object in that case
  if (context_.is_object()) {
    return env_.render(input, context_);
  } else {
    return env_.render(input, {});
  }
}

inja::Template TransformerInstance::parse(std::string_view input) {
  return env_.parse(input);
}

// empty_transformer_instance provides a TransformerInstance that can be used to validate parsing
// of templates. because there is no request data, the returned TransformerInstance
// cannot be used for rendering.
TransformerInstance TransformerInstance::empty_transformer_instance() {
  auto emptyRequestHeaderMap = Http::RequestHeaderMapImpl::create();
  auto header_map = emptyRequestHeaderMap.get();

  GetBodyFunc get_body = []() -> const std::string & {
      const std::string ret("");
      return std::move(ret);
  };

  std::unordered_map<std::string, absl::string_view> extractions;
  json json_body;
  std::unordered_map<std::string, std::string> empty_environ;
  const envoy::config::core::v3::Metadata *cluster_metadata{};
  Random::RandomGeneratorImpl rng;

  TransformerInstance instance(*header_map, header_map, get_body,
          extractions, json_body, empty_environ,
          cluster_metadata, rng);

  return instance;
}

InjaTransformer::InjaTransformer(const TransformationTemplate &transformation, Envoy::Random::RandomGenerator &rng, google::protobuf::BoolValue log_request_response_info)
    : Transformer(log_request_response_info),
      transformation_(transformation),
      rng_(rng) {
  // first we validate the templates parse so we can catch invalid templates at config time
  validate_templates();

  // parse environment
  for (char **env = environ; *env != 0; env++) {
    std::string current_env(*env);
    size_t equals = current_env.find("=");
    if (equals > 0) {
      std::string key = current_env.substr(0, equals);
      std::string value = current_env.substr(equals + 1);
      environ_[key] = value;
    }
  }

  switch (transformation_.body_transformation_case()) {
  case TransformationTemplate::kBody: {
    // this case is handled in transform method
    break;
  }
  case TransformationTemplate::kMergeExtractorsToBody: {
    merged_extractors_to_body_ = true;
    break;
  }
  case TransformationTemplate::kPassthrough:
    break;
  case TransformationTemplate::BODY_TRANSFORMATION_NOT_SET: {
    break;
  }
}
}

InjaTransformer::~InjaTransformer() {}

// validate_templates provides a way to error early if we encounter parser errors
// while processing user-provided templates. This requires the use of an TransformerInstance
// because we will encounter unwanted parser errors if we do not have the callbacks defined.
void InjaTransformer::validate_templates() {
  auto instance = TransformerInstance::empty_transformer_instance();
  if (transformation_.advanced_templates()) {
      instance.set_element_notation(inja::ElementNotation::Pointer);
  }
  const auto &headers = transformation_.headers();
  for (auto it = headers.begin(); it != headers.end(); it++) {
    Http::LowerCaseString header_name(it->first);
    try {
      instance.parse(it->second.text());
    } catch (const std::exception &e) {
      throw EnvoyException(fmt::format(
          "Failed to parse header template '{}': {}", it->first, e.what()));
    }
  }
  const auto &headers_to_append = transformation_.headers_to_append();
  for (auto idx = 0; idx < transformation_.headers_to_append_size(); idx++) {
    const auto &it = headers_to_append.Get(idx);
    try {
      instance.parse(it.value().text());
    } catch (const std::exception &e) {
      throw EnvoyException(fmt::format(
          "Failed to parse header template '{}': {}", it.key(), e.what()));
    }
  }
  const auto &dynamic_metadata_values =
      transformation_.dynamic_metadata_values();
  for (auto it = dynamic_metadata_values.begin();
       it != dynamic_metadata_values.end(); it++) {
    try {
      instance.parse(it->value().text());
    } catch (const std::exception &e) {
      throw EnvoyException(fmt::format(
          "Failed to parse dynamic metadata template '{}': {}", it->key(), e.what()));
    }
  }

  switch (transformation_.body_transformation_case()) {
  case TransformationTemplate::kBody: {
    try {
      instance.parse(transformation_.body().text());
    } catch (const std::exception &e) {
      throw EnvoyException(
          fmt::format("Failed to parse body template {}", e.what()));
    }
    break;
  }
  default: break;
  }

}
void InjaTransformer::transform(Http::RequestOrResponseHeaderMap &header_map,
                                Http::RequestHeaderMap *request_headers,
                                Buffer::Instance &body,
                                Http::StreamFilterCallbacks &callbacks) const {
  absl::optional<std::string> string_body;
  GetBodyFunc get_body = [&string_body, &body]() -> const std::string & {
    if (!string_body.has_value()) {
      string_body.emplace(body.toString());
    }
    return string_body.value();
  };

  json json_body;

  if (transformation_.parse_body_behavior() != TransformationTemplate::DontParse &&
      body.length() > 0) {
    const std::string &bodystring = get_body();
    // parse the body as json
    // TODO: gate this under a parse_body boolean
    if (transformation_.parse_body_behavior() == TransformationTemplate::ParseAsJson) {
      if (transformation_.ignore_error_on_parse()) {
        try {
          json_body = json::parse(bodystring);
        } catch (const std::exception &) {
        }
      } else {
        json_body = json::parse(bodystring);
      }
    } else {
      ASSERT("missing behavior");
    }
  }
  // get the extractions
  std::vector<std::pair<std::string, Extractor>> extractor_pairs;
  std::unordered_map<std::string, absl::string_view> extractions;

  const auto &extractors = transformation_.extractors();
  for (auto it = extractors.begin(); it != extractors.end(); it++) {
    extractor_pairs.emplace_back(std::make_pair(it->first, it->second));
  }
  if (transformation_.advanced_templates()) {
    extractions.reserve(extractor_pairs.size());
  }

  for (const auto &named_extractor : extractor_pairs) {
    const std::string &name = named_extractor.first;
    if (transformation_.advanced_templates()) {
      extractions[name] =
          named_extractor.second.extract(callbacks, header_map, get_body);
    } else {
      absl::string_view name_to_split = name;
      json *current = &json_body;
      for (size_t pos = name_to_split.find("."); pos != std::string::npos;
           pos = name_to_split.find(".")) {
        auto &&field_name = name_to_split.substr(0, pos);
        current = &(*current)[std::string(field_name)];
        name_to_split = name_to_split.substr(pos + 1);
      }
      (*current)[std::string(name_to_split)] =
          named_extractor.second.extract(callbacks, header_map, get_body);
    }
  }

  // get cluster metadata
  const envoy::config::core::v3::Metadata *cluster_metadata{};
  Upstream::ClusterInfoConstSharedPtr ci = callbacks.clusterInfo();
  if (ci.get()) {
    cluster_metadata = &ci->metadata();
  }

  // start transforming!
  TransformerInstance instance(header_map, request_headers, get_body,
                               extractions, json_body, environ_,
                               cluster_metadata, rng_);
  if (transformation_.advanced_templates()) {
      instance.set_element_notation(inja::ElementNotation::Pointer);
  }

  std::vector<std::pair<Http::LowerCaseString, inja::Template>> headers_templates;
  const auto &headers = transformation_.headers();
  for (auto it = headers.begin(); it != headers.end(); it++) {
    Http::LowerCaseString header_name(it->first);
    try {
      headers_templates.emplace_back(std::make_pair(std::move(header_name),
                                           instance.parse(it->second.text())));
    } catch (const std::exception &e) {
      throw EnvoyException(fmt::format(
          "Failed to parse header template '{}': {}", it->first, e.what()));
    }
  }

  std::vector<Http::LowerCaseString> headers_to_remove_templates;
  const auto &headers_to_remove = transformation_.headers_to_remove();
  for (auto idx : headers_to_remove) {
    Http::LowerCaseString header_name(idx);
    headers_to_remove_templates.push_back(header_name);
  }

  std::vector<std::pair<Http::LowerCaseString, inja::Template>> headers_to_append_templates;
  const auto &headers_to_append = transformation_.headers_to_append();
  for (auto idx = 0; idx < transformation_.headers_to_append_size(); idx++) {
    const auto &it = headers_to_append.Get(idx);
    Http::LowerCaseString header_name(it.key());
    try {
      headers_to_append_templates.emplace_back(std::make_pair(std::move(header_name),
                                           instance.parse(it.value().text())));
    } catch (const std::exception &e) {
      throw EnvoyException(fmt::format(
          "Failed to parse header template '{}': {}", it.key(), e.what()));
    }
  }

  std::vector<DynamicMetadataValue> dynamic_metadata_templates;
  const auto &dynamic_metadata_values = transformation_.dynamic_metadata_values();
  for (auto it = dynamic_metadata_values.begin();
       it != dynamic_metadata_values.end(); it++) {
    try {
      DynamicMetadataValue dynamicMetadataValue;
      dynamicMetadataValue.namespace_ = it->metadata_namespace();
      if (dynamicMetadataValue.namespace_.empty()) {
        dynamicMetadataValue.namespace_ =
            SoloHttpFilterNames::get().Transformation;
      }
      dynamicMetadataValue.key_ = it->key();
      dynamicMetadataValue.template_ = instance.parse(it->value().text());
      dynamic_metadata_templates.emplace_back(std::move(dynamicMetadataValue));
    } catch (const std::exception &e) {
      throw EnvoyException(fmt::format(
          "Failed to parse header template '{}': {}", it->key(), e.what()));
    }
  }

  absl::optional<inja::Template> body_template;
  switch (transformation_.body_transformation_case()) {
  case TransformationTemplate::kBody: {
    try {
      body_template.emplace(instance.parse(transformation_.body().text()));
    } catch (const std::exception &e) {
      throw EnvoyException(
          fmt::format("Failed to parse body template {}", e.what()));
    }
    break;
  }
  default:
    // the other cases are handled in the constructor
    break;
  }


  // Body transform:
  absl::optional<Buffer::OwnedImpl> maybe_body;

  if (body_template.has_value()) {
    std::string output = instance.render(body_template.value());
    maybe_body.emplace(output);
  } else if (merged_extractors_to_body_) {
    std::string output = json_body.dump();
    maybe_body.emplace(output);
  }

  // DynamicMetadata transform:
  for (const auto &templated_dynamic_metadata : dynamic_metadata_templates) {
    std::string output = instance.render(templated_dynamic_metadata.template_);
    if (!output.empty()) {
      ProtobufWkt::Struct strct(
          MessageUtil::keyValueStruct(templated_dynamic_metadata.key_, output));
      callbacks.streamInfo().setDynamicMetadata(
          templated_dynamic_metadata.namespace_, strct);
    }
  }

  // Headers transform:
  for (const auto &templated_header : headers_templates) {
    std::string output = instance.render(templated_header.second);
    // remove existing header
    header_map.remove(templated_header.first);
    // TODO(yuval-k): Do we need to support intentional empty headers?
    if (!output.empty()) {
      // we cannot add the key as reference as the headers_templates
      // is in local scope and has shorter lifetime than life of request
      header_map.addCopy(templated_header.first, output);
    }
  }

  for (const auto &header_to_remove : headers_to_remove_templates) {
    header_map.remove(header_to_remove);
  }

  // Headers to Append Values transform:
  for (const auto &templated_header : headers_to_append_templates) {
    std::string output = instance.render(templated_header.second);
    if (!output.empty()) {
      // we cannot add the key as reference as the headers_to_append_templates
      // is in local scope and has shorter lifetime than life of request
      header_map.addCopy(templated_header.first, output);
    }
  }

  // replace body. we do it here so that headers and dynamic metadata have the
  // original body.
  if (maybe_body.has_value()) {
    // remove content length, as we have new body.
    header_map.removeContentLength();
    // replace body
    body.drain(body.length());
    // prepend is used because it doesn't copy, it drains maybe_body
    body.prepend(maybe_body.value());
    header_map.setContentLength(body.length());
  }
}

} // namespace Transformation
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
