#pragma once
#include "fault_world.hpp"
#include "am/application/import_service.hpp"
#include "am/services/import/run.hpp"
#include "zip_builder.hpp"

namespace fault_test {
inline Json import_result(am::catalog::Catalog& c,const Json& fixture,am::import::RunHooks hooks={}) {
  am::jobs::HandlerRegistry registry;
  take(am::import::register_import_jobs(registry,c,"fault",std::move(hooks)));
  am::jobs::ExecutorOptions options; options.auto_retries=0; options.limits={1,1,1,1,1,1,1,1};
  am::jobs::ResourceManager resources(options.limits);
  am::jobs::Executor executor(c.job_store(),resources,registry,options);
  take(executor.run_until_idle());
  am::import::QueuedRun queued; queued.job_id=fixture.at("job_id"); queued.plan_sha256=fixture.at("plan_sha");
  return take(am::import::run_summary(c,queued));
}
inline void import_fixture(am::catalog::Catalog& c,const fs::path& work,std::string_view mode,Json& fixture) {
  am::import::ScanRequest scan;
  scan.profile=take(am::application::load_import_profile());
  const auto folder=work/"import-source";
  if (mode=="import_folder" || mode=="import_between") {
    write(folder/"one/part.stl","solid one\nendsolid one\n");
    scan.roots={folder/"one"};
    if (mode=="import_between") {
      write(folder/"two/part.stl","solid two\nendsolid two\n");
      scan.roots.push_back(folder/"two");
    }
  } else {
    zip_test::Entry part; part.name="part.stl"; part.data="solid zip\nendsolid zip\n"; part.method=8;
    auto built=zip_test::build({part});
    if (mode=="import_nested") {
      zip_test::Entry inner; inner.name="inner.zip"; inner.method=8;
      inner.data.assign(reinterpret_cast<const char*>(built.bytes.data()),built.bytes.size());
      built=zip_test::build({inner,part});
    }
    const auto path=folder/"source.zip";
    write(path,std::string_view(reinterpret_cast<const char*>(built.bytes.data()),built.bytes.size()));
    scan.roots={path};
  }
  auto plan=take(am::import::scan(scan));
  for (auto& candidate:plan.value["candidates"]) {
    candidate["action"]="import";
    for (auto& entry:candidate["entries"]) if (entry["decision"]=="ask") entry["decision"]="include";
  }
  const auto path=work/"import-plan.json";
  write(path,am::import::serialize_plan(plan));
  const auto queued=take(am::import::enqueue_run(c,path));
  fixture["job_id"]=queued.job_id;
  fixture["plan_sha"]=queued.plan_sha256;
  fixture["import_source"]=folder.generic_string();
}
}
