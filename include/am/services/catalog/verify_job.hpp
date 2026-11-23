#pragma once
#include "am/jobs/executor.hpp"
namespace am::catalog {
class Catalog;
Result<void> register_verify_job(jobs::HandlerRegistry&, Catalog&, std::string device,
                                 std::function<void(Error)> warning = {});
}  // namespace am::catalog
