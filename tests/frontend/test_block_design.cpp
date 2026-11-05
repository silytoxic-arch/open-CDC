// Vivado block designs (.bd): the module generated from the cells, nets and
// interface nets, and its analysis with the IP models.

#include "core/Diagnostic.h"
#include "core/Progress.h"
#include "engine/AnalysisSession.h"
#include "engine/ProjectConfig.h"
#include "frontend/BlockDesign.h"

#include "../TestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace opencdc;

namespace {

bool has(const std::string& v, const std::string& s) {
    return v.find(s) != std::string::npos;
}

std::shared_ptr<const engine::AnalysisResult> run(const engine::ProjectConfig& cfg) {
    engine::AnalysisSession session;
    NullProgressSink progress;
    CancellationToken cancel;
    auto r = session.run(cfg, progress, cancel);
    REQUIRE(r);
    for (const auto& d : r->diagnostics)
        UNSCOPED_INFO(format(d));
    return r;
}

const Diagnostic* find(const engine::AnalysisResult& r, std::string_view code) {
    auto it = std::ranges::find(r.diagnostics, code, &Diagnostic::code);
    return it == r.diagnostics.end() ? nullptr : &*it;
}

// the AXI4-Stream clock converter as Vivado 2020.2+ writes its .xci (JSON, with the boundary)
const char* kConverterXci = R"({
  "schema": "xilinx.com:schema:json_instance:1.0",
  "ip_inst": {
    "xci_name": "design_1_cc_0",
    "component_reference": "xilinx.com:ip:axis_clock_converter:1.1",
    "parameters": { "model_parameters": { "C_IS_ACLK_ASYNC": [ { "value": "1" } ] } },
    "boundary": {
      "ports": {
        "s_axis_aclk": [ { "direction": "in" } ],
        "s_axis_aresetn": [ { "direction": "in", "driver_value": "1" } ],
        "s_axis_tdata": [ { "direction": "in", "size_left": "7", "size_right": "0" } ],
        "s_axis_tvalid": [ { "direction": "in" } ],
        "s_axis_tready": [ { "direction": "out" } ],
        "m_axis_aclk": [ { "direction": "in" } ],
        "m_axis_aresetn": [ { "direction": "in", "driver_value": "1" } ],
        "m_axis_tdata": [ { "direction": "out", "size_left": "7", "size_right": "0" } ],
        "m_axis_tvalid": [ { "direction": "out" } ],
        "m_axis_tready": [ { "direction": "in" } ]
      },
      "interfaces": {
        "S_AXIS": { "vlnv": "xilinx.com:interface:axis:1.0", "mode": "slave", "port_maps": {
          "TDATA": [ { "physical_name": "s_axis_tdata" } ], "TVALID": [ { "physical_name": "s_axis_tvalid" } ],
          "TREADY": [ { "physical_name": "s_axis_tready" } ] } },
        "M_AXIS": { "vlnv": "xilinx.com:interface:axis:1.0", "mode": "master", "port_maps": {
          "TDATA": [ { "physical_name": "m_axis_tdata" } ], "TVALID": [ { "physical_name": "m_axis_tvalid" } ],
          "TREADY": [ { "physical_name": "m_axis_tready" } ] } }
      }
    }
  }
})";

const char* kRtl = R"(
module producer (input clk, output [7:0] m_tdata, output m_tvalid, input m_tready);
  reg [7:0] cnt = 8'd0;
  always @(posedge clk) if (m_tready) cnt <= cnt + 8'd1;
  assign m_tdata = cnt;
  assign m_tvalid = 1'b1;
endmodule
module consumer (input clk, input [7:0] s_tdata, input s_tvalid, output s_tready, output [7:0] q,
                 output [3:0] stat_o);
  reg [7:0] r = 8'd0;
  always @(posedge clk) if (s_tvalid) r <= s_tdata;
  assign s_tready = 1'b1;
  assign q = r;
  assign stat_o = r[3:0];
endmodule
)";

/// design_1: producer (clk_a) -> [cc: axis_clock_converter, when `converter`] -> grp/cons (clk_b) -> led
std::string designBd(bool converter) {
    const std::string producer = R"(
      "prod": {
        "vlnv": "xilinx.com:module_ref:producer:1.0", "xci_name": "design_1_prod_0",
        "reference_info": { "ref_type": "hdl", "ref_name": "producer" },
        "ports": { "clk": { "type": "clk", "direction": "I" } },
        "interface_ports": { "M_AXIS": { "mode": "Master", "vlnv": "xilinx.com:interface:axis_rtl:1.0", "port_maps": {
          "TDATA": { "physical_name": "m_tdata", "direction": "O", "left": "7", "right": "0" },
          "TVALID": { "physical_name": "m_tvalid", "direction": "O" },
          "TREADY": { "physical_name": "m_tready", "direction": "I" } } } }
      },)";
    const std::string cc = converter ? R"(
      "cc": { "vlnv": "xilinx.com:ip:axis_clock_converter:1.1", "xci_name": "design_1_cc_0" },)"
                                     : "";
    const std::string grp = R"(
      "grp": {
        "interface_ports": { "S_AXIS": { "mode": "Slave", "vlnv": "xilinx.com:interface:axis_rtl:1.0" },
                             "STAT": { "mode": "Master", "vlnv": "xilinx.com:interface:gpio_rtl:1.0" } },
        "ports": { "CLK": { "type": "clk", "direction": "I" }, "Q": { "direction": "O", "left": "7", "right": "0" } },
        "components": {
          "cons": {
            "vlnv": "xilinx.com:module_ref:consumer:1.0", "xci_name": "design_1_cons_0",
            "reference_info": { "ref_type": "hdl", "ref_name": "consumer" },
            "ports": { "clk": { "type": "clk", "direction": "I" },
                       "q": { "direction": "O", "left": "7", "right": "0" } },
            "interface_ports": {
              "S_AXIS": { "mode": "Slave", "vlnv": "xilinx.com:interface:axis_rtl:1.0", "port_maps": {
                "TDATA": { "physical_name": "s_tdata", "direction": "I", "left": "7", "right": "0" },
                "TVALID": { "physical_name": "s_tvalid", "direction": "I" },
                "TREADY": { "physical_name": "s_tready", "direction": "O" } } },
              "STAT": { "mode": "Master", "vlnv": "xilinx.com:interface:gpio_rtl:1.0", "port_maps": {
                "TRI_O": { "physical_name": "stat_o", "direction": "O", "left": "3", "right": "0" } } } }
          }
        },
        "interface_nets": {
          "S_AXIS_1": { "interface_ports": [ "S_AXIS", "cons/S_AXIS" ] },
          "cons_STAT": { "interface_ports": [ "STAT", "cons/STAT" ] }
        },
        "nets": {
          "CLK_1": { "ports": [ "CLK", "cons/clk" ] },
          "cons_q": { "ports": [ "cons/q", "Q" ] }
        }
      }
    },)";
    const std::string inets = converter ? R"(
      "prod_M_AXIS": { "interface_ports": [ "prod/M_AXIS", "cc/S_AXIS" ] },
      "cc_M_AXIS": { "interface_ports": [ "cc/M_AXIS", "grp/S_AXIS" ] },)"
                                        : R"(
      "prod_M_AXIS": { "interface_ports": [ "prod/M_AXIS", "grp/S_AXIS" ] },)";
    const std::string nets = converter ? R"(
      "clk_a_1": { "ports": [ "clk_a", "prod/clk", "cc/s_axis_aclk" ] },
      "clk_b_1": { "ports": [ "clk_b", "cc/m_axis_aclk", "grp/CLK" ] },)"
                                       : R"(
      "clk_a_1": { "ports": [ "clk_a", "prod/clk" ] },
      "clk_b_1": { "ports": [ "clk_b", "grp/CLK" ] },)";
    return R"({
  "design": {
    "design_info": { "name": "design_1", "tool_version": "2022.2" },
    "ports": {
      "clk_a": { "type": "clk", "direction": "I" },
      "clk_b": { "type": "clk", "direction": "I" },
      "led": { "direction": "O", "left": "7", "right": "0" }
    },
    "interface_ports": { "STAT": { "mode": "Master", "vlnv": "xilinx.com:interface:gpio_rtl:1.0" } },
    "components": {)" +
           producer + cc + grp + R"(
    "interface_nets": {)" +
           inets + R"(
      "grp_STAT": { "interface_ports": [ "STAT", "grp/STAT" ] }
    },
    "nets": {)" +
           nets + R"(
      "grp_Q": { "ports": [ "grp/Q", "led" ] }
    }
  }
})";
}

engine::ProjectConfig project(const test::TempDir& dir, bool converter) {
    const auto bd = dir.write(converter ? "bd/design_1/design_1.bd" : "bd2/design_1/design_1.bd", designBd(converter));
    if (converter)
        dir.write("bd/design_1/ip/design_1_cc_0/design_1_cc_0.xci", kConverterXci);
    engine::ProjectConfig cfg;
    cfg.name = "bd";
    cfg.top = "design_1";
    cfg.sources = {dir.write("rtl.v", kRtl), bd};
    cfg.sourceLibraries.assign(cfg.sources.size(), "");
    cfg.constraints = {dir.write("c.sdc", "create_clock -name clk_a -period 10 [get_ports clk_a]\n"
                                              "create_clock -name clk_b -period 8 [get_ports clk_b]\n"
                                              "set_clock_groups -asynchronous -group clk_a -group clk_b\n")};
    return cfg;
}

} // namespace

TEST_CASE("Block design: the module of a .bd, interface nets expanded through the port maps", "[frontend][bd]") {
    test::TempDir dir;
    (void)project(dir, true);
    auto r = frontend::blockDesignFromFile(dir.path() / "bd/design_1/design_1.bd");
    REQUIRE(r);
    const auto& b = r.value();
    const auto& v = b.verilog;
    INFO(v);
    CHECK(b.name == "design_1");
    CHECK(b.cells == 3);
    CHECK(b.hierarchies == 1);
    REQUIRE(b.ipFiles.size() == 1);
    CHECK(b.ipFiles[0].filename() == "design_1_cc_0.xci");
    for (const char* s : {"module design_1 (", "input  wire clk_a,", "output wire [7:0] led,",
                          // the design's interface port: one port per signal
                          "output wire [3:0] STAT_tri_o",
                          // the interface nets carry the names Vivado gives them
                          "wire [7:0] prod_M_AXIS_TDATA;", "wire [0:0] cc_M_AXIS_TREADY;",
                          "producer prod (", ".m_tdata(prod_M_AXIS_TDATA)", ".m_tready(prod_M_AXIS_TREADY)",
                          "design_1_cc_0 cc (", ".s_axis_aclk(clk_a)", ".m_axis_aclk(clk_b)",
                          ".s_axis_tdata(prod_M_AXIS_TDATA)", ".m_axis_tdata(cc_M_AXIS_TDATA)",
                          // unconnected inputs get their default value
                          ".s_axis_aresetn(1'd1)",
                          // the hierarchy is flattened: the instance name is the cell path
                          "consumer grp_cons (", ".clk(clk_b)", ".s_tdata(cc_M_AXIS_TDATA)", ".q(led)",
                          ".stat_o(STAT_tri_o)"})
        CHECK(has(v, s));
    // negative: nothing is driven twice or left without a driver
    CHECK(b.notes.empty());
    CHECK_FALSE(has(v, "assign"));
}

TEST_CASE("Block design: parts of a port in different interfaces, unconnected outputs", "[frontend][bd]") {
    test::TempDir dir;
    // a two-slot IP whose slots share the concatenated port (as the AXI crossbar)
    dir.write("d/ip/d_sw_0/d_sw_0.xci", R"({ "ip_inst": { "xci_name": "d_sw_0",
      "component_reference": "xilinx.com:ip:axis_switch:1.1",
      "boundary": {
        "ports": { "aclk": [ { "direction": "in" } ],
                   "s_axis_tdata": [ { "direction": "in", "size_left": "15", "size_right": "0" } ],
                   "m_axis_tdata": [ { "direction": "out", "size_left": "7", "size_right": "0" } ],
                   "m_axis_tvalid": [ { "direction": "out" } ] },
        "interfaces": {
          "S00_AXIS": { "port_maps": { "TDATA": [ { "physical_name": "s_axis_tdata", "physical_left": "7", "physical_right": "0" } ] } },
          "S01_AXIS": { "port_maps": { "TDATA": [ { "physical_name": "s_axis_tdata", "physical_left": "15", "physical_right": "8" } ] } },
          "M00_AXIS": { "port_maps": { "TDATA": [ { "physical_name": "m_axis_tdata" } ] } } } } } })");
    const std::string ref = R"("vlnv": "xilinx.com:module_ref:src:1.0", "reference_info": { "ref_name": "src" },
        "interface_ports": { "M": { "port_maps": { "TDATA": { "physical_name": "d", "direction": "O", "left": "7", "right": "0" } } } })";
    const std::string bd = R"({ "design": { "design_info": { "name": "d" },
      "ports": { "clk": { "direction": "I" } },
      "components": {
        "a": { )" + ref + R"( },
        "b": { )" + ref + R"( },
        "sw": { "vlnv": "xilinx.com:ip:axis_switch:1.1", "xci_name": "d_sw_0" } },
      "interface_nets": {
        "a_M": { "interface_ports": [ "a/M", "sw/S00_AXIS" ] },
        "b_M": { "interface_ports": [ "b/M", "sw/S01_AXIS" ] } },
      "nets": { "clk_1": { "ports": [ "clk", "sw/aclk" ] } } } })";
    auto r = frontend::blockDesignFromText(bd, dir.path() / "d");
    REQUIRE(r);
    const auto& v = r.value().verilog;
    INFO(v);
    CHECK(has(v, ".s_axis_tdata({b_M_TDATA, a_M_TDATA})"));
    CHECK(has(v, ".m_axis_tdata()"));
    CHECK(has(v, "src a ("));
    CHECK(has(v, ".d(a_M_TDATA)"));
    // negative: one slot only: the other half of the port is tied to 0
    const std::string one = R"({ "design": { "design_info": { "name": "d" }, "components": {
        "b": { )" + ref + R"( }, "sw": { "vlnv": "xilinx.com:ip:axis_switch:1.1", "xci_name": "d_sw_0" } },
      "interface_nets": { "b_M": { "interface_ports": [ "b/M", "sw/S01_AXIS" ] } } } })";
    auto r1 = frontend::blockDesignFromText(one, dir.path() / "d");
    REQUIRE(r1);
    CHECK(has(r1.value().verilog, ".s_axis_tdata({b_M_TDATA, 8'd0})"));
    CHECK(has(r1.value().verilog, ".aclk(1'd0)"));
}

TEST_CASE("Block design: errors", "[frontend][bd]") {
    test::TempDir dir;
    auto bad = frontend::blockDesignFromText("{ not json", dir.path());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "FE-BD-PARSE");
    auto noName = frontend::blockDesignFromText(R"({ "design": { "components": {} } })", dir.path());
    REQUIRE_FALSE(noName);
    CHECK(noName.error().code == "FE-BD-PARSE");
    // a cell whose ports are not known (no output products next to the .bd)
    auto missing = frontend::blockDesignFromText(
        R"({ "design": { "design_info": { "name": "d" }, "components": {
             "x": { "vlnv": "xilinx.com:ip:axi_gpio:2.0", "xci_name": "d_x_0" } } } })",
        dir.path());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "FE-BD-CELL");
    CHECK(has(missing.error().message, "'x'"));
    CHECK(frontend::isBlockDesignFile("a/b/design_1.bd"));
    CHECK_FALSE(frontend::isBlockDesignFile("a/b/design_1.bxml"));
}

TEST_CASE("Block design: analyzed with the IP models; the clock converter separates the clocks", "[frontend][bd]") {
    test::TempDir dir;
    const auto r = run(project(dir, true));
    const auto* bd = find(*r, "FE-BD-MODULE");
    REQUIRE(bd);
    CHECK(has(bd->message, "module 'design_1' with 3 cell(s), 1 hierarchical cell(s) flattened, 1 IP configuration(s)"));
    const auto* ip = find(*r, "FE-XCI-MODEL");
    REQUIRE(ip);
    CHECK(has(ip->message, "AXI4-Stream clock converter, s_axis_aclk to m_axis_aclk, asynchronous clocks"));
    CHECK_FALSE(r->hasErrors());
    REQUIRE(r->cdc);
    CHECK(r->cdc->paths.empty());
    // negative: the producer straight into the consumer: the interface net carries the crossing
    test::TempDir dir2;
    const auto r2 = run(project(dir2, false));
    REQUIRE(r2->cdc);
    REQUIRE_FALSE(r2->cdc->paths.empty());
    const bool toConsumer = std::ranges::any_of(r2->cdc->paths, [](const auto& p) {
        return p.dstName.find("grp_cons") != std::string::npos;
    });
    CHECK(toConsumer);
}
