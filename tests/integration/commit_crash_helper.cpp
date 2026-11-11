#include "am/storage/commit/protocol.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace am;
using namespace am::commit;
namespace {
Result<void> produce(asset_zip::ZipWriter& writer) {
  const std::string manifest = R"({"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}},"title":"new"})";
  asset_zip::EntryOptions o; o.method = asset_zip::CompressionMethod::stored; o.uncompressed_size = manifest.size();
  auto m = writer.begin_entry("manifest.json", o); if (!m) return m.error(); auto mw = m->write(std::as_bytes(std::span(manifest))); if (!mw) return mw;
  auto mf = m->finish(); if (!mf) return mf.error();
  o.uncompressed_size = 4096; auto body = writer.begin_entry("content/body", o); if (!body) return body.error(); const std::string half(2048, 'n');
  auto a = body->write(std::as_bytes(std::span(half))); if (!a) return a; a = body->write(std::as_bytes(std::span(half))); if (!a) return a;
  auto finished = body->finish(); if (!finished) return finished.error(); return {};
}
}
int main(int argc, char** argv) {
  if (argc < 5) return 2;
  const std::string mode = argv[1]; const auto root = std::filesystem::path(argv[2]); const std::string asset = argv[3];
  if (mode == "hold") {
    auto lock = AssetLock::acquire(root, asset); if (!lock) return 3;
    { std::ofstream ready(argv[4], std::ios::binary); ready << "ready"; if (!ready) return 4; }
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (argc != 6) return 2;
  const std::string op = argv[4], point = argv[5]; asset_zip::ReadBudget budget(1000000); CommitRequest request(budget);
  request.library_root = root; request.target_path = root / "asset.zip"; request.asset_id = asset; request.op_id = op; request.estimated_size = 6000;
  auto base = inspect_zip(request.target_path, budget); if (!base) { std::cerr << base.error().ctx.detail; return 5; } request.base = *base;
  request.producer = produce; request.hooks.mid_write_after_bytes = 2250;
  request.hooks.point = [&](FaultPoint current) -> Result<void> {
    if (point_name(current) == point) {
#ifdef _WIN32
      TerminateProcess(GetCurrentProcess(), 91);
#else
      std::_Exit(91);
#endif
    }
    return {};
  };
  auto result = commit_zip(request); if (!result) { std::cerr << result.error().message_id << ": " << result.error().ctx.detail; return 6; } return 0;
}
