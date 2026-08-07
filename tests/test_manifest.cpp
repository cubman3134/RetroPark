#include <doctest/doctest.h>
#include "loader/Manifest.h"

using rp::parse_manifest;
using rp::CoreManifest;

static const char* kValid = R"({
  "id":"refcore_present","name":"Reference","type":"presenting",
  "abi_version":1,"graphics_api":"d3d11","entry":"refcore_present.dll"})";

TEST_CASE("manifest: valid parses") {
    CoreManifest m; std::string err;
    CHECK(parse_manifest(kValid, m, err) == RP_OK);
    CHECK(m.id == "refcore_present");
    CHECK(m.type == RP_CORE_PRESENTING);
    CHECK(m.graphics_api == RP_GFX_D3D11);
    CHECK(m.abi_version == 1u);
    CHECK(m.entry == "refcore_present.dll");
}

TEST_CASE("manifest: missing field rejected") {
    CoreManifest m; std::string err;
    CHECK(parse_manifest(R"({"id":"x"})", m, err) == RP_ERR_BAD_ARG);
    CHECK(!err.empty());
}

TEST_CASE("manifest: unknown type rejected") {
    CoreManifest m; std::string err;
    const char* j = R"({"id":"x","name":"n","type":"driven-plus","abi_version":1,
                        "graphics_api":"d3d11","entry":"x.dll"})";
    CHECK(parse_manifest(j, m, err) == RP_ERR_BAD_ARG);
}

TEST_CASE("manifest: driven type is accepted (declared)") {
    CoreManifest m; std::string err;
    const char* j = R"({"id":"x","name":"n","type":"driven","abi_version":1,
                        "graphics_api":"d3d11","entry":"x.dll"})";
    CHECK(parse_manifest(j, m, err) == RP_OK);
    CHECK(m.type == RP_CORE_DRIVEN);
}

TEST_CASE("manifest: malformed json rejected") {
    CoreManifest m; std::string err;
    CHECK(parse_manifest("{not json", m, err) == RP_ERR_BAD_ARG);
}

TEST_CASE("manifest: graphics_api none is accepted (driven cores)") {
    CoreManifest m; std::string err;
    const char* j = R"({"id":"d","name":"n","type":"driven","abi_version":3,
                        "graphics_api":"none","entry":"d.dll"})";
    CHECK(parse_manifest(j, m, err) == RP_OK);
    CHECK(m.graphics_api == RP_GFX_NONE);
}
