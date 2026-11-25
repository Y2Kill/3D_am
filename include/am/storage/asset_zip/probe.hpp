#pragma once
#include <optional>
#include <string>
#include <functional>
#include <stop_token>
#include "am/domain/manifest/codec.hpp"
#include "am/storage/asset_zip/reader.hpp"
namespace am::asset_zip {
enum class ProbeOutcome { not_zip, invalid_zip, foreign_zip, own, own_unsupported_version, own_corrupt };
struct FormatVersion {
  // Canonical integer text, since V1 precedes V2's signedness/range checks.
  // Minor is empty if it is unavailable; full schema validation belongs to V2.
  std::string major, minor;
};
struct ProbeResult {
  ProbeOutcome outcome = ProbeOutcome::not_zip;
  std::optional<FormatVersion> version;
  std::optional<Error> error;  // Underlying reader/codec error is chained in cause.
};
struct ProbeContext {
  ReadBudget* budget = nullptr;
  std::function<Result<void>()> check_deadline;
  // Optional caller policy for actual manifest output; asset readers leave it empty.
  std::function<Result<void>(const EntryInfo&, std::uint64_t)> check_output;
  std::stop_token stop{};
  // The caller selects the input profile; no dependency on archive adapters.
  std::function<Result<ZipArchive>()> open_input;
};
Result<ProbeResult> probe_container(std::shared_ptr<const RandomAccessSource> source,
                                  const ZipLimits& limits = {}, const manifest::CodecLimits& codec_limits = {});
Result<ProbeResult> probe_container(std::shared_ptr<const RandomAccessSource> source,
                                  const ZipLimits& limits, const manifest::CodecLimits& codec_limits,
                                  const ProbeContext& context);
}  // namespace am::asset_zip
