#pragma once
#include <optional>
#include <string>
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
Result<ProbeResult> probe_container(std::shared_ptr<const RandomAccessSource> source,
                                  const ZipLimits& limits = {}, const manifest::CodecLimits& codec_limits = {});
}  // namespace am::asset_zip
