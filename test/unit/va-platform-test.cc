// SPDX-License-Identifier: MIT
//
// The qualification in data/iris-codec-capabilities.json was taken on one
// SoC's Iris block. These pin what the driver does with a node that names
// another one, with a node that names none, and with a `compatible` list
// whose later entries name a platform the node only borrows a driver from.

#include "../../src/va/driver.h"

#include <cstring>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

va::Capabilities probed(const char* platform)
{
    va::Capabilities caps;
    caps.platform = platform;
    caps.profiles = { VAProfileH264High, VAProfileHEVCMain, VAProfileAV1Profile0 };
    caps.encode_profiles = { VAProfileH264High };
    caps.import_formats = { V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC };
    caps.output_formats = { V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC };
    caps.nv12 = true;
    return caps;
}

bool untouched(const va::Capabilities& caps)
{
    return caps.profiles.size() == 3 && caps.encode_profiles.size() == 1 && caps.import_formats.size() == 2;
}

} // namespace

int main()
{
    using namespace std::string_literals;

    check(iris::first_compatible("qcom,sm8550-iris\0"s) == "qcom,sm8550-iris", "a single entry is the identity");
    check(iris::first_compatible("qcom,x1e80100-iris\0qcom,sm8550-iris\0"s) == "qcom,x1e80100-iris",
        "a fallback entry is not the identity");
    check(iris::first_compatible("").empty(), "an unreadable property is no identity");

    check(va::platform_qualified("qcom,sm8550-iris"), "the qualified platform is recognised");
    check(!va::platform_qualified("qcom,x1e80100-iris"), "a platform with the qualified one as fallback is not");
    check(!va::platform_qualified(""), "no identity is not a qualified one");

    {
        auto caps = probed("qcom,sm8550-iris");
        check(!std::strcmp(va::apply_platform_policy(caps, false), "qualified") && untouched(caps),
            "the qualified platform advertises what it probed");
    }
    {
        auto caps = probed("qcom,sm8650-iris");
        check(!std::strcmp(va::apply_platform_policy(caps, false), "withheld"), "another platform is withheld");
        check(caps.profiles.empty() && caps.encode_profiles.empty() && caps.import_formats.empty(),
            "a withheld platform advertises no profile and no Import codec");
        check(caps.output_formats.size() == 2 && caps.nv12, "what the node enumerates is kept");
    }
    {
        auto caps = probed("qcom,sm8650-iris");
        check(!std::strcmp(va::apply_platform_policy(caps, true), "override") && untouched(caps),
            "the override advertises what the node probed");
    }
    {
        auto caps = probed("");
        check(!std::strcmp(va::apply_platform_policy(caps, false), "unidentified") && untouched(caps),
            "a node without an identity is left as probed");
    }

    if (failures)
        return 1;
    std::cout << "PASS va-platform\n";
    return 0;
}
