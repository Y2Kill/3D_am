#include "am/storage/repository/repository.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace am;
using namespace am::repository;
int main(int argc, char** argv) {
  if(argc!=5) return 2;
  const std::string mode=argv[1], asset=argv[3], point=argv[4]; const std::filesystem::path root=argv[2];
  if(mode=="hold") {
    auto held=commit::AssetLock::acquire(root,asset); if(!held) return 3;
    {std::ofstream ready(point);ready<<"ready";if(!ready)return 4;}
    for(;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  RepositoryOptions o; o.library_id="550e8400-e29b-41d4-a716-446655440000";o.application_version="0.1.0-test";
  o.test_hooks.commit.point=[&](commit::FaultPoint p)->Result<void>{
    if(mode=="crash"&&commit::point_name(p)==point) {
#ifdef _WIN32
      TerminateProcess(GetCurrentProcess(),91);
#else
      std::_Exit(91);
#endif
    } return {};
  };
  auto repo=AssetRepository::open(root,o);if(!repo){std::cerr<<to_log_string(repo.error());return 5;}
  AssetRef ref{o.library_id,asset};auto snap=repo->snapshot(ref);if(!snap){std::cerr<<to_log_string(snap.error());return 5;}
  ChangeRequest r;r.ref=ref;r.expected_rev=snap->revision();r.ops.push_back(SetMetadata{"asset","title",Json("child")});
  auto changed=repo->change(r);if(!changed){std::cerr<<to_log_string(changed.error());return changed.error().code==Errc::locked?14:6;}return 0;
}
