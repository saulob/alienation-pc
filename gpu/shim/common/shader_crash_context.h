// aln: bounded shader compile state printed by every GPU assertion failure.
#pragma once
#include <string>
#include <string_view>
#include "common/types.h"

namespace Common::ShaderCrash {

void SetBuildIdentity(std::string identity);

class Scope {
public:
    Scope(std::string hw_stage, std::string sw_stage, u64 hash, u64 pipeline);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

bool Active();
void SetPass(const char* pass);
void SetDetail(std::string detail);
void Report();

} // namespace Common::ShaderCrash
